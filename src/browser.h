#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "filebrowser.h"
#include "theme.h"

/* -------------------------------------------------------------------------
 * Layout & view modes
 * ---------------------------------------------------------------------- */

typedef enum {
    LAYOUT_LARGE = 0,   /* 2-column grid, big tiles           */
    LAYOUT_SMALL,       /* 4-column grid, small tiles         */
    LAYOUT_LIST,        /* single-column list                 */
    LAYOUT_SHOWCASE,    /* carousel: one large cover, peeks   */
    LAYOUT_COUNT
} BrowserLayout;

typedef enum {
    VIEW_FOLDERS = 0,   /* top-level: grid of shows / flat folders */
    VIEW_SEASONS,       /* show's season list (skipped for single-season shows) */
    VIEW_FILES,         /* episode list inside a season or flat folder */
} BrowserView;

typedef enum {
    BROWSER_ACTION_NONE = 0,
    BROWSER_ACTION_PLAY,           /* user selected a file — path in action_path */
    BROWSER_ACTION_QUIT,
    BROWSER_ACTION_LAYOUT_CHANGED, /* SEL pressed — caller should persist layout pref */
    BROWSER_ACTION_FETCH_ART,      /* Y pressed — fetch cover art for selected folder */
} BrowserAction;

/* -------------------------------------------------------------------------
 * Browser state
 * ---------------------------------------------------------------------- */

typedef struct {
    BrowserView   view;
    BrowserLayout layout;         /* VIEW_FOLDERS layout */
    BrowserLayout season_layout;  /* VIEW_SEASONS layout (independent) */
    int           selected;     /* highlighted index in current view         */
    int           scroll_row;   /* first visible row (grid) or item (list)   */
    int           folder_idx;   /* which folder is open (VIEW_SEASONS/FILES) */
    int           season_idx;   /* which season is open (VIEW_FILES in show) */
    int           season_scroll; /* scroll offset for season list             */

    /* set when action == BROWSER_ACTION_PLAY */
    char          action_path[1024];
    BrowserAction action;

    /* Per-file progress cache — rebuilt when folder/season changes.
       Values: -1.0 = no data, 0.0–1.0 = progress ratio (>=0.95 = completed). */
    float        *file_progress;
    int           prog_folder_idx;  /* folder the cache was built for, -1 = invalid */
    int           prog_season_idx;  /* season the cache was built for, -1 = invalid */

    /* B-to-exit confirmation toast */
    int    exit_confirm;      /* 1 = awaiting second B press */
    Uint32 exit_confirm_at;   /* SDL_GetTicks() when confirm was triggered */
} BrowserState;

/* -------------------------------------------------------------------------
 * Cover texture cache — one SDL_Texture* per MediaFolder
 * ---------------------------------------------------------------------- */

typedef struct {
    SDL_Texture **textures;   /* parallel to lib->folders */
    int           count;

    /* Blurred backdrop for VIEW_SEASONS / VIEW_FILES */
    SDL_Texture  *backdrop;
    int           backdrop_idx;  /* folder_idx it was built for; -1 = none */

    /* Season cover thumbnails — rebuilt when entering a different show */
    SDL_Texture **season_textures;
    int           season_tex_count;
    int           season_tex_folder_idx;  /* -1 = none */
} CoverCache;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/* Call after library_scan(). Allocates the cover texture cache slots. */
void browser_init(BrowserState *state, CoverCache *cache, const MediaLibrary *lib);

/* Generate mosaic covers for series whose season covers are already on disk.
   Call once after browser_init(), before the first frame. */
void browser_generate_pending_mosaics(SDL_Renderer *renderer, MediaLibrary *lib,
                                      SDL_Texture *default_cover);

/* Draw current view. Call every frame. */
void browser_draw(SDL_Renderer *renderer, TTF_Font *font, TTF_Font *font_small,
                  BrowserState *state, CoverCache *cache, MediaLibrary *lib,
                  SDL_Texture *default_cover, const Theme *theme,
                  int win_w, int win_h);

/* Handle one SDL event. Returns true if event was consumed. */
int browser_handle_event(BrowserState *state, const MediaLibrary *lib,
                         const SDL_Event *ev);

/* Free cached cover textures (not the MediaLibrary itself). */
void cover_cache_free(CoverCache *cache);

/* Free heap allocations inside BrowserState (call before exit). */
void browser_state_free(BrowserState *state);
