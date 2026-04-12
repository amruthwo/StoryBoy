#include "cover.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <SDL2/SDL_image.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#if defined(SB_A30)
#include <spawn.h>
#include <sys/resource.h>
extern char **environ;
#endif

/* -------------------------------------------------------------------------
 * Embedded artwork extraction
 * ---------------------------------------------------------------------- */

/* Extract the first attached-picture stream from an audio file and return
   it as an SDL_Texture.  Returns NULL if none found or on error. */
/* Load an image from an SDL_RWops, scale to 256×256 max, return as texture.
   freesrc: passed through to SDL_RWclose after load. */
static SDL_Texture *load_texture_scaled(SDL_Renderer *renderer,
                                        SDL_RWops *rw, int freesrc) {
#ifdef SB_A30
    SDL_Surface *surf = IMG_Load_RW(rw, freesrc);
    if (!surf) return NULL;
    if (surf->w > 256 || surf->h > 256) {
        int tw = surf->w > surf->h ? 256 : (surf->w * 256 / surf->h);
        int th = surf->h > surf->w ? 256 : (surf->h * 256 / surf->w);
        SDL_Surface *small = SDL_CreateRGBSurfaceWithFormat(
            0, tw, th, surf->format->BitsPerPixel, surf->format->format);
        if (small) { SDL_BlitScaled(surf, NULL, small, NULL); SDL_FreeSurface(surf); surf = small; }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
#else
    return IMG_LoadTexture_RW(renderer, rw, freesrc);
#endif
}

/* Load a cover image from a file path, scaled to 256×256 max on SB_A30. */
static SDL_Texture *load_texture_file_scaled(SDL_Renderer *renderer,
                                             const char *path) {
#ifdef SB_A30
    SDL_Surface *surf = IMG_Load(path);
    if (!surf) return NULL;
    if (surf->w > 256 || surf->h > 256) {
        int tw = surf->w > surf->h ? 256 : (surf->w * 256 / surf->h);
        int th = surf->h > surf->w ? 256 : (surf->h * 256 / surf->w);
        SDL_Surface *small = SDL_CreateRGBSurfaceWithFormat(
            0, tw, th, surf->format->BitsPerPixel, surf->format->format);
        if (small) { SDL_BlitScaled(surf, NULL, small, NULL); SDL_FreeSurface(surf); surf = small; }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
#else
    return IMG_LoadTexture(renderer, path);
#endif
}

static SDL_Texture *extract_embedded_cover(SDL_Renderer *renderer,
                                            const char *audio_path) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) < 0)
        return NULL;

    fmt->probesize            = 256 * 1024;  /* tiny: we only need stream info */
    fmt->max_analyze_duration = 100000;

    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return NULL;
    }

    SDL_Texture *tex = NULL;

    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        AVPacket *pkt = &st->attached_pic;
        if (!pkt->data || pkt->size <= 0) continue;

        SDL_RWops *rw = SDL_RWFromMem(pkt->data, pkt->size);
        if (rw) {
            tex = load_texture_scaled(renderer, rw, 1 /* freesrc */);
            if (tex) break;
        }
    }

    avformat_close_input(&fmt);
    return tex;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

SDL_Texture *cover_load(SDL_Renderer *renderer, const char *audio_path) {
#if !defined(SB_A30)
    /* On all SB_A30 builds (SpruceOS and OnionOS armhf) skip inline extraction:
       two concurrent AVFormatContexts risk OOM on SpruceOS, and on OnionOS the
       slow V2/V3 CPU makes it impractical during playback too.  cover.jpg is
       written by extract_cover (background process) before the user picks a book. */
    SDL_Texture *tex = extract_embedded_cover(renderer, audio_path);
    if (tex) return tex;
#endif

    /* Try local cover.jpg / cover.png in the same directory */
    char dir[1024];
    strncpy(dir, audio_path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else dir[0] = '\0';

    if (dir[0]) {
        char p[1200];
        SDL_Texture *tex;
        snprintf(p, sizeof(p), "%s/cover.jpg", dir);
        tex = load_texture_file_scaled(renderer, p);
        if (tex) return tex;
        snprintf(p, sizeof(p), "%s/cover.png", dir);
        tex = load_texture_file_scaled(renderer, p);
        if (tex) return tex;
    }

    return NULL;
}

SDL_Texture *cover_load_file(SDL_Renderer *renderer, const char *cover_path) {
    if (!cover_path || !cover_path[0]) return NULL;
    return load_texture_file_scaled(renderer, cover_path);
}

int cover_extract_to_file(const char *audio_path, const char *dest_path) {
    if (!audio_path || !dest_path) return 0;

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) < 0) return 0;

    fmt->probesize            = 256 * 1024;
    fmt->max_analyze_duration = 100000;

    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt); return 0;
    }

    int saved = 0;
    for (unsigned int i = 0; i < fmt->nb_streams && !saved; i++) {
        AVStream *st = fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        AVPacket *pkt = &st->attached_pic;
        if (!pkt->data || pkt->size <= 0) continue;

        FILE *f = fopen(dest_path, "wb");
        if (f) {
            if ((int)fwrite(pkt->data, 1, (size_t)pkt->size, f) == pkt->size)
                saved = 1;
            fclose(f);
            if (!saved) remove(dest_path);
        }
    }

    avformat_close_input(&fmt);
    return saved;
}

void cover_fetch_async(const char *title, const char *author,
                       const char *book_dir) {
    if (!title || !title[0] || !book_dir || !book_dir[0]) return;

    /* Resolve fetch_cover binary path relative to this executable */
#if defined(SB_A30)
    const char *bin = "/mnt/SDCARD/App/StoryBoy/bin32/fetch_cover";
#elif defined(SB_TRIMUI_BRICK) || defined(SB_TRIMUI_SMART)
    const char *bin = "/mnt/SDCARD/App/StoryBoy/bin64/fetch_cover";
#else
    /* Desktop / test build: look next to the storyboy binary */
    const char *bin = "./fetch_cover";
#endif

#if defined(SB_A30)
    /* posix_spawn — non-blocking, preferred on embedded hardware */
    char *args[5];
    int   argc = 0;
    args[argc++] = (char *)bin;
    args[argc++] = (char *)book_dir;
    args[argc++] = (char *)title;
    if (author && author[0])
        args[argc++] = (char *)author;
    args[argc] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    /* Redirect stdout to /dev/null; stderr to log for diagnostics */
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2,
        "/mnt/SDCARD/App/StoryBoy/fetch_cover.log",
        O_WRONLY | O_CREAT | O_APPEND, 0644);

    pid_t pid;
    posix_spawn(&pid, bin, &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    /* Don't waitpid — fire and forget; cover_fetch_done polls dest file */
#else
    /* Non-A30 hardware / desktop: fork+exec */
    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        if (author && author[0])
            execl(bin, bin, book_dir, title, author, (char *)NULL);
        else
            execl(bin, bin, book_dir, title, (char *)NULL);
        _exit(1);
    }
    /* parent: don't wait — fire and forget */
    (void)pid;
#endif
}

int cover_fetch_done(const char *dest_path) {
    return dest_path && dest_path[0] && access(dest_path, F_OK) == 0;
}
