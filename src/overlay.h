#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "theme.h"

/* -------------------------------------------------------------------------
 * Help overlay — full controls reference, shown when MENU is held ≥ 1 s.
 * Caller is responsible for dismissing (clear overlay_active on keydown).
 * ---------------------------------------------------------------------- */

void help_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
               const Theme *theme, int win_w, int win_h);

/* -------------------------------------------------------------------------
 * First-run tutorial — 6 slides, shown once on first launch.
 * ---------------------------------------------------------------------- */

#define TUTORIAL_SLIDE_COUNT 6

typedef struct {
    int active;   /* 1 while tutorial is on screen */
    int slide;    /* 0 .. TUTORIAL_SLIDE_COUNT-1   */
} TutorialState;

/* Draw the current tutorial slide over the rendered frame. */
void tutorial_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                   const TutorialState *t, const Theme *theme,
                   int win_w, int win_h);

/* Advance one slide. Clears t->active and returns 0 when all slides done.
   Caller should then call config_set_firstrun_done() + config_save(). */
int tutorial_next(TutorialState *t);

/* -------------------------------------------------------------------------
 * Resume-prompt overlay — shown when a saved position is found.
 * path     — basename of the file (for the panel title)
 * pos_sec  — saved position in seconds
 * ---------------------------------------------------------------------- */

void resume_prompt_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                        const Theme *theme, int win_w, int win_h,
                        const char *path, double pos_sec);

/* -------------------------------------------------------------------------
 * Up-Next countdown — shown for 5 s between EOS and auto-advance.
 * path         — basename of the next file
 * seconds_left — countdown value (5 … 0)
 * ---------------------------------------------------------------------- */

void upnext_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                 const Theme *theme, int win_w, int win_h,
                 const char *path, int seconds_left);

/* -------------------------------------------------------------------------
 * Error overlay — shown when a file cannot be opened.
 * path   — the file path that failed
 * errmsg — the error string from player_open
 * ---------------------------------------------------------------------- */

void error_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                const Theme *theme, int win_w, int win_h,
                const char *path, const char *errmsg);

/* -------------------------------------------------------------------------
 * Fetch-art prompt — shown when Y is pressed on a book in the browser.
 * book_name — display name of the selected book/folder
 * ---------------------------------------------------------------------- */

void fetch_art_prompt_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                           const Theme *theme, int win_w, int win_h,
                           const char *book_name);

/* -------------------------------------------------------------------------
 * Shared panel helper — draws a rounded, theme-styled dialog panel.
 * Exported so inline overlays in main.c can match the style.
 * ---------------------------------------------------------------------- */
void overlay_panel(SDL_Renderer *r, int x, int y, int w, int h,
                   const Theme *theme);

