#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "theme.h"

/* Height of the status bar in pixels (scaled to win_w). */
int statusbar_height(int win_w);

/* Draw the status bar at the top of the screen.
   Call AFTER browser_draw / player_draw so it renders on top.
   win_w / win_h are the full window dimensions (not the content area). */
void statusbar_draw(SDL_Renderer *renderer, TTF_Font *font,
                    const Theme *theme, int win_w, int win_h);
