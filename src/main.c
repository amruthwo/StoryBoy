#include <stdio.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include "platform.h"
#include "filebrowser.h"
#include "browser.h"
#include "cover.h"
#include "history.h"
#include "theme.h"
#include "decoder.h"
#include "audio.h"
#include "player.h"
#include "resume.h"
#include "overlay.h"
#include "statusbar.h"
#ifdef SB_A30
#include "a30_screen.h"
#endif
#ifdef SB_TRIMUI_BRICK
#include "brick_screen.h"
#endif
#if defined(SB_A30)
#define SB_HW 1
#include <spawn.h>
#include <sys/wait.h>
#include <sys/resource.h>
extern char **environ;
#elif defined(SB_TRIMUI_BRICK) || defined(SB_TRIMUI_SMART)
#define SB_HW 1
#endif

#define FPS_CAP 60
#define SCREENSAVER_TIMEOUT_MS (2 * 60 * 1000)  /* 2 minutes of inactivity */

/* -------------------------------------------------------------------------
 * Background embedded-cover extraction (all SB_A30 armhf builds)
 *
 * Inline extraction during library_scan is disabled on all armhf builds: on
 * SpruceOS it risks OOM; on OnionOS the slow V2/V3 CPU makes scanning take
 * 30+ seconds.  Instead we spawn extract_cover after the scan completes so
 * the browser appears immediately and covers arrive in the background.
 * ---------------------------------------------------------------------- */
#if defined(SB_A30)
static void spawn_cover_extraction(const MediaLibrary *lib) {
    int n = 0;
    for (int i = 0; i < lib->folder_count; i++) {
        const MediaFolder *f = &lib->folders[i];
        if (f->is_series) {
            for (int s = 0; s < f->season_count; s++)
                if (!f->seasons[s].cover) n++;
        } else {
            if (!f->cover) n++;
        }
    }
    if (n == 0) return;

    const char *bin = "/mnt/SDCARD/App/StoryBoy/bin32/extract_cover";
    char **args = malloc((size_t)(n + 2) * sizeof(char *));
    if (!args) return;

    int argc = 0;
    args[argc++] = (char *)bin;
    for (int i = 0; i < lib->folder_count; i++) {
        const MediaFolder *f = &lib->folders[i];
        if (f->is_series) {
            for (int s = 0; s < f->season_count; s++)
                if (!f->seasons[s].cover) args[argc++] = (char *)f->seasons[s].path;
        } else {
            if (!f->cover) args[argc++] = (char *)f->path;
        }
    }
    args[argc] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2,
        "/mnt/SDCARD/App/StoryBoy/extract_cover.log",
        O_WRONLY | O_CREAT | O_APPEND, 0644);
    pid_t pid;
    posix_spawn(&pid, bin, &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    free(args);
    /* Fire and forget — extract_cover writes cover.jpg files in the background */
}
#endif

/* Sleep timer presets: 0 = off, then 10/30/60/120 minutes */
static const int SLEEP_PRESETS_SEC[] = { 0, 30, 10*60, 30*60, 60*60, 120*60 };
#define SLEEP_PRESET_COUNT 6
static inline int sc(int base, int w) { return (int)(base * w / 640.0f + 0.5f); }

static void fill_rounded_rect_main(SDL_Renderer *r,
                                   int x, int y, int w, int h,
                                   int rad,
                                   Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    if (rad <= 0 || rad * 2 >= w || rad * 2 >= h) {
        if (rad * 2 >= h) rad = h / 2;
        if (rad <= 0) {
            SDL_Rect rect = {x, y, w, h};
            SDL_RenderFillRect(r, &rect);
            if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
            return;
        }
    }
    SDL_Rect c = {x + rad, y, w - 2 * rad, h};
    SDL_RenderFillRect(r, &c);
    SDL_Rect left  = {x,           y + rad, rad, h - 2 * rad};
    SDL_Rect right = {x + w - rad, y + rad, rad, h - 2 * rad};
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);
    for (int dy = 0; dy < rad; dy++) {
        int dist = rad - dy;
        int span = (int)sqrtf((float)(rad * rad - dist * dist));
        SDL_RenderDrawLine(r, x + rad - span,     y + dy,
                              x + rad - 1,         y + dy);
        SDL_RenderDrawLine(r, x + w - rad,         y + dy,
                              x + w - rad + span - 1, y + dy);
        SDL_RenderDrawLine(r, x + rad - span,     y + h - 1 - dy,
                              x + rad - 1,         y + h - 1 - dy);
        SDL_RenderDrawLine(r, x + w - rad,         y + h - 1 - dy,
                              x + w - rad + span - 1, y + h - 1 - dy);
    }
    if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

#define Y_DOUBLETAP_MS 350  /* max ms between two Y presses to count as double-tap */

/* -------------------------------------------------------------------------
 * SIGTERM / SIGINT handler
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_quit = 0;
static void handle_sig(int sig) { (void)sig; g_quit = 1; }
static int g_sleep_on_exit = 0;  /* set to 1 to suspend device after cleanup */

typedef enum {
    MODE_BROWSER,
    MODE_HISTORY,
    MODE_RESUME_PROMPT,
    MODE_FETCH_ART_PROMPT,
    MODE_UPNEXT,
    MODE_PLAYBACK,
} AppMode;

/* Navigate browser to the folder that contains file_path.
   Non-series folders: sets VIEW_FOLDERS so exiting playback returns to the
   folder grid (not the mp3 file list).  Series folders: sets VIEW_FILES.
   Returns the file index within the folder (used by do_play for play_file_idx). */
static int navigate_to_file(BrowserState *state, const MediaLibrary *lib,
                              const char *file_path) {
    char dir[1024];
    strncpy(dir, file_path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    for (int fi = 0; fi < lib->folder_count; fi++) {
        const MediaFolder *mf = &lib->folders[fi];
        if (!mf->is_series) {
            if (strcmp(mf->path, dir) == 0) {
                state->folder_idx = fi;
                state->season_idx = -1;
                /* Return to folder grid on exit — don't navigate into mp3 list */
                state->view       = VIEW_FOLDERS;
                state->selected   = fi;
                state->scroll_row = 0;
                /* Find and return the file index within this folder */
                for (int i = 0; i < mf->file_count; i++) {
                    if (strcmp(mf->files[i].path, file_path) == 0)
                        return i;
                }
                return 0;
            }
        } else {
            for (int si = 0; si < mf->season_count; si++) {
                if (strcmp(mf->seasons[si].path, dir) == 0) {
                    state->folder_idx = fi;
                    state->season_idx = si;
                    /* Single-file seasons are played directly from VIEW_SEASONS;
                       return there on exit so B lands on the season grid, not
                       the file list the user never saw. */
                    if (mf->seasons[si].file_count == 1) {
                        state->view          = VIEW_SEASONS;
                        state->season_scroll = si / 4; /* approximate scroll */
                    } else {
                        state->view       = VIEW_FILES;
                    }
                    state->scroll_row = 0;
                    state->selected   = si;
                    for (int i = 0; i < mf->seasons[si].file_count; i++) {
                        if (strcmp(mf->seasons[si].files[i].path, file_path) == 0) {
                            state->selected = i;
                            return i;
                        }
                    }
                    return 0;
                }
            }
        }
    }
    state->view = VIEW_FOLDERS;
    return 0;
}

/* Helper: get current file list and count based on folder/season context. */
static const AudioFile *get_file_list(const MediaLibrary *lib,
                                       int folder_idx, int season_idx,
                                       int *out_count) {
    if (folder_idx < 0 || folder_idx >= lib->folder_count) {
        *out_count = 0; return NULL;
    }
    const MediaFolder *fol = &lib->folders[folder_idx];
    if (fol->is_series && season_idx >= 0 && season_idx < fol->season_count) {
        *out_count = fol->seasons[season_idx].file_count;
        return fol->seasons[season_idx].files;
    }
    *out_count = fol->file_count;
    return fol->files;
}

/* Save resume progress — uses book API for multi-file books, file API for single. */
static void do_resume_save(const Player *player,
                           const MediaLibrary *lib,
                           int folder_idx, int file_idx, int season_idx,
                           double book_offset, double book_total_dur) {
    int fcount;
    get_file_list(lib, folder_idx, season_idx, &fcount);
    if (fcount > 1) {
        /* Multi-file book */
        char folder[1024];
        snprintf(folder, sizeof(folder), "%s", player->path);
        char *sl = strrchr(folder, '/');
        if (sl) *sl = '\0';
        double pos = audio_get_clock(&player->audio);
        resume_save_book(folder, file_idx, pos,
                         book_total_dur, book_offset + pos);
    } else {
        resume_save(player->path,
                    audio_get_clock(&player->audio),
                    player->probe.duration_sec);
    }
}

/* Probe all files in a list and return the sum of their durations.
   Returns 0.0 if probing fails or file_count == 0. */
static double compute_book_total_dur(const AudioFile *files, int file_count) {
    double total = 0.0;
    for (int i = 0; i < file_count; i++) {
        ProbeInfo pi = {0};
        if (decoder_probe(files[i].path, &pi, NULL, 0) == 0)
            total += pi.duration_sec;
    }
#ifdef SB_A30
    /* Return pages freed during probing back to the OS before player_open
       needs a large contiguous allocation. */
    malloc_trim(0);
#endif
    return total;
}

/* Fill tick_fracs with the normalised position of each file boundary.
   tick_fracs[i] = cumulative_dur[0..i] / total_dur for i = 0 .. file_count-2.
   Returns the number of ticks written. */
static int compute_book_ticks(const AudioFile *files, int file_count,
                               double total_dur,
                               float *tick_fracs, int max_ticks) {
    if (file_count <= 1 || total_dur <= 0.0 || !tick_fracs || max_ticks <= 0)
        return 0;
    int n = 0;
    double cumul = 0.0;
    for (int i = 0; i < file_count - 1 && n < max_ticks; i++) {
        ProbeInfo pi = {0};
        if (decoder_probe(files[i].path, &pi, NULL, 0) == 0)
            cumul += pi.duration_sec;
        float frac = (float)(cumul / total_dur);
        if (frac > 0.0f && frac < 1.0f)
            tick_fracs[n++] = frac;
    }
    return n;
}

/* Cross-file aware seek for multi-file books.
   When seeking backwards past the start of the current file, reopens the
   previous file and seeks to the right position.  Falls through to a plain
   player_seek for forward seeks and for single-file playback. */
static void do_book_seek(Player *player, double delta,
                         SDL_Renderer *renderer,
                         BrowserState *state, const MediaLibrary *lib,
                         int *play_file_idx, int *play_season_idx,
                         double *book_offset, Uint32 *last_resume_save) {
    if (player->state == PLAYER_STOPPED) return;

    int fcount;
    const AudioFile *files = get_file_list(lib,
        state->folder_idx, *play_season_idx, &fcount);

    if (fcount <= 1 || delta >= 0) {
        /* Single file or forward seek — delegate normally */
        player_seek(player, delta);
        return;
    }

    double cur = audio_get_clock(&player->audio);
    double new_file_pos = cur + delta; /* delta < 0 */

    if (new_file_pos >= 0) {
        player_seek(player, delta);
        return;
    }

    /* new_file_pos < 0: need to cross into a previous file */
    double remaining = new_file_pos; /* negative "overshoot" */
    int idx = *play_file_idx;

    while (remaining < 0 && idx > 0) {
        idx--;
#ifdef SB_A30
        /* On memory-constrained devices avoid decoder_probe while the current
           player's demux context is still open — two concurrent FFmpeg contexts
           exhaust RAM.  Jump to the start of the previous file instead. */
        remaining = 0;
        break;
#else
        ProbeInfo pi = {0};
        if (decoder_probe(files[idx].path, &pi, NULL, 0) != 0) {
            remaining = 0;
            break;
        }
        *book_offset -= pi.duration_sec;
        remaining += pi.duration_sec;
#endif
    }
    if (remaining < 0) remaining = 0;

    char errbuf[256] = {0};
    player_close(player);
    if (player_open(player, files[idx].path, renderer, errbuf, sizeof(errbuf)) == 0) {
        player_play(player);
        navigate_to_file(state, lib, files[idx].path);
        *play_file_idx    = idx;
        *last_resume_save = SDL_GetTicks();
        player_seek_to(player, remaining);
        player_show_osd(player);
    }
}

/* Open + start playback of a file. Returns 0 on success. */
static int do_play(Player *player, const char *path, SDL_Renderer *renderer,
                   BrowserState *state, const MediaLibrary *lib,
                   int *play_folder_idx, int *play_file_idx, int *play_season_idx,
                   Uint32 *last_resume_save,
                   char *errbuf, int errsz) {
    if (player_open(player, path, renderer, errbuf, errsz) != 0)
        return -1;
    player_play(player);
    int file_idx      = navigate_to_file(state, lib, path);
    *play_folder_idx  = state->folder_idx;
    *play_file_idx    = file_idx;
    *play_season_idx  = state->season_idx;
    *last_resume_save = SDL_GetTicks();
    return 0;
}

int main(int argc, char *argv[]) {
    platform_init_from_env();

    int win_w = g_display_w;
    int win_h = g_display_h;
#ifndef SB_HW
    if (argc == 3) { win_w = atoi(argv[1]); win_h = atoi(argv[2]); }
    else (void)argv;
    (void)argc;
#else
    (void)argc; (void)argv;
#endif

    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);
#ifdef SB_HW
    signal(SIGCHLD, SIG_IGN);
#endif

    printf("StoryBoy — platform: %s  %dx%d\n",
           platform_name(get_platform()), win_w, win_h);

    config_load("storyboy.conf");
    {
        char api_dir[768];
        snprintf(api_dir, sizeof(api_dir), "%s/resources/api",
                 g_app_dir[0] ? g_app_dir : ".");
        config_load_api_keys(api_dir);
    }

    /* SB_A30: display goes directly to fb0 via a30_screen_init — SDL video
       subsystem is never used.  Initialising it would prevent mmiyoo SDL2
       (MiyooMini V2/V3) from loading, since that build has no dummy driver. */
#ifdef SB_A30
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
#endif
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); SDL_Quit(); return 1;
    }
    if (IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG) == 0) {
        fprintf(stderr, "IMG_Init: %s\n", IMG_GetError());
        TTF_Quit(); SDL_Quit(); return 1;
    }

#if !defined(SB_A30) && !defined(SB_TRIMUI_BRICK)
    SDL_Window   *win      = NULL;
#endif
    SDL_Renderer *renderer = NULL;
#if defined(SB_A30) || defined(SB_TRIMUI_BRICK)
    SDL_Surface *hw_surf = NULL;
#endif
#ifdef SB_A30
    if (a30_screen_init() != 0) {
        fprintf(stderr, "a30_screen_init failed\n");
        IMG_Quit(); TTF_Quit(); SDL_Quit(); return 1;
    }
    hw_surf = SDL_CreateRGBSurface(0, win_w, win_h, 32,
                                0x00FF0000u, 0x0000FF00u,
                                0x000000FFu, 0xFF000000u);
    if (!hw_surf) {
        fprintf(stderr, "SDL_CreateRGBSurface: %s\n", SDL_GetError());
        a30_screen_close(); IMG_Quit(); TTF_Quit(); SDL_Quit(); return 1;
    }
    renderer = SDL_CreateSoftwareRenderer(hw_surf);
#elif defined(SB_TRIMUI_BRICK)
    if (brick_screen_init() != 0) {
        fprintf(stderr, "brick_screen_init failed\n");
        IMG_Quit(); TTF_Quit(); SDL_Quit(); return 1;
    }
    hw_surf = SDL_CreateRGBSurface(0, win_w, win_h, 32,
                                0x00FF0000u, 0x0000FF00u,
                                0x000000FFu, 0xFF000000u);
    if (!hw_surf) {
        fprintf(stderr, "SDL_CreateRGBSurface: %s\n", SDL_GetError());
        brick_screen_close(); IMG_Quit(); TTF_Quit(); SDL_Quit(); return 1;
    }
    renderer = SDL_CreateSoftwareRenderer(hw_surf);
#elif defined(SB_HW)
    /* 64-bit devices: standard SDL window + accelerated renderer */
    win = SDL_CreateWindow("StoryBoy",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_FULLSCREEN_DESKTOP);
    renderer = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#else
    win = SDL_CreateWindow("StoryBoy",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif

    float ui_scale = win_w / 640.0f;
    { float hs = win_h / 480.0f; if (hs < ui_scale) ui_scale = hs; }
    int font_sz  = (int)(18.0f * ui_scale + 0.5f);
    int font_ssz = (int)(14.0f * ui_scale + 0.5f);
    TTF_Font *font       = TTF_OpenFont("resources/fonts/DejaVuSans.ttf", font_sz);
    TTF_Font *font_small = TTF_OpenFont("resources/fonts/DejaVuSans.ttf", font_ssz);

    if (!renderer || !font || !font_small) {
        fprintf(stderr, "Init error: %s / %s\n", SDL_GetError(), TTF_GetError());
        goto cleanup;
    }
#if !defined(SB_A30) && !defined(SB_TRIMUI_BRICK)
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        goto cleanup;
    }
#endif

    SDL_DisableScreenSaver();

    SDL_Texture *default_cover = theme_render_cover(renderer,
                                                     "resources/default_cover.svg");
    if (!default_cover)
        default_cover = IMG_LoadTexture(renderer, "resources/default_cover.png");

    SDL_Texture *default_folder = theme_render_folder_cover(renderer,
                                                             "resources/default_folder.svg");

    /* Splash screen — visible during media library scan and any first-run setup */
    {
        SDL_Texture *splash_icon = IMG_LoadTexture(renderer, "resources/icon.png");
        SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xff);
        SDL_RenderClear(renderer);
        int icon_size = (int)(80.0f * ui_scale + 0.5f);
        int icon_y    = win_h / 2 - icon_size / 2 - (int)(20.0f * ui_scale);
        if (splash_icon) {
            SDL_Rect dst = { win_w/2 - icon_size/2, icon_y, icon_size, icon_size };
            SDL_RenderCopy(renderer, splash_icon, NULL, &dst);
            SDL_DestroyTexture(splash_icon);
        }
        SDL_Color white = {0xff, 0xff, 0xff, 0xff};
        SDL_Surface *tsurf = TTF_RenderUTF8_Blended(font, "StoryBoy loading...", white);
        if (tsurf) {
            SDL_Texture *ttex = SDL_CreateTextureFromSurface(renderer, tsurf);
            if (ttex) {
                SDL_Rect dst = { win_w/2 - tsurf->w/2,
                                 icon_y + icon_size + (int)(14.0f * ui_scale),
                                 tsurf->w, tsurf->h };
                SDL_RenderCopy(renderer, ttex, NULL, &dst);
                SDL_DestroyTexture(ttex);
            }
            SDL_FreeSurface(tsurf);
        }
        SDL_RenderPresent(renderer);
#ifdef SB_A30
        a30_flip(hw_surf);
#elif defined(SB_TRIMUI_BRICK)
        brick_flip(hw_surf);
#endif
    }

    printf("Scanning media library...\n");
    MediaLibrary lib;
    library_scan(&lib);
    printf("Found %d folder(s).\n", lib.folder_count);
#if defined(SB_A30)
    spawn_cover_extraction(&lib);

    /* Pre-open the most recently played file behind the splash screen.
       The SD card is fully dedicated to the open, so the browser appears
       instantly when done rather than being sluggish. */
    {
        ResumeEntry *entries = NULL;
        int n = resume_load_all(&entries);
        if (n > 0 && entries[0].path[0]) {
            const char *preopen_path = entries[0].path;
            /* Multi-file book: resolve folder + file_idx → audio file path */
            if (entries[0].file_idx >= 0) {
                for (int i = 0; i < lib.folder_count; i++) {
                    MediaFolder *f = &lib.folders[i];
                    if (strcmp(f->path, entries[0].path) == 0 &&
                        entries[0].file_idx < f->file_count) {
                        preopen_path = f->files[entries[0].file_idx].path;
                        break;
                    }
                }
            }
            demux_preopen_start(preopen_path);

            /* Update splash text and wait for the open to finish */
            {
                SDL_Color white = {0xff, 0xff, 0xff, 0xff};
                SDL_Texture *splash_icon = IMG_LoadTexture(renderer, "resources/icon.png");
                int icon_size = (int)(80.0f * ui_scale + 0.5f);
                int icon_y    = win_h / 2 - icon_size / 2 - (int)(20.0f * ui_scale);
                int text_y    = icon_y + icon_size + (int)(14.0f * ui_scale);

                while (!demux_preopen_is_done()) {
                    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xff);
                    SDL_RenderClear(renderer);
                    if (splash_icon) {
                        SDL_Rect dst = { win_w/2 - icon_size/2, icon_y,
                                         icon_size, icon_size };
                        SDL_RenderCopy(renderer, splash_icon, NULL, &dst);
                    }
                    SDL_Surface *tsurf = TTF_RenderUTF8_Blended(
                        font, "StoryBoy is loading...", white);
                    if (tsurf) {
                        SDL_Texture *ttex = SDL_CreateTextureFromSurface(renderer, tsurf);
                        if (ttex) {
                            SDL_Rect dst = { win_w/2 - tsurf->w/2, text_y,
                                             tsurf->w, tsurf->h };
                            SDL_RenderCopy(renderer, ttex, NULL, &dst);
                            SDL_DestroyTexture(ttex);
                        }
                        SDL_FreeSurface(tsurf);
                    }
                    SDL_RenderPresent(renderer);
#ifdef SB_A30
                    a30_flip(hw_surf);
#endif
                    SDL_Delay(100);
                }
                if (splash_icon) SDL_DestroyTexture(splash_icon);
            }
        }
        free(entries);
    }
#endif

    BrowserState state;
    CoverCache   cache;
    browser_init(&state, &cache, &lib);
    browser_generate_pending_mosaics(renderer, &lib, default_cover);
    state.layout        = (BrowserLayout)config_get_layout();
    state.season_layout = (BrowserLayout)config_get_season_layout();

    HistoryState history;
    memset(&history, 0, sizeof(history));

    AppMode  mode              = MODE_BROWSER;
    Player   player;
    memset(&player, 0, sizeof(player));
    Uint32   last_resume_save  = 0;
    int      play_folder_idx   = 0;
    int      play_file_idx     = 0;
    int      play_season_idx   = -1;
    int      sleep_preset_idx  = 0;  /* index into SLEEP_PRESETS_MIN */
    int      screensaver_on    = 0;  /* manually toggled screensaver */
    Uint32   last_y_press_ms   = 0;  /* for Y double-tap detection */
    Uint32   scrn_hint_hide_at = 0;  /* show "double-tap Y to wake" hint until this tick */

    /* Multi-file book progress tracking */
    double   book_offset       = 0.0; /* sum of durations of files before current */
    double   book_total_dur    = 0.0; /* total book duration (all files) */
    float    book_tick_fracs[64];     /* fraction positions of file boundaries [0..1] */
    int      book_n_ticks      = 0;   /* number of valid entries in book_tick_fracs */

    /* Resume-prompt state */
    char   resume_path[1024] = {0};
    double resume_pos        = 0.0;
    int    resume_folder_idx = 0;
    /* Book-resume extras (multi-file) */
    int    resume_book_file_idx = 0;   /* file index to open on resume */
    double resume_book_offset   = 0.0; /* book_offset to restore */

    /* Fetch-art prompt state */
    int    fetch_art_folder_idx = -1;
    int    fetch_art_season_idx = -1; /* -1 = folder-level; ≥0 = season index */

    /* Up-Next state */
    char   upnext_path[1024]  = {0};
    int    upnext_folder_idx  = 0;
    int    upnext_file_idx    = 0;
    Uint32 upnext_start       = 0;
#define UPNEXT_DELAY_MS 5000

    int  error_active     = 0;
    char error_path[1024] = {0};
    char error_msg[256]   = {0};

    int          overlay_active = 0;
    SDL_Texture *help_cache     = NULL;

    Uint32        menu_down_at = 0;
    TutorialState tutorial     = { .active = !config_firstrun_done(), .slide = 0 };

#ifdef SB_HW
    Uint32 wake_prev_frame = 0;
#endif

    int    running   = 1;
    long   frame_ns  = 1000000000L / FPS_CAP;

    while (running) {
        if (g_quit) {
            if (player.state != PLAYER_STOPPED)
                do_resume_save(&player, &lib,
                               play_folder_idx, play_file_idx, play_season_idx,
                               book_offset, book_total_dur);
            running = 0;
            break;
        }

        Uint32 frame_start = SDL_GetTicks();
        struct timespec ts_frame_start;
        clock_gettime(CLOCK_MONOTONIC, &ts_frame_start);

#ifdef SB_HW
        /* Sleep/wake detection */
        if (mode == MODE_PLAYBACK && player.state != PLAYER_STOPPED) {
            if (wake_prev_frame > 0 &&
                    frame_start - wake_prev_frame > 2000) {
                fprintf(stderr, "sleep/wake: gap %ums\n",
                        frame_start - wake_prev_frame);
                double wake_pos = audio_get_clock(&player.audio);
                audio_wake(&player.audio);
#ifdef SB_A30
                a30_screen_wake();
#endif
                player_set_volume(&player, player.volume);
                player_seek_to(&player, wake_pos);
                SDL_Delay(400);
                player_show_wake_toast(&player);
                if (player.audio.dev)
                    atomic_store(&player.audio.wake_silence_until,
                                 SDL_GetTicks() + 500);
                if (player.state == PLAYER_PLAYING && player.audio.dev)
                    SDL_PauseAudioDevice(player.audio.dev, 0);
                player_show_osd(&player);
                wake_prev_frame = SDL_GetTicks();
            } else {
                wake_prev_frame = frame_start;
            }
        } else {
            wake_prev_frame = 0;
        }

#ifdef SB_A30
        a30_poll_events();
#elif defined(SB_TRIMUI_BRICK)
        brick_poll_events();
#endif
#endif /* SB_HW */

        /* ---------------------------------------------------------------
         * Event handling
         * ------------------------------------------------------------- */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }

            if (tutorial.active) {
                if (ev.type == SDL_KEYDOWN) {
                    if (!tutorial_next(&tutorial)) {
                        config_set_firstrun_done();
                        config_save("storyboy.conf");
                    }
                }
                continue;
            }

            if (overlay_active) {
                if (ev.type == SDL_KEYDOWN) {
                    overlay_active = 0;
                    if (help_cache) { SDL_DestroyTexture(help_cache); help_cache = NULL; }
                }
                continue;
            }

            if (error_active) {
                if (ev.type == SDL_KEYDOWN) error_active = 0;
                continue;
            }

            /* MENU hold → help overlay */
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE
                    && !ev.key.repeat) {
                menu_down_at = SDL_GetTicks();
                continue;
            }
            if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_ESCAPE) {
                Uint32 held = menu_down_at ? SDL_GetTicks() - menu_down_at : 0;
                menu_down_at = 0;
                if (held >= 1000) {
                    overlay_active = 1;
                } else {
                    if (mode == MODE_BROWSER) {
                        state.action = BROWSER_ACTION_QUIT;
                    } else if (mode == MODE_HISTORY) {
                        history.action = HISTORY_ACTION_BACK;
                    } else if (mode == MODE_RESUME_PROMPT || mode == MODE_UPNEXT) {
                        mode = MODE_BROWSER;
                    } else { /* MODE_PLAYBACK */
                        resume_save(player.path,
                                    audio_get_clock(&player.audio),
                                    player.probe.duration_sec);
                        player_close(&player);
                        state.prog_folder_idx = -1;
                        state.prog_season_idx = -1;
                        mode = MODE_BROWSER;
                    }
                }
                continue;
            }

            /* Screensaver dismiss — any key resets activity */
            if (mode == MODE_PLAYBACK && ev.type == SDL_KEYDOWN) {
                player_reset_activity(&player);
            }

            if (mode == MODE_HISTORY) {
                history_handle_event(&history, &ev);
            } else if (mode == MODE_BROWSER) {
                /* X — open history */
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_LALT) {
                    history_load(&history);
                    mode = MODE_HISTORY;
                    goto browser_event_done;
                }

                /* R1 — cycle theme */
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_PAGEDOWN &&
                    !ev.key.repeat) {
                    theme_cycle();
                    config_save("storyboy.conf");
                    if (default_cover) { SDL_DestroyTexture(default_cover); default_cover = NULL; }
                    default_cover = theme_render_cover(renderer, "resources/default_cover.svg");
                    if (!default_cover)
                        default_cover = IMG_LoadTexture(renderer, "resources/default_cover.png");
                    if (default_folder) { SDL_DestroyTexture(default_folder); default_folder = NULL; }
                    default_folder = theme_render_folder_cover(renderer,
                                                               "resources/default_folder.svg");
                    goto browser_event_done;
                }

                browser_handle_event(&state, &lib, &ev);

                if (state.action == BROWSER_ACTION_LAYOUT_CHANGED) {
                    config_set_layout((int)state.layout);
                    config_set_season_layout((int)state.season_layout);
                    config_save("storyboy.conf");
                    state.action = BROWSER_ACTION_NONE;
                }

                if (state.action == BROWSER_ACTION_PLAY) {
                    char errbuf[256] = {0};

                    /* Determine if this is a multi-file book */
                    navigate_to_file(&state, &lib, state.action_path);
                    int sel_fcount;
                    const AudioFile *sel_files = get_file_list(&lib,
                        state.folder_idx, state.season_idx, &sel_fcount);

                    /* Get book directory */
                    char sel_folder[1024];
                    snprintf(sel_folder, sizeof(sel_folder), "%s", state.action_path);
                    { char *sl = strrchr(sel_folder, '/'); if (sl) *sl = '\0'; }

                    int    book_fi  = 0;
                    double book_sv  = -1.0;
                    double book_td  = 0.0;
                    double book_bp  = 0.0;

                    if (sel_fcount > 1 &&
                        resume_load_book(sel_folder, &book_fi, &book_sv,
                                         &book_td, &book_bp) &&
                        book_bp > 5.0) {
                        /* Book resume found */
                        const char *resume_file = (book_fi < sel_fcount)
                                                  ? sel_files[book_fi].path
                                                  : sel_files[0].path;
                        strncpy(resume_path, resume_file, sizeof(resume_path) - 1);
                        resume_pos          = book_sv;
                        resume_folder_idx   = state.folder_idx;
                        (void)book_fi;
                        resume_book_file_idx = book_fi;
                        resume_book_offset  = book_bp - book_sv;
                        /* Use book_td from stored entry; probe all files if missing */
                        book_total_dur = (book_td > 0.0)
                            ? book_td
                            : compute_book_total_dur(sel_files, sel_fcount);
                        book_n_ticks = compute_book_ticks(sel_files, sel_fcount,
                            book_total_dur, book_tick_fracs, 64);
                        mode = MODE_RESUME_PROMPT;
                    } else {
                        /* Single-file or no book resume — check per-file resume */
                        double sv = resume_load(state.action_path);
                        if (sv > 5.0 && sel_fcount <= 1) {
                            strncpy(resume_path, state.action_path,
                                    sizeof(resume_path) - 1);
                            resume_pos        = sv;
                            resume_folder_idx = state.folder_idx;
                            resume_book_file_idx = 0;
                            resume_book_offset   = 0.0;
                            book_total_dur       = 0.0;
                            book_offset          = 0.0;
                            book_n_ticks         = 0;
                            mode = MODE_RESUME_PROMPT;
                        } else {
                            /* Fresh start */
                            book_offset = 0.0;
                            book_total_dur = (sel_fcount > 1)
                                ? compute_book_total_dur(sel_files, sel_fcount)
                                : 0.0;
                            book_n_ticks = (sel_fcount > 1)
                                ? compute_book_ticks(sel_files, sel_fcount,
                                      book_total_dur, book_tick_fracs, 64)
                                : 0;
                            if (do_play(&player, state.action_path, renderer,
                                        &state, &lib,
                                        &play_folder_idx, &play_file_idx,
                                        &play_season_idx, &last_resume_save,
                                        errbuf, sizeof(errbuf)) != 0) {
                                strncpy(error_path, state.action_path,
                                        sizeof(error_path) - 1);
                                strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                                error_active = 1;
                            } else {
                                /* Probe total book duration asynchronously on first save */
                                mode = MODE_PLAYBACK;
                            }
                        }
                    }
                    state.action = BROWSER_ACTION_NONE;
                }

                if (state.action == BROWSER_ACTION_QUIT) {
                    running = 0;
                    state.action = BROWSER_ACTION_NONE;
                }

                if (state.action == BROWSER_ACTION_FETCH_ART) {
                    fetch_art_folder_idx = state.folder_idx;
                    {
                        const MediaFolder *_fal = &lib.folders[state.folder_idx];
                        fetch_art_season_idx = (_fal->is_series &&
                                                state.view == VIEW_SEASONS)
                                               ? state.season_idx : -1;
                    }
                    mode = MODE_FETCH_ART_PROMPT;
                    state.action = BROWSER_ACTION_NONE;
                }

                browser_event_done:;
            } else if (mode == MODE_RESUME_PROMPT) {
                if (ev.type == SDL_KEYDOWN) {
                    char errbuf[256] = {0};
                    int  act = ev.key.keysym.sym;
                    if (act == SDLK_RETURN || act == SDLK_SPACE) {
                        /* A — resume at saved position */
                        if (do_play(&player, resume_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, resume_path, sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                            mode = MODE_BROWSER;
                        } else {
                            book_offset = resume_book_offset;
                            player_seek_to(&player, resume_pos);
                            player_show_osd(&player);
                            mode = MODE_PLAYBACK;
                        }
                    } else if (act == SDLK_LALT) {
                        /* X — start from beginning of book */
                        int fcount;
                        const AudioFile *rfiles = get_file_list(&lib,
                            resume_folder_idx, state.season_idx, &fcount);
                        const char *start_path = (rfiles && fcount > 0)
                                                 ? rfiles[0].path : resume_path;
                        book_offset    = 0.0;
                        book_total_dur = (rfiles && fcount > 1)
                            ? compute_book_total_dur(rfiles, fcount) : 0.0;
                        book_n_ticks   = (rfiles && fcount > 1)
                            ? compute_book_ticks(rfiles, fcount,
                                  book_total_dur, book_tick_fracs, 64)
                            : 0;
                        if (do_play(&player, start_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, start_path, sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                            mode = MODE_BROWSER;
                        } else {
                            mode = MODE_PLAYBACK;
                        }
                    } else if (act == SDLK_LCTRL || act == SDLK_BACKSPACE) {
                        /* B — cancel */
                        mode = MODE_BROWSER;
                    }
                    (void)resume_book_file_idx;
                }

            } else if (mode == MODE_FETCH_ART_PROMPT) {
                if (ev.type == SDL_KEYDOWN) {
                    int act = ev.key.keysym.sym;
                    int fi  = fetch_art_folder_idx;
                    int si  = fetch_art_season_idx;
                    int is_season = (si >= 0 && fi >= 0 && fi < lib.folder_count
                                     && si < lib.folders[fi].season_count);
                    /* Resolve path and title */
                    const char *fpath = NULL;
                    const char *fname = NULL;
                    if (is_season) {
                        fpath = lib.folders[fi].seasons[si].path;
                        fname = lib.folders[fi].seasons[si].name;
                    } else if (fi >= 0 && fi < lib.folder_count) {
                        fpath = lib.folders[fi].path;
                        const char *sl = strrchr(fpath, '/');
                        fname = sl ? sl + 1 : fpath;
                    }

                    if ((act == SDLK_RETURN || act == SDLK_SPACE) && fpath) {
                        /* A — fetch art */
                        char covpath[1280];
                        snprintf(covpath, sizeof(covpath), "%s/cover.jpg", fpath);
                        remove(covpath);
                        snprintf(covpath, sizeof(covpath), "%s/cover.png", fpath);
                        remove(covpath);
                        /* Clear cached texture */
                        if (is_season) {
                            if (si < cache.season_tex_count && cache.season_textures[si]) {
                                SDL_DestroyTexture(cache.season_textures[si]);
                                cache.season_textures[si] = NULL;
                            }
                        } else if (fi < cache.count && cache.textures[fi]) {
                            SDL_DestroyTexture(cache.textures[fi]);
                            cache.textures[fi] = NULL;
                        }
                        cover_fetch_async(fname, NULL, fpath);
                        mode = MODE_BROWSER;
                    } else if (act == SDLK_LALT && fpath) {
                        /* X — clear art only */
                        char covpath[1280];
                        snprintf(covpath, sizeof(covpath), "%s/cover.jpg", fpath);
                        remove(covpath);
                        snprintf(covpath, sizeof(covpath), "%s/cover.png", fpath);
                        remove(covpath);
                        if (is_season) {
                            if (si < cache.season_tex_count && cache.season_textures[si]) {
                                SDL_DestroyTexture(cache.season_textures[si]);
                                cache.season_textures[si] = NULL;
                            }
                        } else if (fi < cache.count && cache.textures[fi]) {
                            SDL_DestroyTexture(cache.textures[fi]);
                            cache.textures[fi] = NULL;
                        }
                        mode = MODE_BROWSER;
                    } else if (act == SDLK_LCTRL || act == SDLK_BACKSPACE) {
                        /* B — cancel */
                        mode = MODE_BROWSER;
                    }
                }

            } else if (mode == MODE_UPNEXT) {
                if (ev.type == SDL_KEYDOWN) {
                    int act = ev.key.keysym.sym;
                    if (act == SDLK_RETURN || act == SDLK_SPACE) {
                        char errbuf[256] = {0};
                        if (do_play(&player, upnext_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx, &play_season_idx,
                                    &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, upnext_path, sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                            mode = MODE_BROWSER;
                        } else {
                            mode = MODE_PLAYBACK;
                        }
                    } else if (act == SDLK_LCTRL || act == SDLK_BACKSPACE) {
                        mode = MODE_BROWSER;
                    }
                    (void)upnext_folder_idx; (void)upnext_file_idx;
                }

            } else { /* MODE_PLAYBACK */
                if (ev.type == SDL_KEYDOWN) {
                    SDL_Keycode key = ev.key.keysym.sym;

                    /* Y double-tap: toggle screensaver (works regardless of state) */
                    if (key == SDLK_LSHIFT) {
                        Uint32 now = SDL_GetTicks();
                        if (now - last_y_press_ms <= Y_DOUBLETAP_MS) {
                            screensaver_on = !screensaver_on;
                            last_y_press_ms = 0;   /* reset so triple-tap doesn't re-fire */
                            scrn_hint_hide_at = 0; /* clear hint on successful wake */
                        } else {
                            last_y_press_ms = now;
                        }
                        /* consume — no other action for Y */
                        goto playback_event_done;
                    }

                    /* During screensaver, block all input except Y (handled above) */
                    if (screensaver_on || player_screensaver_active(&player, SCREENSAVER_TIMEOUT_MS)) {
                        scrn_hint_hide_at = SDL_GetTicks() + 2000;
                        goto playback_event_done;
                    }

                    player_show_osd(&player);
                    player_reset_activity(&player);

                    switch (key) {
                        /* A — pause / resume */
                        case SDLK_SPACE:
                            if (player.state == PLAYER_PLAYING) player_pause(&player);
                            else if (player.state == PLAYER_PAUSED) player_resume(&player);
                            break;

                        /* D-pad left/right — ±10s seek */
                        case SDLK_LEFT:
                            do_book_seek(&player, -10.0, renderer,
                                         &state, &lib,
                                         &play_file_idx, &play_season_idx,
                                         &book_offset, &last_resume_save);
                            break;
                        case SDLK_RIGHT: player_seek(&player, +10.0); break;

                        /* L1/R1 — ±60s seek */
                        case SDLK_PAGEUP:
                            do_book_seek(&player, -60.0, renderer,
                                         &state, &lib,
                                         &play_file_idx, &play_season_idx,
                                         &book_offset, &last_resume_save);
                            break;
                        case SDLK_PAGEDOWN: player_seek(&player, +60.0); break;

                        /* L2/R2 — prev/next chapter (±15min if no chapters) */
                        case SDLK_COMMA: player_prev_chapter(&player); break;
                        case SDLK_PERIOD: player_next_chapter(&player); break;

                        /* Up/Down — brightness */
                        case SDLK_UP:   player_brightness_up(&player); break;
                        case SDLK_DOWN: player_brightness_dn(&player); break;

                        /* Vol+/Vol- */
                        case SDLK_MINUS:  player_volume_dn(&player); break;
                        case SDLK_EQUALS: player_volume_up(&player); break;

                        /* X — sleep timer cycle */
                        case SDLK_LALT:
                            if (!ev.key.repeat) {
                                sleep_preset_idx =
                                    (sleep_preset_idx + 1) % SLEEP_PRESET_COUNT;
                                player_set_sleep_timer(&player,
                                    SLEEP_PRESETS_SEC[sleep_preset_idx]);
                            }
                            break;

                        /* START — cycle playback speed */
                        case SDLK_RETURN:
                            if (!ev.key.repeat)
                                player_cycle_speed(&player);
                            break;

                        /* SELECT — reset speed to 1x */
                        case SDLK_RCTRL:
                            player_set_speed(&player, 0);  /* index 0 = 1.0x */
                            break;

                        /* B — stop + back to browser */
                        case SDLK_LCTRL:
                        case SDLK_BACKSPACE:
                            do_resume_save(&player, &lib,
                                           play_folder_idx, play_file_idx,
                                           play_season_idx,
                                           book_offset, book_total_dur);
                            player_close(&player);
                            sleep_preset_idx = 0;
                            screensaver_on = 0;
                            state.prog_folder_idx = -1;
                            state.prog_season_idx = -1;
                            mode = MODE_BROWSER;
                            break;

                        default: break;
                    }
                    playback_event_done:;
                }
            }
        }

        /* ---------------------------------------------------------------
         * Logic updates (outside event loop)
         * ------------------------------------------------------------- */

        /* Handle browser / history actions that emit from non-event paths */
        if (mode == MODE_BROWSER || mode == MODE_HISTORY) {
            if (history.action == HISTORY_ACTION_PLAY) {
                char errbuf[256] = {0};

                /* Detect book entries: action_path is a folder, so
                   resume_load_book will return a valid entry for it. */
                int    hbook_fi = 0;
                double hbook_sv = -1.0, hbook_td = 0.0, hbook_bp = 0.0;
                int is_book = resume_load_book(history.action_path,
                                               &hbook_fi, &hbook_sv,
                                               &hbook_td, &hbook_bp);

                if (is_book) {
                    /* Find folder in library */
                    int hfidx = -1;
                    for (int fi = 0; fi < lib.folder_count; fi++) {
                        if (strcmp(lib.folders[fi].path,
                                   history.action_path) == 0) {
                            hfidx = fi; break;
                        }
                    }
                    if (hfidx < 0) is_book = 0; /* folder gone — fall through */
                    if (hfidx >= 0) {
                        int hfcount;
                        const AudioFile *hfiles = get_file_list(&lib, hfidx,
                                                                 -1, &hfcount);
                        const char *play_path =
                            (hfiles && hbook_fi < hfcount)
                            ? hfiles[hbook_fi].path
                            : (hfiles ? hfiles[0].path : NULL);
                        if (play_path) {
                            book_total_dur = (hbook_td > 0.0)
                                ? hbook_td
                                : compute_book_total_dur(hfiles, hfcount);
                            book_n_ticks = compute_book_ticks(hfiles, hfcount,
                                book_total_dur, book_tick_fracs, 64);
                            if (hbook_bp > 5.0) {
                                strncpy(resume_path, play_path,
                                        sizeof(resume_path) - 1);
                                resume_pos           = hbook_sv;
                                resume_folder_idx    = hfidx;
                                resume_book_file_idx = hbook_fi;
                                resume_book_offset   = hbook_bp - hbook_sv;
                                history_free(&history);
                                mode = MODE_RESUME_PROMPT;
                            } else {
                                book_offset = 0.0;
                                if (do_play(&player, play_path, renderer,
                                            &state, &lib,
                                            &play_folder_idx, &play_file_idx,
                                            &play_season_idx,
                                            &last_resume_save,
                                            errbuf, sizeof(errbuf)) == 0) {
                                    history_free(&history);
                                    mode = MODE_PLAYBACK;
                                } else {
                                    strncpy(error_path, history.action_path,
                                            sizeof(error_path) - 1);
                                    strncpy(error_msg, errbuf,
                                            sizeof(error_msg) - 1);
                                    error_active = 1;
                                }
                            }
                        }
                    }
                }
                if (!is_book) {
                    /* Single-file entry (or book fallback) */
                    book_total_dur     = 0.0;
                    book_offset        = 0.0;
                    resume_book_offset = 0.0;
                    book_n_ticks       = 0;
                    double sv = resume_load(history.action_path);
                    if (sv > 5.0) {
                        strncpy(resume_path, history.action_path,
                                sizeof(resume_path) - 1);
                        resume_pos        = sv;
                        navigate_to_file(&state, &lib, history.action_path);
                        resume_folder_idx = state.folder_idx;
                        history_free(&history);
                        mode = MODE_RESUME_PROMPT;
                    } else {
                        if (do_play(&player, history.action_path, renderer,
                                    &state, &lib,
                                    &play_folder_idx, &play_file_idx,
                                    &play_season_idx, &last_resume_save,
                                    errbuf, sizeof(errbuf)) != 0) {
                            strncpy(error_path, history.action_path,
                                    sizeof(error_path) - 1);
                            strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                            error_active = 1;
                        } else {
                            history_free(&history);
                            mode = MODE_PLAYBACK;
                        }
                    }
                }
                history.action = HISTORY_ACTION_NONE;
            }
            if (history.action == HISTORY_ACTION_BACK) {
                history_free(&history);
                history.action = HISTORY_ACTION_NONE;
                mode = MODE_BROWSER;
            }
            if (history.action == HISTORY_ACTION_CLEAR) {
                /* Per-entry remove: try both stores; one will be a no-op */
                resume_clear(history.action_path);
                resume_remove_completed(history.action_path);
                history.action = HISTORY_ACTION_NONE;
                history_free(&history);
                history_load(&history);
            }
        }

        /* Playback update */
        if (mode == MODE_PLAYBACK) {
            player_update(&player);

            /* Sleep timer */
            if (player_sleep_expired(&player)) {
                do_resume_save(&player, &lib,
                               play_folder_idx, play_file_idx, play_season_idx,
                               book_offset, book_total_dur);
                player_close(&player);
                sleep_preset_idx = 0;
                screensaver_on = 0;
                state.prog_folder_idx = -1;
                state.prog_season_idx = -1;
                mode = MODE_BROWSER;
                running = 0;
#ifdef SB_HW
                g_sleep_on_exit = 1;
#endif
            }

            if (player.eos && mode == MODE_PLAYBACK) {
                int fcount;
                const AudioFile *eos_files = get_file_list(&lib,
                    play_folder_idx, play_season_idx, &fcount);
                int next = play_file_idx + 1;

                if (fcount > 1 && next < fcount) {
                    /* Multi-file book mid-advance — update book_offset before close */
                    book_offset += player.probe.duration_sec;
                    /* Book resume will be written by the next do_resume_save */
                } else if (fcount > 1 && next >= fcount) {
                    /* Finished the last file of a multi-file book */
                    char folder[1024];
                    snprintf(folder, sizeof(folder), "%s", player.path);
                    char *sl = strrchr(folder, '/');
                    if (sl) *sl = '\0';
                    resume_record_completed(folder);
                    resume_clear_book(folder);
                    book_offset = 0.0; book_total_dur = 0.0; book_n_ticks = 0;
                } else {
                    /* Single-file book completed */
                    resume_record_completed(player.path);
                    resume_clear(player.path);
                }

                player_close(&player);

                /* Auto-advance to next file in folder/season — seamless for
                   multi-file books (no countdown screen). */
                if (eos_files && next < fcount) {
                    char errbuf[256] = {0};
                    if (do_play(&player, eos_files[next].path, renderer,
                                &state, &lib,
                                &play_folder_idx, &play_file_idx, &play_season_idx,
                                &last_resume_save,
                                errbuf, sizeof(errbuf)) != 0) {
                        strncpy(error_path, eos_files[next].path,
                                sizeof(error_path) - 1);
                        strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                        error_active = 1;
                        state.prog_folder_idx = -1;
                        state.prog_season_idx = -1;
                        mode = MODE_BROWSER;
                    }
                    /* mode stays MODE_PLAYBACK — seamless continuation */
                } else {
                    state.prog_folder_idx = -1;
                    state.prog_season_idx = -1;
                    mode = MODE_BROWSER;
                }
            } else if (mode == MODE_PLAYBACK) {
                Uint32 now = SDL_GetTicks();
                if (player.state == PLAYER_PLAYING &&
                    now - last_resume_save >= 15000) {
                    do_resume_save(&player, &lib,
                                   play_folder_idx, play_file_idx, play_season_idx,
                                   book_offset, book_total_dur);
                    last_resume_save = now;
                }
            }
        }

        /* Up-Next auto-advance */
        if (mode == MODE_UPNEXT) {
            Uint32 elapsed = SDL_GetTicks() - upnext_start;
            if (elapsed >= UPNEXT_DELAY_MS) {
                char errbuf[256] = {0};
                if (do_play(&player, upnext_path, renderer,
                            &state, &lib,
                            &play_folder_idx, &play_file_idx, &play_season_idx,
                            &last_resume_save,
                            errbuf, sizeof(errbuf)) != 0) {
                    strncpy(error_path, upnext_path, sizeof(error_path) - 1);
                    strncpy(error_msg, errbuf, sizeof(error_msg) - 1);
                    error_active = 1;
                    state.prog_folder_idx = -1;
                    state.prog_season_idx = -1;
                    mode = MODE_BROWSER;
                } else {
                    mode = MODE_PLAYBACK;
                }
            }
        }

        /* ---------------------------------------------------------------
         * Render
         * ------------------------------------------------------------- */
        int sbar_h = statusbar_height(win_w);

        if (mode == MODE_BROWSER || mode == MODE_RESUME_PROMPT ||
            mode == MODE_HISTORY  || mode == MODE_UPNEXT ||
            mode == MODE_FETCH_ART_PROMPT) {

            SDL_Rect content_vp = {0, sbar_h, win_w, win_h - sbar_h};
            SDL_RenderSetViewport(renderer, &content_vp);

            if (mode == MODE_HISTORY) {
                history_draw(renderer, font, font_small, &history,
                             theme_get(), win_w, win_h - sbar_h);
            } else {
                browser_draw(renderer, font, font_small,
                             &state, &cache, &lib,
                             default_folder ? default_folder : default_cover,
                             theme_get(), win_w, win_h - sbar_h);
            }

            SDL_RenderSetViewport(renderer, NULL);
            statusbar_draw(renderer, font, theme_get(), win_w, win_h);

        } else {
            /* Playback mode */
            player_draw(renderer, font, font_small, &player, theme_get(), win_w, win_h,
                        player.osd_visible, book_offset, book_total_dur,
                        book_n_ticks > 0 ? book_tick_fracs : NULL, book_n_ticks);

            /* Screensaver overlay — manual toggle or auto-timeout */
            if (screensaver_on || player_screensaver_active(&player, SCREENSAVER_TIMEOUT_MS)) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 250);
                SDL_Rect full = { 0, 0, win_w, win_h };
                SDL_RenderFillRect(renderer, &full);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

                /* "Double-tap Y to wake" hint — shown for 2s after any key press */
                if (scrn_hint_hide_at && SDL_GetTicks() < scrn_hint_hide_at) {
                    SDL_Color white = { 255, 255, 255, 255 };
                    SDL_Surface *surf = TTF_RenderUTF8_Blended(font_small,
                                            "Double-tap Y to wake", white);
                    if (surf) {
                        SDL_Texture *htex = SDL_CreateTextureFromSurface(renderer, surf);
                        if (htex) {
                            SDL_Rect dst = {
                                (win_w - surf->w) / 2,
                                win_h / 2 - surf->h / 2,
                                surf->w, surf->h
                            };
                            SDL_SetTextureBlendMode(htex, SDL_BLENDMODE_BLEND);
                            SDL_RenderCopy(renderer, htex, NULL, &dst);
                            SDL_DestroyTexture(htex);
                        }
                        SDL_FreeSurface(surf);
                    }
                }
            }

            /* Sleep timer countdown pill — drawn on top of everything including screensaver */
            if (player.sleep_armed) {
                Uint32 now_pill = SDL_GetTicks();
                int rem_sec = (player.sleep_end_ms > now_pill)
                              ? (int)((player.sleep_end_ms - now_pill) / 1000) : 0;
                char pill[32];
                snprintf(pill, sizeof(pill), "zz %d:%02d", rem_sec / 60, rem_sec % 60);
                SDL_Color white = { 255, 255, 255, 220 };
                SDL_Surface *ps = TTF_RenderUTF8_Blended(font, pill, white);
                if (ps) {
                    int hpad = sc(16, win_w), vpad = sc(6, win_w);
                    int pw = ps->w + hpad * 2, ph = ps->h + vpad * 2;
                    int px = (win_w - pw) / 2;
                    int sbar_h = statusbar_height(win_w);
                    int py = sbar_h + sc(6, win_w);
                    int pill_rad = ph / 2;  /* full pill shape */
                    fill_rounded_rect_main(renderer, px, py, pw, ph,
                                           pill_rad, 0, 0, 0, 180);
                    SDL_Texture *pt = SDL_CreateTextureFromSurface(renderer, ps);
                    if (pt) {
                        SDL_SetTextureBlendMode(pt, SDL_BLENDMODE_BLEND);
                        SDL_Rect td = { px + hpad, py + vpad, ps->w, ps->h };
                        SDL_RenderCopy(renderer, pt, NULL, &td);
                        SDL_DestroyTexture(pt);
                    }
                    SDL_FreeSurface(ps);
                }
            }
        }

        /* Overlay layers */
        if (overlay_active) {
            if (!help_cache) {
                help_cache = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_TARGET, win_w, win_h);
                if (help_cache) {
                    SDL_SetRenderTarget(renderer, help_cache);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
                    SDL_RenderClear(renderer);
                    help_draw(renderer, font, font_small, theme_get(), win_w, win_h);
                    SDL_SetRenderTarget(renderer, NULL);
                    SDL_SetTextureBlendMode(help_cache, SDL_BLENDMODE_BLEND);
                }
            }
            if (help_cache)
                SDL_RenderCopy(renderer, help_cache, NULL, NULL);
            else
                help_draw(renderer, font, font_small, theme_get(), win_w, win_h);
        } else if (tutorial.active) {
            tutorial_draw(renderer, font, font_small, &tutorial, theme_get(), win_w, win_h);
        } else if (mode == MODE_RESUME_PROMPT) {
            double rdisp = (resume_book_offset > 0.0)
                           ? resume_book_offset + resume_pos : resume_pos;
            /* For multi-file books, show folder name instead of track filename */
            char rname_buf[1024];
            const char *rname = resume_path;
            if (resume_book_offset > 0.0) {
                strncpy(rname_buf, resume_path, sizeof(rname_buf) - 1);
                rname_buf[sizeof(rname_buf) - 1] = '\0';
                char *sl = strrchr(rname_buf, '/');
                if (sl) { *sl = '\0'; rname = rname_buf; }
            }
            resume_prompt_draw(renderer, font, font_small, theme_get(), win_w, win_h,
                               rname, rdisp);
        } else if (mode == MODE_FETCH_ART_PROMPT) {
            const char *book_name = "";
            {
                int fi = fetch_art_folder_idx, si = fetch_art_season_idx;
                if (fi >= 0 && fi < lib.folder_count) {
                    if (si >= 0 && si < lib.folders[fi].season_count)
                        book_name = lib.folders[fi].seasons[si].name;
                    else
                        book_name = lib.folders[fi].name;
                }
            }
            fetch_art_prompt_draw(renderer, font, font_small, theme_get(), win_w, win_h,
                                  book_name);
        } else if (mode == MODE_UPNEXT) {
            int secs_left = (int)((UPNEXT_DELAY_MS - (SDL_GetTicks() - upnext_start) + 999) / 1000);
            if (secs_left < 0) secs_left = 0;
            upnext_draw(renderer, font, font_small, theme_get(), win_w, win_h,
                        upnext_path, secs_left);
        }
        if (error_active) {
            error_draw(renderer, font, font_small, theme_get(), win_w, win_h,
                       error_path, error_msg);
        }

        SDL_RenderPresent(renderer);
#ifdef SB_A30
        a30_flip(hw_surf);
#elif defined(SB_TRIMUI_BRICK)
        brick_flip(hw_surf);
#endif

        /* Frame rate cap */
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        long elapsed_ns = (ts_now.tv_sec  - ts_frame_start.tv_sec)  * 1000000000L
                        + (ts_now.tv_nsec - ts_frame_start.tv_nsec);
        if (elapsed_ns < frame_ns) {
            struct timespec sleep_ts = { 0, frame_ns - elapsed_ns };
            nanosleep(&sleep_ts, NULL);
        }
    }

    /* Cleanup */
    if (player.state != PLAYER_STOPPED) {
        resume_save(player.path,
                    audio_get_clock(&player.audio),
                    player.probe.duration_sec);
        player_close(&player);
    }
    history_free(&history);
    browser_state_free(&state);
    cover_cache_free(&cache);
    library_free(&lib);
    demux_preopen_cancel();
    if (default_cover)  SDL_DestroyTexture(default_cover);
    if (default_folder) SDL_DestroyTexture(default_folder);
    if (help_cache)     SDL_DestroyTexture(help_cache);

cleanup:
    if (font_small) TTF_CloseFont(font_small);
    if (font)       TTF_CloseFont(font);
    if (renderer)   SDL_DestroyRenderer(renderer);
#if defined(SB_A30) || defined(SB_TRIMUI_BRICK)
    if (hw_surf) SDL_FreeSurface(hw_surf);
#endif
#ifdef SB_A30
    a30_screen_close();
#elif defined(SB_TRIMUI_BRICK)
    brick_screen_close();
#else
    if (win) SDL_DestroyWindow(win);
#endif
    IMG_Quit(); TTF_Quit(); SDL_Quit();

#ifdef SB_HW
    if (g_sleep_on_exit) {
        /* Signal launch.sh to suspend via SpruceOS sleep_helper.sh.
           Writing directly to /sys/power/state bypasses SpruceOS's watchdog
           and causes a second spurious sleep after the user wakes the device.
           Instead, write a sentinel file; launch.sh calls sleep_helper.sh
           after we exit, letting SpruceOS coordinate the suspend properly. */
        int sfd = open("/tmp/storyboy_suspend",
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sfd >= 0) close(sfd);
    }
#endif
    return 0;
}
