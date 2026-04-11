#pragma once
#include <SDL2/SDL.h>

/* -------------------------------------------------------------------------
 * Color + theme definitions
 * ---------------------------------------------------------------------- */

typedef struct {
    Uint8 r, g, b;
} RGB;

typedef struct {
    const char *name;
    RGB background;
    RGB text;
    RGB secondary;
    RGB highlight_bg;
    RGB highlight_text;
    RGB statusbar_fg;
    /* Cover art SVG recoloring (default_cover.svg) */
    RGB cover_body;
    RGB cover_tab;
    RGB cover_shadow;
    RGB cover_screen;
    RGB cover_play;
    /* Folder cover SVG recoloring (default_folder.svg) */
    RGB folder_tab;
    RGB folder_screen;
    RGB folder_body;
} Theme;

extern const Theme THEMES[];
extern const int   THEME_COUNT;

/* -------------------------------------------------------------------------
 * Active theme
 * ---------------------------------------------------------------------- */

const Theme *theme_get(void);
int          theme_set(const char *name);   /* returns 0 if not found */
void         theme_cycle(void);

/* -------------------------------------------------------------------------
 * Config
 * ---------------------------------------------------------------------- */

void config_save(const char *path);

int  config_firstrun_done(void);
void config_set_firstrun_done(void);

/* Google Books API key (empty string if not set). */
const char *config_books_api_key(void);
void        config_set_books_api_key(const char *key);

/* Browser layout preferences (0=LARGE, 1=SMALL, 2=LIST, 3=SHOWCASE). */
int  config_get_layout(void);
void config_set_layout(int l);
int  config_get_season_layout(void);
void config_set_season_layout(int l);

/* -------------------------------------------------------------------------
 * storyboy.conf
 * ---------------------------------------------------------------------- */

void config_load(const char *path);

/* Load API keys from resources/api/ directory.
   Reads GoogleBooks_API.txt.
   Only sets a key if not already set (storyboy.conf takes priority). */
void config_load_api_keys(const char *api_dir);

/* -------------------------------------------------------------------------
 * Default cover art — SVG recolored + rasterized per theme
 * ---------------------------------------------------------------------- */

#define COVER_SIZE 256

SDL_Texture *theme_render_cover(SDL_Renderer *renderer, const char *svg_path);
SDL_Texture *theme_render_folder_cover(SDL_Renderer *renderer, const char *svg_path);
