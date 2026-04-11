#pragma once
/* -------------------------------------------------------------------------
 * a30_screen.h — Miyoo A30 framebuffer render path + evdev input
 *
 * The A30 runs SpruceOS with SDL_VIDEODRIVER=dummy, so SDL2 renders into a
 * CPU-side SDL_Surface.  a30_flip() writes that surface to /dev/fb0 with a
 * 90° CCW rotation (the panel is mounted in landscape, but we run 640×480
 * portrait-style; the hardware compositor expects a 480×640 portrait write).
 *
 * a30_poll_events() reads /dev/input/event3 (O_NONBLOCK) and injects
 * SDL2 keyboard events so the rest of StoryBoy needs zero changes.
 * ---------------------------------------------------------------------- */

#ifdef SB_A30

#include <SDL2/SDL.h>
#include "platform.h"

/* Must be called once after SDL_Init.
   Opens /dev/fb0, mmaps the framebuffer, caches yoffset + stride.
   Returns 0 on success, -1 on error (writes message to stderr). */
int  a30_screen_init(void);

/* Blit SDL_Surface to the visible fb0 page with 90° CCW rotation.
   surface must be WIN_W × WIN_H (640×480) ARGB8888.
   Call once per frame, after SDL_RenderPresent. */
void a30_flip(SDL_Surface *surface);

/* Non-blocking: drain /dev/input/event3, inject SDL2 key events.
   Call at the top of each frame's SDL_PollEvent loop. */
void a30_poll_events(void);

/* Release resources (munmap, close fds).  Call on shutdown. */
void a30_screen_close(void);

/* Call immediately after sleep/wake is detected.
   Permanently disables FBIOPAN_DISPLAY for the rest of the session —
   the sunxi driver can block for 100+ seconds after wake waiting for vsync.
   After this call a30_flip() writes directly to the displayed page. */
void a30_screen_wake(void);

#endif /* SB_A30 */
