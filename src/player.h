#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "decoder.h"
#include "audio.h"
#include "theme.h"

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} PlayerState;

/* Playback speed presets (pitch-preserving via atempo filter) */
#define SPEED_PRESET_COUNT 4
extern const float SPEED_PRESETS[SPEED_PRESET_COUNT]; /* 1.0, 1.25, 1.5, 2.0 */

typedef struct {
    DemuxCtx    demux;
    AudioCtx    audio;
    ProbeInfo   probe;
    PlayerState state;
    char        path[1024];

    /* Set to 1 when audio EOS sentinel is received */
    int          eos;

    /* OSD visibility */
    int          osd_visible;
    Uint32       osd_hide_at;  /* SDL_GetTicks() value when OSD should hide */

    float        volume;           /* 0.0–1.0 */

    /* Brightness — 1.0 = full, 0.0 = black (SDL overlay dimming) */
    float        brightness;
    int          bri_osd_visible;
    Uint32       bri_osd_hide_at;

    /* Audio track OSD */
    int          audio_osd_visible;
    Uint32       audio_osd_hide_at;
    char         audio_osd_label[64];

    /* Playback speed */
    int          speed_idx;          /* index into SPEED_PRESETS */
    int          speed_osd_visible;
    Uint32       speed_osd_hide_at;

    /* Sleep timer */
    int          sleep_armed;        /* 1 = timer running */
    Uint32       sleep_end_ms;       /* SDL_GetTicks() value when timer fires */
    int          sleep_osd_visible;
    Uint32       sleep_osd_hide_at;
    char         sleep_osd_label[32];

    /* Screensaver — tracks last user activity time */
    Uint32       last_activity_ms;   /* SDL_GetTicks() of last user input */

    /* Sleep/wake resume toast */
    int          wake_osd_visible;
    Uint32       wake_osd_hide_at;

    /* Cover art texture — loaded by player_open, reloaded when async fetch completes */
    SDL_Texture    *cover_tex;
    SDL_Renderer   *renderer;    /* stored for async cover reload */
    char            cover_book_dir[1024]; /* dir to poll for cover.jpg */
} Player;

/* Open file, initialise demux + audio. Does not start playback. */
int  player_open  (Player *p, const char *path, SDL_Renderer *renderer,
                   char *errbuf, int errbuf_sz);

void player_play  (Player *p);
void player_pause (Player *p);
void player_resume(Player *p);

/* Call once per frame in the main loop.  Updates OSD timers and EOS flag. */
int  player_update(Player *p);

/* Stop and release all resources. Safe to call multiple times. */
void player_close (Player *p);

/* Seek by delta_sec relative to current position. Shows OSD. */
void player_seek   (Player *p, double delta_sec);

/* Seek to an absolute position in seconds. Does NOT show the OSD. */
void player_seek_to(Player *p, double pos_sec);

/* Show the OSD for 3 seconds. */
void player_show_osd(Player *p);

/* Volume control — step is 0.1; clamps to [0.0, 1.0]. */
void player_set_volume  (Player *p, float vol);
void player_volume_up   (Player *p);
void player_volume_dn   (Player *p);

/* Brightness control — step is 0.1; clamps to [0.1, 1.0]. */
void player_brightness_up(Player *p);
void player_brightness_dn(Player *p);

/* Cycle to the next audio track (no-op if only one track). */
void player_cycle_audio(Player *p);

/* Cycle playback speed: 1x → 1.25x → 1.5x → 2x → 1x. Shows OSD toast. */
void player_cycle_speed(Player *p);

/* Set speed to a specific preset index (0 = 1x). Shows OSD toast. */
void player_set_speed  (Player *p, int idx);

/* Set sleep timer.  minutes=0 cancels.  Shows status toast. */
void player_set_sleep_timer(Player *p, int seconds);

/* Returns 1 if the sleep timer has fired (armed and now past deadline). */
int  player_sleep_expired(const Player *p);

/* Record user activity (resets screensaver countdown). */
void player_reset_activity(Player *p);

/* Returns 1 if screensaver should be active (no activity for timeout_ms). */
int  player_screensaver_active(const Player *p, Uint32 timeout_ms);

/* Chapter navigation (no-op if file has no chapters). */
void player_next_chapter(Player *p);
void player_prev_chapter(Player *p);

/* Show "Resuming…" toast for 2s — call on sleep/wake recovery. */
void player_show_wake_toast(Player *p);

/* Render the player screen to the window.
   show_statusbar: if non-zero, draw the status bar before the brightness
   overlay so it is dimmed uniformly with the rest of the frame.
   book_offset/book_total_dur: for multi-file books, pass the cumulative
   offset and total duration so the progress bar reflects the full book.
   Pass 0/0 for single-file playback. */
void player_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                 const Player *p, const Theme *theme, int win_w, int win_h,
                 int show_statusbar,
                 double book_offset, double book_total_dur,
                 const float *book_tick_fracs, int book_n_ticks);
