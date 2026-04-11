#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "resume.h"
#include "filebrowser.h"
#include "theme.h"

/* -------------------------------------------------------------------------
 * History page — shows In Progress + Recently Watched lists read from
 * resume.dat and history.dat.
 * ---------------------------------------------------------------------- */

typedef enum {
    HISTORY_ACTION_NONE = 0,
    HISTORY_ACTION_PLAY,    /* play action_path (with normal resume seek) */
    HISTORY_ACTION_BACK,    /* return to browser */
    HISTORY_ACTION_CLEAR,   /* SELECT pressed — caller removes action_path entry then reloads */
} HistoryAction;

typedef struct {
    /* In-progress entries from resume.dat (most-recent first) */
    ResumeEntry  *in_progress;
    int           in_progress_count;

    /* Completed paths from history.dat (most-recent first) */
    char        (*completed)[1024];
    int           completed_count;

    /* Navigation */
    int           selected;  /* 0..(total-1): navigable index across both lists */
    int           scroll_y;  /* pixel scroll offset into the content area */

    /* Set by history_handle_event() */
    HistoryAction action;
    char          action_path[1024];
} HistoryState;

/* Total navigable entries */
static inline int history_total(const HistoryState *h) {
    return h->in_progress_count + h->completed_count;
}

/* Load entries from disk into *h.  Call once when entering history mode. */
void history_load  (HistoryState *h);

/* Free heap memory owned by *h (does not free h itself). */
void history_free  (HistoryState *h);

/* Draw the history page to renderer every frame.
   Also updates h->scroll_y to keep the selection visible. */
void history_draw  (SDL_Renderer *renderer, TTF_Font *font,
                    TTF_Font *font_small, HistoryState *h,
                    const Theme *theme, int win_w, int win_h);

/* Handle one SDL event.  Sets h->action when the user makes a choice.
   Returns 1 if the event was consumed. */
int  history_handle_event(HistoryState *h, const SDL_Event *ev);
