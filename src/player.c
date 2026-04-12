#include "player.h"
#include "cover.h"
#include "hintbar.h"
#include "statusbar.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#ifdef SB_A30
#include <malloc.h>  /* malloc_trim */
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>

/* -------------------------------------------------------------------------
 * Speed presets
 * ---------------------------------------------------------------------- */

const float SPEED_PRESETS[SPEED_PRESET_COUNT] = { 1.0f, 1.25f, 1.5f, 2.0f };

/* -------------------------------------------------------------------------
 * Open
 * ---------------------------------------------------------------------- */

int player_open(Player *p, const char *path, SDL_Renderer *renderer,
                char *errbuf, int errbuf_sz) {
    float prev_vol   = p->volume;
    float prev_speed = (p->speed_idx >= 0 && p->speed_idx < SPEED_PRESET_COUNT)
                       ? SPEED_PRESETS[p->speed_idx] : 1.0f;
    int   prev_speed_idx = p->speed_idx;

    memset(p, 0, sizeof(*p));
    snprintf(p->path, sizeof(p->path), "%s", path);

#ifdef SB_A30
    /* On memory-constrained devices skip the standalone decoder_probe open.
       Open demux first, then derive probe info from the already-open fmt_ctx.
       Use a pre-opened context if available (started by demux_preopen_start
       after library scan), otherwise open fresh. */
    malloc_trim(0);
    if (!demux_preopen_claim(path, &p->demux)) {
        if (demux_open(&p->demux, path, errbuf, errbuf_sz) < 0)
            return -1;
    } else {
        fprintf(stderr, "player_open: using pre-opened demux\n");
    }
    {
        AVFormatContext *fmt = p->demux.fmt_ctx;
        if (fmt->duration != AV_NOPTS_VALUE)
            p->probe.duration_sec = (double)fmt->duration / AV_TIME_BASE;
        p->probe.audio_stream_idx = p->demux.audio_stream_idx;
        if (p->demux.audio_stream_idx >= 0) {
            AVStream          *as = fmt->streams[p->demux.audio_stream_idx];
            AVCodecParameters *ap = as->codecpar;
            p->probe.sample_rate  = ap->sample_rate;
            p->probe.channels     = ap->ch_layout.nb_channels;
            const AVCodecDescriptor *cd = avcodec_descriptor_get(ap->codec_id);
            snprintf(p->probe.audio_codec, sizeof(p->probe.audio_codec),
                     "%s", cd ? cd->name : "unknown");
        }
        for (unsigned int i = 0; i < fmt->nb_streams; i++)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                p->probe.audio_track_count++;
        p->probe.num_chapters = (int)fmt->nb_chapters;
    }
#else
    if (decoder_probe(path, &p->probe, errbuf, errbuf_sz) < 0)
        return -1;

    if (demux_open(&p->demux, path, errbuf, errbuf_sz) < 0)
        return -1;
#endif

    if (p->demux.audio_stream_idx >= 0) {
        AVStream *as = p->demux.fmt_ctx->streams[p->demux.audio_stream_idx];
        if (audio_open(&p->audio, as->codecpar,
                       &p->demux.audio_queue, errbuf, errbuf_sz) < 0) {
            demux_close(&p->demux);
            return -1;
        }
        p->audio.codec_ctx->pkt_timebase = as->time_base;
        /* Record stream start time so the clock is always file-relative (0 = BOF).
           Some m4b files have a non-zero stream start_time; subtracting it prevents
           the position counter from showing a large absolute offset. */
        p->audio.start_sec = (as->start_time != AV_NOPTS_VALUE && as->start_time > 0)
                             ? (double)as->start_time * av_q2d(as->time_base)
                             : 0.0;
    }

    p->state = PLAYER_STOPPED;

    /* Restore / initialise volume */
#ifdef SB_A30
    if (g_display_rotation == 270) {
        float svm_vol = (prev_vol > 0.0f) ? prev_vol : 0.8f;
        FILE *amix = popen("/usr/bin/amixer sget 'Soft Volume Master' 2>/dev/null", "r");
        if (amix) {
            char line[256];
            while (fgets(line, sizeof(line), amix)) {
                int raw = 0;
                char *b = strstr(line, "[");
                if (b && sscanf(b, "[%d%%]", &raw) == 1) {
                    svm_vol = raw / 100.0f;
                    if (svm_vol > 1.0f) svm_vol = 1.0f;
                    break;
                }
            }
            pclose(amix);
        }
        p->volume = svm_vol;
    } else {
        p->volume = (prev_vol > 0.0f) ? prev_vol : 1.0f;
    }
    atomic_store(&p->audio.volume, p->volume);
#else
    p->volume = (prev_vol > 0.0f) ? prev_vol : 1.0f;
    atomic_store(&p->audio.volume, p->volume);
#endif

    p->brightness = 1.0f;

    /* Restore playback speed */
    p->speed_idx = prev_speed_idx;
    audio_set_speed(&p->audio, prev_speed);

    p->last_activity_ms = SDL_GetTicks();

    /* Store renderer for async cover reload */
    p->renderer = renderer;

    /* Get book directory */
    snprintf(p->cover_book_dir, sizeof(p->cover_book_dir), "%s", path);
    char *bslash = strrchr(p->cover_book_dir, '/');
    if (bslash) *bslash = '\0';

    /* Load cover art: embedded → local file → async fetch */
    p->cover_tex = cover_load(renderer, path);
    if (!p->cover_tex) {
        /* Use folder name as book title — much better API search query than
           the track filename (e.g. "Brandon Sanderson - Legion" not "Track02") */
        const char *sl = strrchr(p->cover_book_dir, '/');
        const char *book_title = sl ? sl + 1 : p->cover_book_dir;
        cover_fetch_async(book_title, NULL, p->cover_book_dir);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Playback control
 * ---------------------------------------------------------------------- */

void player_play(Player *p) {
    if (p->state != PLAYER_STOPPED) return;
    if (p->demux.audio_stream_idx >= 0) audio_start(&p->audio);
    demux_start(&p->demux);
    p->state = PLAYER_PLAYING;
}

void player_pause(Player *p) {
    if (p->state != PLAYER_PLAYING) return;
    audio_pause(&p->audio, 1);
    p->state = PLAYER_PAUSED;
}

void player_resume(Player *p) {
    if (p->state != PLAYER_PAUSED) return;
    audio_pause(&p->audio, 0);
    p->state = PLAYER_PLAYING;
}

void player_show_osd(Player *p) {
    p->osd_visible = 1;
    p->osd_hide_at = SDL_GetTicks() + 3000;
}

void player_show_wake_toast(Player *p) {
    p->wake_osd_visible = 1;
    p->wake_osd_hide_at = SDL_GetTicks() + 2000;
}

void player_set_volume(Player *p, float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    p->volume = vol;
    atomic_store(&p->audio.volume, vol);
    p->vol_osd_visible = 1;
    p->vol_osd_hide_at = SDL_GetTicks() + 1500;
}

/* SpruceOS uses 20 equal steps (1/20 = 0.05) for hardware VOL keys. */
#ifdef SB_A30
void player_volume_up(Player *p) { player_set_volume(p, p->volume + 0.05f); }
void player_volume_dn(Player *p) { player_set_volume(p, p->volume - 0.05f); }
#else
void player_volume_up(Player *p) { player_set_volume(p, p->volume + 0.1f); }
void player_volume_dn(Player *p) { player_set_volume(p, p->volume - 0.1f); }
#endif

static void set_brightness(Player *p, float b) {
    if (b < 0.1f) b = 0.1f;
    if (b > 1.0f) b = 1.0f;
    p->brightness = b;
    p->bri_osd_visible = 1;
    p->bri_osd_hide_at = SDL_GetTicks() + 1500;
}
void player_brightness_up(Player *p) { set_brightness(p, p->brightness + 0.1f); }
void player_brightness_dn(Player *p) { set_brightness(p, p->brightness - 0.1f); }

void player_cycle_audio(Player *p) {
    if (p->demux.audio_stream_count <= 1) return;

    double current_pos = audio_get_clock(&p->audio);
    int new_idx = demux_cycle_audio(&p->demux);
    AVStream *as = p->demux.fmt_ctx->streams[new_idx];

    char errbuf[256] = {0};
    if (audio_switch_stream(&p->audio, &p->demux.audio_queue,
                            as->codecpar, as->time_base,
                            errbuf, sizeof(errbuf)) < 0) {
        fprintf(stderr, "audio_switch_stream: %s\n", errbuf);
        return;
    }
    p->audio.codec_ctx->pkt_timebase = as->time_base;
    p->audio.start_sec = (as->start_time != AV_NOPTS_VALUE && as->start_time > 0)
                         ? (double)as->start_time * av_q2d(as->time_base)
                         : 0.0;

    /* Seek back to current position so new track starts at same point */
    player_seek_to(p, current_pos);

    /* Build OSD label */
    int pos = 0;
    for (int i = 0; i < p->demux.audio_stream_count; i++) {
        if (p->demux.audio_stream_indices[i] == new_idx) { pos = i; break; }
    }
    AVDictionaryEntry *lang = av_dict_get(as->metadata, "language", NULL, 0);
    if (lang)
        snprintf(p->audio_osd_label, sizeof(p->audio_osd_label),
                 "Audio %d/%d: %s", pos + 1, p->demux.audio_stream_count, lang->value);
    else
        snprintf(p->audio_osd_label, sizeof(p->audio_osd_label),
                 "Audio %d/%d", pos + 1, p->demux.audio_stream_count);

    p->audio_osd_visible = 1;
    p->audio_osd_hide_at = SDL_GetTicks() + 2000;
}

void player_cycle_speed(Player *p) {
    p->speed_idx = (p->speed_idx + 1) % SPEED_PRESET_COUNT;
    float spd = SPEED_PRESETS[p->speed_idx];
    audio_set_speed(&p->audio, spd);
    p->speed_osd_visible = 1;
    p->speed_osd_hide_at = SDL_GetTicks() + 1500;
}

void player_set_speed(Player *p, int idx) {
    if (idx < 0 || idx >= SPEED_PRESET_COUNT) return;
    p->speed_idx = idx;
    float spd = SPEED_PRESETS[p->speed_idx];
    audio_set_speed(&p->audio, spd);
    p->speed_osd_visible = 1;
    p->speed_osd_hide_at = SDL_GetTicks() + 1500;
}

void player_set_sleep_timer(Player *p, int seconds) {
    if (seconds <= 0) {
        p->sleep_armed = 0;
        snprintf(p->sleep_osd_label, sizeof(p->sleep_osd_label), "Sleep: off");
    } else {
        p->sleep_armed  = 1;
        p->sleep_end_ms = SDL_GetTicks() + (Uint32)(seconds * 1000);
        if (seconds < 60)
            snprintf(p->sleep_osd_label, sizeof(p->sleep_osd_label), "Sleep: %ds", seconds);
        else
            snprintf(p->sleep_osd_label, sizeof(p->sleep_osd_label), "Sleep: %dm", seconds / 60);
    }
    p->sleep_osd_visible = 1;
    p->sleep_osd_hide_at = SDL_GetTicks() + 2000;
}

int player_sleep_expired(const Player *p) {
    return p->sleep_armed && SDL_GetTicks() >= p->sleep_end_ms;
}

void player_reset_activity(Player *p) {
    p->last_activity_ms = SDL_GetTicks();
}

int player_screensaver_active(const Player *p, Uint32 timeout_ms) {
    if (p->state != PLAYER_PLAYING) return 0;
    return (SDL_GetTicks() - p->last_activity_ms) >= timeout_ms;
}

void player_next_chapter(Player *p) {
    int count = demux_chapter_count(&p->demux);
    if (count <= 0) {
        /* No chapters — seek +15 minutes */
        player_seek(p, 15.0 * 60.0);
        return;
    }
    double pos = audio_get_clock(&p->audio);
    int cur = demux_chapter_at(&p->demux, pos);
    if (cur < count - 1) {
        double target = demux_chapter_start(&p->demux, cur + 1);
        player_seek_to(p, target);
        player_show_osd(p);
    }
}

void player_prev_chapter(Player *p) {
    int count = demux_chapter_count(&p->demux);
    if (count <= 0) {
        /* No chapters — seek -15 minutes */
        player_seek(p, -15.0 * 60.0);
        return;
    }
    double pos = audio_get_clock(&p->audio);
    int cur = demux_chapter_at(&p->demux, pos);
    double cur_start = demux_chapter_start(&p->demux, cur);
    /* If we're more than 3s into the chapter, restart it; else go to previous */
    if (pos - cur_start > 3.0) {
        player_seek_to(p, cur_start);
    } else if (cur > 0) {
        double target = demux_chapter_start(&p->demux, cur - 1);
        player_seek_to(p, target);
    } else {
        player_seek_to(p, 0.0);
    }
    player_show_osd(p);
}

void player_seek(Player *p, double delta_sec) {
    if (p->state == PLAYER_STOPPED) return;
    double current = audio_get_clock(&p->audio);
    double target  = current + delta_sec;
    if (target < 0) target = 0;
    if (target > p->probe.duration_sec) target = p->probe.duration_sec;
    demux_request_seek(&p->demux, target);
    SDL_LockMutex(p->audio.clock_mutex);
    p->audio.clock = target;
    SDL_UnlockMutex(p->audio.clock_mutex);
    player_show_osd(p);
}

void player_seek_to(Player *p, double pos_sec) {
    if (p->state == PLAYER_STOPPED) return;
    double target = pos_sec;
    if (target < 0) target = 0;
    if (target > p->probe.duration_sec) target = p->probe.duration_sec;
    demux_request_seek(&p->demux, target);
    SDL_LockMutex(p->audio.clock_mutex);
    p->audio.clock = target;
    SDL_UnlockMutex(p->audio.clock_mutex);
}

void player_close(Player *p) {
    audio_stop(&p->audio);
    demux_stop(&p->demux);
    audio_close(&p->audio);
    demux_close(&p->demux);
    if (p->cover_tex) { SDL_DestroyTexture(p->cover_tex); p->cover_tex = NULL; }
    p->state = PLAYER_STOPPED;
#ifdef SB_A30
    malloc_trim(0);
#endif
}

/* -------------------------------------------------------------------------
 * Update — call once per main loop iteration
 * ---------------------------------------------------------------------- */

int player_update(Player *p) {
    Uint32 now = SDL_GetTicks();

    /* Auto-hide OSD after 3s during playback */
    if (p->osd_visible && p->state == PLAYER_PLAYING && now >= p->osd_hide_at)
        p->osd_visible = 0;

    if (p->vol_osd_visible   && now >= p->vol_osd_hide_at)   p->vol_osd_visible   = 0;
    if (p->bri_osd_visible   && now >= p->bri_osd_hide_at)   p->bri_osd_visible   = 0;
    if (p->audio_osd_visible && now >= p->audio_osd_hide_at) p->audio_osd_visible = 0;
    if (p->speed_osd_visible && now >= p->speed_osd_hide_at) p->speed_osd_visible = 0;
    if (p->sleep_osd_visible && now >= p->sleep_osd_hide_at) p->sleep_osd_visible = 0;
    if (p->wake_osd_visible  && now >= p->wake_osd_hide_at)  p->wake_osd_visible  = 0;

    /* EOS detection */
    if (!p->eos && p->audio.eos)
        p->eos = 1;

    /* Async cover reload — poll for cover.jpg once the fetch_cover binary writes it.
       Check every ~2 seconds to avoid stat() overhead every frame. */
    static Uint32 cover_poll_at = 0;
    if (!p->cover_tex && p->cover_book_dir[0] && p->renderer &&
        now >= cover_poll_at) {
        cover_poll_at = now + 2000;
        char cpath[1280];
        snprintf(cpath, sizeof(cpath), "%s/cover.jpg", p->cover_book_dir);
        if (cover_fetch_done(cpath))
            p->cover_tex = cover_load_file(p->renderer, cpath);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Draw helpers
 * ---------------------------------------------------------------------- */

static inline int sc(int base, int w) { return (int)(base * w / 640.0f + 0.5f); }

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawColor(r, R, G, B, 0xff);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h,
                               int rad, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
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
        SDL_RenderDrawLine(r, x + rad - span,     y + dy,         x + rad - 1,         y + dy);
        SDL_RenderDrawLine(r, x + w - rad,         y + dy,         x + w - rad + span - 1, y + dy);
        SDL_RenderDrawLine(r, x + rad - span,     y + h - 1 - dy, x + rad - 1,         y + h - 1 - dy);
        SDL_RenderDrawLine(r, x + w - rad,         y + h - 1 - dy, x + w - rad + span - 1, y + h - 1 - dy);
    }
    if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text,
                      int x, int y, int max_w, Uint8 R, Uint8 G, Uint8 B) {
    SDL_Color col = { R, G, B, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = { x, y, surf->w < max_w ? surf->w : max_w, surf->h };
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

/* -------------------------------------------------------------------------
 * OSD sub-sections
 * ---------------------------------------------------------------------- */

/* Draw the bottom OSD bar (progress bar, timestamps, track/chapter name, hint bar).
   In audio-only mode this is always visible (osd_visible is ignored by callers).
   book_offset/book_total_dur: when > 0, display book-wide progress instead of
   per-file progress. */
static void draw_osd(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                     const Player *p, const Theme *t, int win_w, int win_h,
                     double book_offset, double book_total_dur,
                     const float *book_tick_fracs, int book_n_ticks) {
    double clock = audio_get_clock(&p->audio);

    /* Use book-wide progress when we have a total duration for a multi-file book */
    double display_pos = (book_total_dur > 0) ? book_offset + clock : clock;
    double display_dur = (book_total_dur > 0) ? book_total_dur : p->probe.duration_sec;
    double bar_dur     = (book_total_dur > 0) ? book_total_dur : p->probe.duration_sec;
    double bar_pos     = display_pos;

    char pos_str[32], dur_str[32];
    format_duration(display_pos, pos_str, sizeof(pos_str));
    format_duration(display_dur, dur_str, sizeof(dur_str));
    (void)bar_dur;

    int osd_h  = sc(80, win_w);
    int osd_y  = win_h - osd_h;
    int bar_h  = sc(6, win_w);
    int bar_y  = osd_y + sc(12, win_w);
    int bar_x  = sc(16, win_w);
    int bar_w  = win_w - sc(32, win_w);
    int text_y = bar_y + bar_h + sc(8, win_w);

    /* Semi-transparent background */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_Rect bg = { 0, osd_y, win_w, osd_h };
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    /* Progress bar track */
    fill_rect(r, bar_x, bar_y, bar_w, bar_h,
              t->secondary.r, t->secondary.g, t->secondary.b);
    /* Progress bar fill + scrubber */
    if (display_dur > 0) {
        float frac = (float)(bar_pos / display_dur);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        fill_rect(r, bar_x, bar_y, (int)(bar_w * frac), bar_h,
                  t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b);
        int dot_x = bar_x + (int)(bar_w * frac) - 4;
        fill_rect(r, dot_x, bar_y - 3, 8, bar_h + 6,
                  t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
    }

    /* Chapter tick marks on the progress bar */
    int n_ch = demux_chapter_count(&p->demux);
    if (book_n_ticks > 0 && book_tick_fracs) {
        /* Multi-file book: ticks at each file boundary */
        for (int i = 0; i < book_n_ticks; i++) {
            float frac = book_tick_fracs[i];
            if (frac <= 0.0f || frac >= 1.0f) continue;
            int tx = bar_x + (int)(bar_w * frac);
            fill_rect(r, tx - 1, bar_y - 2, 2, bar_h + 4,
                      t->text.r, t->text.g, t->text.b);
        }
    } else if (n_ch > 1 && book_total_dur <= 0 && p->probe.duration_sec > 0) {
        /* Single-file book: ticks from embedded chapter metadata */
        for (int i = 1; i < n_ch; i++) {
            double cstart = demux_chapter_start(&p->demux, i);
            float frac = (float)(cstart / p->probe.duration_sec);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int tx = bar_x + (int)(bar_w * frac);
            fill_rect(r, tx - 1, bar_y - 2, 2, bar_h + 4,
                      t->text.r, t->text.g, t->text.b);
        }
    }

    /* Timestamp left, total right */
    draw_text(r, font, pos_str, bar_x, text_y, 120, 0xff, 0xff, 0xff);
    int dur_w = 0, dummy = 0;
    TTF_SizeUTF8(font, dur_str, &dur_w, &dummy);
    draw_text(r, font, dur_str, win_w - sc(16, win_w) - dur_w, text_y,
              dur_w + 4, 0xff, 0xff, 0xff);

    /* Track/chapter name centred */
    const char *label = NULL;
    char chapter_label[128] = {0};
    if (n_ch > 0) {
        int cur_ch = demux_chapter_at(&p->demux, clock);
        if (cur_ch >= 0) {
            AVChapter *ch = p->demux.fmt_ctx->chapters[cur_ch];
            AVDictionaryEntry *title = av_dict_get(ch->metadata, "title", NULL, 0);
            if (title && title->value[0]) {
                snprintf(chapter_label, sizeof(chapter_label), "%s", title->value);
                label = chapter_label;
            }
        }
    }
    if (!label) {
        if (book_total_dur > 0) {
            /* Multi-file book: show folder/book name instead of chapter filename */
            static char book_name[256];
            strncpy(book_name, p->path, sizeof(book_name) - 1);
            book_name[sizeof(book_name) - 1] = '\0';
            char *sl1 = strrchr(book_name, '/');
            if (sl1) {
                *sl1 = '\0';
                const char *sl2 = strrchr(book_name, '/');
                label = sl2 ? sl2 + 1 : book_name;
            } else {
                label = p->path;
            }
        } else {
            const char *slash = strrchr(p->path, '/');
            label = slash ? slash + 1 : p->path;
        }
    }
    int name_w = 0;
    TTF_SizeUTF8(font, label, &name_w, &dummy);
    int name_x = (win_w - name_w) / 2;
    if (name_x < sc(8, win_w)) name_x = sc(8, win_w);
    draw_text(r, font, label, name_x, text_y, win_w - sc(16, win_w),
              0xff, 0xff, 0xff);

    /* Pause icon — centred above the OSD bar */
    if (p->state == PLAYER_PAUSED) {
        int glyph_h  = sc(40, win_w);
        int bar2_w   = sc(10, win_w);
        int bar2_gap = sc(4,  win_w);
        int glyph_y  = osd_y / 2 - glyph_h / 2;
        fill_rect(r, win_w / 2 - bar2_gap - bar2_w, glyph_y, bar2_w, glyph_h,
                  t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
        fill_rect(r, win_w / 2 + bar2_gap, glyph_y, bar2_w, glyph_h,
                  t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
    }

    /* Hint bar */
    static const HintItem play_hints[] = {
        { "A",      "Pause"  },
        { "\xe2\x86\x90\xe2\x86\x92", "\xc2\xb1" "10s" },
        { "L1/R1",  "\xc2\xb1" "60s" },
        { "L2/R2",  "\xc2\xb1" "Ch." },
        { "START",  "Speed"  },
        { "B",      "Stop"   },
    };
    static const HintItem pause_hints[] = {
        { "A",      "Resume" },
        { "\xe2\x86\x90\xe2\x86\x92", "\xc2\xb1" "10s" },
        { "L1/R1",  "\xc2\xb1" "60s" },
        { "L2/R2",  "\xc2\xb1" "Ch." },
        { "START",  "Speed"  },
        { "B",      "Stop"   },
    };
    const HintItem *hints = (p->state == PLAYER_PAUSED) ? pause_hints : play_hints;
    int hint_count = 6;
    int hint_bar_h = sc(24, win_w);
    int hint_y     = win_h - hint_bar_h;
    int gh = TTF_FontHeight(font_small) + TTF_FontHeight(font_small) * 2 / 7;
    int ig = TTF_FontHeight(font_small) * 9 / 7;
    int total_w = 0;
    for (int i = 0; i < hint_count; i++)
        total_w += hintbar_item_width(font_small, &hints[i], gh)
                   + (i < hint_count - 1 ? ig : 0);
    int hint_x = (win_w - total_w) / 2;
    if (hint_x < sc(8, win_w)) hint_x = sc(8, win_w);
    hintbar_draw_items(r, font_small, hints, hint_count, t, hint_x, hint_y, hint_bar_h);
}

/* Generic segmented bar (volume / brightness) */
static void draw_indicator_bar(SDL_Renderer *r, TTF_Font *font,
                               const Theme *t, const char *label,
                               float value, int box_x, int box_y, int win_w) {
    int steps  = 10;
    int filled = (int)(value * steps + 0.5f);
    int seg_w  = sc(14, win_w);
    int seg_h  = sc(10, win_w);
    int gap    = sc(3,  win_w);
    int pad    = sc(8,  win_w);
    int bar_w  = steps * (seg_w + gap) - gap;
    int bar_h  = seg_h + pad * 2 + TTF_FontHeight(font) + sc(4, win_w);
    int label_slot = sc(46, win_w);
    int box_w  = bar_w + pad * 4 + label_slot;

    fill_rounded_rect(r, box_x, box_y, box_w, bar_h, sc(8, win_w),
                      0, 0, 0, 180);

    draw_text(r, font, label, box_x + pad, box_y + pad,
              label_slot, 0xc0, 0xc0, 0xc0);

    int sx = box_x + pad + label_slot;
    int sy = box_y + pad;
    for (int i = 0; i < steps; i++) {
        int x = sx + i * (seg_w + gap);
        if (i < filled)
            fill_rect(r, x, sy, seg_w, seg_h,
                      t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b);
        else
            fill_rect(r, x, sy, seg_w, seg_h, 0x50, 0x50, 0x50);
    }

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(value * 100.0f + 0.5f));
    draw_text(r, font, pct, sx, sy + seg_h + sc(4, win_w), sc(60, win_w),
              0xff, 0xff, 0xff);
}

static void draw_volume_bar(SDL_Renderer *r, TTF_Font *font,
                            const Player *p, const Theme *t, int win_w) {
    int pad   = sc(8, win_w);
    int seg_w = sc(14, win_w);
    int gap   = sc(3,  win_w);
    int box_w = 10 * (seg_w + gap) - gap + pad * 4 + sc(46, win_w);
    int box_x = win_w - box_w - pad * 2;
    draw_indicator_bar(r, font, t, "VOL", p->volume,
                       box_x, statusbar_height(win_w) + pad, win_w);
}

static void draw_brightness_bar(SDL_Renderer *r, TTF_Font *font,
                                const Player *p, const Theme *t, int win_w) {
    int pad = sc(8, win_w);
    draw_indicator_bar(r, font, t, "BRI", p->brightness,
                       pad * 2, statusbar_height(win_w) + pad, win_w);
}

/* Centred toast pill — used for speed, audio track, wake */
static void draw_toast(SDL_Renderer *r, TTF_Font *font, const Theme *t,
                       const char *label, int win_w, int y) {
    int lw = 0, lh = 0;
    TTF_SizeUTF8(font, label, &lw, &lh);
    int hpad = sc(16, win_w);
    int vpad = sc(8,  win_w);
    int bw = lw + hpad * 2;
    int bh = lh + vpad * 2;
    int bx = (win_w - bw) / 2;
    fill_rounded_rect(r, bx, y, bw, bh, sc(12, win_w),
                      t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 230);
    draw_text(r, font, label, bx + hpad, y + vpad, lw,
              t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
}

/* -------------------------------------------------------------------------
 * Main draw
 * ---------------------------------------------------------------------- */

void player_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                 const Player *p, const Theme *t, int win_w, int win_h,
                 int show_statusbar,
                 double book_offset, double book_total_dur,
                 const float *book_tick_fracs, int book_n_ticks) {
    SDL_SetRenderDrawColor(r, t->background.r, t->background.g, t->background.b, 0xff);
    SDL_RenderClear(r);

    /* Cover art — center-cropped square, centred in the content area */
    int osd_h = sc(80, win_w);
    int content_h = win_h - osd_h;
    if (p->cover_tex) {
        int max_w = win_w  - sc(32, win_w);
        int max_h = content_h - sc(16, win_w);
        int side  = (max_w < max_h) ? max_w : max_h;
        SDL_Rect dst = { (win_w     - side) / 2,
                         (content_h - side) / 2,
                         side, side };
        int tw, th;
        SDL_QueryTexture(p->cover_tex, NULL, NULL, &tw, &th);
        SDL_Rect src;
        int use_src = 0;
        if (tw > th) { src = (SDL_Rect){ (tw-th)/2, 0,       th, th }; use_src = 1; }
        else if (th > tw) { src = (SDL_Rect){ 0, (th-tw)/2, tw, tw }; use_src = 1; }
        SDL_RenderCopy(r, p->cover_tex, use_src ? &src : NULL, &dst);
    } else {
        /* No cover — draw title + codec info as text */
        int lh  = TTF_FontHeight(font);
        int pad = sc(12, win_w);
        int y   = content_h / 4;

        /* Book/file name */
        const char *slash = strrchr(p->path, '/');
        const char *name  = slash ? slash + 1 : p->path;
        /* Strip extension from display name */
        char disp[1024];
        snprintf(disp, sizeof(disp), "%s", name);
        char *dot = strrchr(disp, '.');
        if (dot) *dot = '\0';

        int name_w = 0, dummy = 0;
        TTF_SizeUTF8(font, disp, &name_w, &dummy);
        int nx = (win_w - name_w) / 2;
        if (nx < pad) nx = pad;
        draw_text(r, font, disp, nx, y, win_w - pad * 2,
                  t->text.r, t->text.g, t->text.b);
        y += lh + pad;

        char info[128];
        int n_ch = demux_chapter_count(&p->demux);
        if (n_ch > 0)
            snprintf(info, sizeof(info), "%s  %d Hz  %d ch  (%d chapters)",
                     p->probe.audio_codec, p->probe.sample_rate,
                     p->probe.channels, n_ch);
        else
            snprintf(info, sizeof(info), "%s  %d Hz  %d ch",
                     p->probe.audio_codec, p->probe.sample_rate, p->probe.channels);
        int iw = 0;
        TTF_SizeUTF8(font, info, &iw, &dummy);
        draw_text(r, font, info, (win_w - iw) / 2, y, win_w - pad * 2,
                  t->secondary.r, t->secondary.g, t->secondary.b);
    }

    /* OSD (progress bar + hints) — always visible for audio-only */
    draw_osd(r, font, font_small, p, t, win_w, win_h, book_offset, book_total_dur,
             book_tick_fracs, book_n_ticks);

    /* Status bar */
    if (show_statusbar)
        statusbar_draw(r, font, t, win_w, win_h);

    /* Brightness dimming overlay */
    if (p->brightness < 0.999f) {
        Uint8 alpha = (Uint8)((1.0f - p->brightness) * 220.0f);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, alpha);
        SDL_Rect full = { 0, 0, win_w, win_h };
        SDL_RenderFillRect(r, &full);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }

    /* Volume bar */
#ifdef SB_TRIMUI_BRICK
    if (!g_hw_has_volume_osd && p->vol_osd_visible)
#else
    if (p->vol_osd_visible)
#endif
        draw_volume_bar(r, font, p, t, win_w);

    /* Brightness bar */
    if (p->bri_osd_visible)
        draw_brightness_bar(r, font, p, t, win_w);

    /* Toast OSD: speed, audio track, wake — stacked below statusbar */
    int sbar_h  = statusbar_height(win_w);
    int pad     = sc(8, win_w);
    int lh      = TTF_FontHeight(font);
    int row_h   = lh + pad * 2 + sc(4, win_w);
    int osd_row = 0;

    if (p->speed_osd_visible) {
        char label[32];
        float spd = SPEED_PRESETS[p->speed_idx];
        if (spd == 1.0f)
            snprintf(label, sizeof(label), "Speed: 1x");
        else
            snprintf(label, sizeof(label), "Speed: %.2fx", (double)spd);
        draw_toast(r, font, t, label, win_w,
                   sbar_h + pad * 2 + osd_row * row_h);
        osd_row++;
    }

    if (p->audio_osd_visible) {
        draw_toast(r, font, t, p->audio_osd_label, win_w,
                   sbar_h + pad * 2 + osd_row * row_h);
        osd_row++;
    }

    if (p->sleep_osd_visible) {
        draw_toast(r, font, t, p->sleep_osd_label, win_w,
                   sbar_h + pad * 2 + osd_row * row_h);
        osd_row++;
    }

    if (p->wake_osd_visible) {
        draw_toast(r, font, t, "Resuming\xe2\x80\xa6", win_w,
                   (win_h - lh) / 2);
    }
}
