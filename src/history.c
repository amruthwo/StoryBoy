#include "history.h"
#include "hintbar.h"
#include "decoder.h"   /* format_duration */
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Layout constants
 * ---------------------------------------------------------------------- */

#define TITLE_H    32
#define HEADER_H   26
#define HINT_BAR_H 24
#define PADDING     8
#define BAR_H       4    /* height of the progress bar strip */

/* Entry height is computed from font metrics in history_draw() and cached here
 * so the geometry helpers can use it without needing font parameters. */
static int s_entry_h = 62;  /* default matches original fixed value */

/* -------------------------------------------------------------------------
 * Internal geometry helpers
 * ---------------------------------------------------------------------- */

/* Absolute Y within the scrollable content area for navigable index i. */
static int entry_abs_y(const HistoryState *h, int nav_idx) {
    int y = 0;
    if (h->in_progress_count > 0) {
        y += HEADER_H;
        if (nav_idx < h->in_progress_count)
            return y + nav_idx * s_entry_h;
        y += h->in_progress_count * s_entry_h;
        nav_idx -= h->in_progress_count;
    }
    if (h->completed_count > 0)
        y += HEADER_H;
    return y + nav_idx * s_entry_h;
}

/* Total height of the scrollable content area. */
static int total_content_h(const HistoryState *h) {
    int h_px = 0;
    if (h->in_progress_count > 0)
        h_px += HEADER_H + h->in_progress_count * s_entry_h;
    if (h->completed_count > 0)
        h_px += HEADER_H + h->completed_count * s_entry_h;
    return h_px;
}

/* Adjust scroll_y so that the selected entry is fully visible. */
static void clamp_scroll(HistoryState *h, int content_h) {
    int total = history_total(h);
    if (total == 0) { h->scroll_y = 0; return; }

    int sel = h->selected;
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;

    int abs_y = entry_abs_y(h, sel);

    if (abs_y < h->scroll_y)
        h->scroll_y = abs_y;
    if (abs_y + s_entry_h > h->scroll_y + content_h)
        h->scroll_y = abs_y + s_entry_h - content_h;

    int max_scroll = total_content_h(h) - content_h;
    if (max_scroll < 0) max_scroll = 0;
    if (h->scroll_y > max_scroll) h->scroll_y = max_scroll;
    if (h->scroll_y < 0)          h->scroll_y = 0;
}

/* -------------------------------------------------------------------------
 * Drawing helpers (mirrors browser.c style)
 * ---------------------------------------------------------------------- */

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text,
                      int x, int y, int max_w,
                      Uint8 R, Uint8 G, Uint8 B) {
    if (!text || text[0] == '\0') return;
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

static Uint8 dim(Uint8 v, int amt) { return (v > amt) ? (Uint8)(v - amt) : 0; }
static Uint8 bright(Uint8 v, int amt) { return (v + amt < 255) ? (Uint8)(v + amt) : 255; }

/* Extract basename from a path ("/a/b/c.mkv" -> "c.mkv") */
static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Extract parent folder name ("/a/b/c.mkv" -> "b") */
static void folder_name_of(const char *path, char *out, int out_sz) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    if (!slash) { strncpy(out, ".", (size_t)out_sz); return; }
    *slash = '\0';
    slash = strrchr(tmp, '/');
    strncpy(out, slash ? slash + 1 : tmp, (size_t)out_sz);
    out[out_sz - 1] = '\0';
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void history_load(HistoryState *h) {
    memset(h, 0, sizeof(*h));
    h->in_progress_count = resume_load_all(&h->in_progress);
    h->completed_count   = resume_load_completed(&h->completed);
}

void history_free(HistoryState *h) {
    free(h->in_progress);
    free(h->completed);
    memset(h, 0, sizeof(*h));
}

void history_draw(SDL_Renderer *renderer, TTF_Font *font,
                  TTF_Font *font_small, HistoryState *h,
                  const Theme *theme, int win_w, int win_h) {
    const Theme *t = theme;

    /* Compute entry height from actual font metrics so layout scales with
     * font size (important on larger screens like the Trimui Brick). */
    s_entry_h = PADDING + TTF_FontHeight(font) + 2
                + TTF_FontHeight(font_small) + BAR_H + PADDING + 4;

    /* Keep scroll in sync with selection before rendering */
    int content_h = win_h - TITLE_H - HINT_BAR_H;
    clamp_scroll(h, content_h);

    /* Background */
    SDL_SetRenderDrawColor(renderer,
        t->background.r, t->background.g, t->background.b, 0xff);
    SDL_RenderClear(renderer);

    /* ---- Title bar ---- */
    fill_rect(renderer, 0, 0, win_w, TITLE_H,
              dim(t->background.r, 18),
              dim(t->background.g, 18),
              dim(t->background.b, 18), 0xff);
    draw_text(renderer, font, "HISTORY",
              PADDING, (TITLE_H - TTF_FontHeight(font)) / 2,
              win_w - PADDING * 2,
              t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b);

    /* ---- Content area (clipped between title and hint bar) ---- */
    int content_top = TITLE_H;

    /* Enable clipping so entries don't bleed into title/hint bars */
    SDL_Rect clip = { 0, content_top, win_w, content_h };
    SDL_RenderSetClipRect(renderer, &clip);

    int total = history_total(h);

    if (total == 0) {
        draw_text(renderer, font, "No history yet.",
                  PADDING, content_top + PADDING,
                  win_w - PADDING * 2,
                  t->secondary.r, t->secondary.g, t->secondary.b);
    } else {
        /* Current scroll offset: entries start at content_top - scroll_y */
        int base_y = content_top - h->scroll_y;

        int y = base_y;

        /* ---- IN PROGRESS section ---- */
        if (h->in_progress_count > 0) {
            /* Section header */
            fill_rect(renderer, 0, y, win_w, HEADER_H,
                      dim(t->background.r, 10),
                      dim(t->background.g, 10),
                      dim(t->background.b, 10), 0xff);
            draw_text(renderer, font_small, "IN PROGRESS",
                      PADDING, y + (HEADER_H - TTF_FontHeight(font_small)) / 2,
                      win_w - PADDING * 2,
                      t->secondary.r, t->secondary.g, t->secondary.b);
            y += HEADER_H;

            for (int i = 0; i < h->in_progress_count; i++) {
                /* Skip entries fully above the viewport */
                if (y + s_entry_h <= content_top) { y += s_entry_h; continue; }
                /* Stop when below viewport */
                if (y >= content_top + content_h) break;

                int nav_idx = i;
                int sel     = (nav_idx == h->selected);
                const ResumeEntry *e = &h->in_progress[i];

                /* Row background */
                if (sel) {
                    fill_rect(renderer, PADDING, y,
                              win_w - PADDING * 2, s_entry_h,
                              t->highlight_bg.r, t->highlight_bg.g,
                              t->highlight_bg.b, 0xff);
                } else if (i % 2 == 0) {
                    fill_rect(renderer, PADDING, y,
                              win_w - PADDING * 2, s_entry_h,
                              dim(t->background.r, 8),
                              dim(t->background.g, 8),
                              dim(t->background.b, 8), 0xff);
                }

                RGB tc = sel ? t->highlight_text : t->text;
                int fname_y = y + PADDING;
                int row_w   = win_w - PADDING * 4;

                /* Filename */
                draw_text(renderer, font, basename_of(e->path),
                          PADDING * 2, fname_y, row_w,
                          tc.r, tc.g, tc.b);

                /* Folder + time info on second line.
                   For multi-file books book_pos > 0 and is the absolute
                   position from the book start; use it instead of pos_in_file. */
                char folder[128], pos_s[16], dur_s[16], info[192];
                folder_name_of(e->path, folder, sizeof(folder));
                double display_pos = (e->book_pos > 0.0) ? e->book_pos : e->position;
                format_duration(display_pos, pos_s, sizeof(pos_s));

                if (e->duration > 0.0) {
                    format_duration(e->duration, dur_s, sizeof(dur_s));
                    int pct = (int)(display_pos / e->duration * 100.0);
                    if (pct > 100) pct = 100;
                    snprintf(info, sizeof(info), "%s  %s / %s  (%d%%)",
                             folder, pos_s, dur_s, pct);
                } else {
                    snprintf(info, sizeof(info), "%s  %s", folder, pos_s);
                }

                int info_y = y + PADDING + TTF_FontHeight(font) + 2;
                draw_text(renderer, font_small, info,
                          PADDING * 2, info_y, row_w,
                          sel ? dim(tc.r, 20) : t->secondary.r,
                          sel ? dim(tc.g, 20) : t->secondary.g,
                          sel ? dim(tc.b, 20) : t->secondary.b);

                /* Progress bar */
                int bar_y = y + s_entry_h - BAR_H - 3;
                int bar_w = win_w - PADDING * 4;
                /* Background track */
                fill_rect(renderer, PADDING * 2, bar_y, bar_w, BAR_H,
                          dim(t->background.r, 15),
                          dim(t->background.g, 15),
                          dim(t->background.b, 15), 0xff);
                /* Filled portion */
                if (e->duration > 0.0) {
                    double pct = display_pos / e->duration;
                    if (pct > 1.0) pct = 1.0;
                    int filled = (int)(bar_w * pct);
                    if (filled > 0) {
                        Uint8 br = sel ? bright(t->highlight_bg.r, 30)
                                       : t->highlight_bg.r;
                        Uint8 bg = sel ? bright(t->highlight_bg.g, 30)
                                       : t->highlight_bg.g;
                        Uint8 bb = sel ? bright(t->highlight_bg.b, 30)
                                       : t->highlight_bg.b;
                        fill_rect(renderer, PADDING * 2, bar_y, filled, BAR_H,
                                  br, bg, bb, 0xff);
                    }
                }

                y += s_entry_h;
            }
        }

        /* ---- COMPLETED section ---- */
        if (h->completed_count > 0) {
            /* Section header */
            fill_rect(renderer, 0, y, win_w, HEADER_H,
                      dim(t->background.r, 10),
                      dim(t->background.g, 10),
                      dim(t->background.b, 10), 0xff);
            draw_text(renderer, font_small, "COMPLETED",
                      PADDING, y + (HEADER_H - TTF_FontHeight(font_small)) / 2,
                      win_w - PADDING * 2,
                      t->secondary.r, t->secondary.g, t->secondary.b);
            y += HEADER_H;

            for (int i = 0; i < h->completed_count; i++) {
                if (y + s_entry_h <= content_top) { y += s_entry_h; continue; }
                if (y >= content_top + content_h) break;

                int nav_idx = h->in_progress_count + i;
                int sel     = (nav_idx == h->selected);
                const char *path = h->completed[i];

                /* Row background */
                if (sel) {
                    fill_rect(renderer, PADDING, y,
                              win_w - PADDING * 2, s_entry_h,
                              t->highlight_bg.r, t->highlight_bg.g,
                              t->highlight_bg.b, 0xff);
                } else if (i % 2 == 0) {
                    fill_rect(renderer, PADDING, y,
                              win_w - PADDING * 2, s_entry_h,
                              dim(t->background.r, 8),
                              dim(t->background.g, 8),
                              dim(t->background.b, 8), 0xff);
                }

                RGB tc = sel ? t->highlight_text : t->text;
                int row_w = win_w - PADDING * 4;

                /* Filename */
                draw_text(renderer, font, basename_of(path),
                          PADDING * 2, y + PADDING, row_w,
                          tc.r, tc.g, tc.b);

                /* Folder name + "Watched" label */
                char folder[128], info[160];
                folder_name_of(path, folder, sizeof(folder));
                snprintf(info, sizeof(info), "%s  \xe2\x9c\x93 Watched", folder);
                draw_text(renderer, font_small, info,
                          PADDING * 2,
                          y + PADDING + TTF_FontHeight(font) + 2,
                          row_w,
                          sel ? dim(tc.r, 20) : t->secondary.r,
                          sel ? dim(tc.g, 20) : t->secondary.g,
                          sel ? dim(tc.b, 20) : t->secondary.b);

                y += s_entry_h;
            }
        }
    }

    SDL_RenderSetClipRect(renderer, NULL);

    /* ---- Hint bar ---- */
    if (total > 0) {
        static const HintItem hints_with[] = {
            { "A", "Play" }, { "B", "Back" }, { "SEL", "Remove" }
        };
        hintbar_draw_row(renderer, font, font_small, hints_with, 3, t, win_w, win_h);
    } else {
        static const HintItem hints_empty[] = { { "B", "Back" } };
        hintbar_draw_row(renderer, font, font_small, hints_empty, 1, t, win_w, win_h);
    }
}

int history_handle_event(HistoryState *h, const SDL_Event *ev) {
    if (ev->type != SDL_KEYDOWN) return 0;

    SDL_Keycode key = ev->key.keysym.sym;
    int total = history_total(h);

    /* Back */
    if (key == SDLK_LCTRL || key == SDLK_BACKSPACE || key == SDLK_LALT) {
        h->action = HISTORY_ACTION_BACK;
        return 1;
    }

    /* Remove selected entry */
    if (key == SDLK_RCTRL) {
        if (total == 0) return 1;
        int idx = h->selected;
        if (idx < h->in_progress_count) {
            snprintf(h->action_path, sizeof(h->action_path),
                     "%s", h->in_progress[idx].path);
        } else {
            idx -= h->in_progress_count;
            snprintf(h->action_path, sizeof(h->action_path),
                     "%s", h->completed[idx]);
        }
        h->action = HISTORY_ACTION_CLEAR;
        return 1;
    }

    /* Navigate */
    if (key == SDLK_UP) {
        if (h->selected > 0) h->selected--;
        return 1;
    }
    if (key == SDLK_DOWN) {
        if (h->selected < total - 1) h->selected++;
        return 1;
    }

    /* Play selected entry */
    if (key == SDLK_RETURN || key == SDLK_SPACE) {
        if (total == 0) return 1;
        int idx = h->selected;
        if (idx < h->in_progress_count) {
            snprintf(h->action_path, sizeof(h->action_path),
                     "%s", h->in_progress[idx].path);
        } else {
            idx -= h->in_progress_count;
            snprintf(h->action_path, sizeof(h->action_path),
                     "%s", h->completed[idx]);
        }
        h->action = HISTORY_ACTION_PLAY;
        return 1;
    }

    return 0;
}
