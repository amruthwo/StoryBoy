#pragma once
#ifdef SB_TRIMUI_BRICK
/* -------------------------------------------------------------------------
 * brick_screen.h — Trimui Brick framebuffer + evdev input
 * ---------------------------------------------------------------------- */

#include <SDL2/SDL.h>
#include "platform.h"

/* Panel dimensions — resolved at runtime from g_display_w/h */
#define BRICK_W  g_display_w
#define BRICK_H  g_display_h

int  brick_screen_init(void);
void brick_screen_close(void);

/* UI flip: blit SDL surface directly to fb0 (no rotation — display is landscape) */
void brick_flip(SDL_Surface *surface);

/* Drain evdev events and inject SDL2 keyboard events */
void brick_poll_events(void);

#endif /* SB_TRIMUI_BRICK */
