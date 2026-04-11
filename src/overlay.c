#include "overlay.h"
#include "hintbar.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Scale base pixel value proportionally to the window width (base = 640 ref) */
static inline int sc(int base, int w)  { return (int)(base * w / 640.0f + 0.5f); }
/* Scale base pixel value proportionally to the window height (base = 480 ref) */
static inline int sc_h(int base, int h){ return (int)(base * h / 480.0f + 0.5f); }
/* glyph_h for hint items derived from font height (matches hintbar.c's glyph_pad) */
static inline int gh(TTF_Font *f) { int h = TTF_FontHeight(f); return h + h * 2 / 7; }
/* item gap between hint items (matches hintbar.c's item_gap) */
static inline int ig(TTF_Font *f) { return TTF_FontHeight(f) * 9 / 7; }

/* -------------------------------------------------------------------------
 * Local drawing helpers
 * ---------------------------------------------------------------------- */

static void draw_dim(SDL_Renderer *r, int win_w, int win_h) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_Rect full = { 0, 0, win_w, win_h };
    SDL_RenderFillRect(r, &full);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
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
        SDL_RenderDrawLine(r, x + rad - span,     y + dy,         x + rad - 1,             y + dy);
        SDL_RenderDrawLine(r, x + w - rad,         y + dy,         x + w - rad + span - 1,  y + dy);
        SDL_RenderDrawLine(r, x + rad - span,     y + h - 1 - dy, x + rad - 1,             y + h - 1 - dy);
        SDL_RenderDrawLine(r, x + w - rad,         y + h - 1 - dy, x + w - rad + span - 1,  y + h - 1 - dy);
    }
    if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

static void draw_rounded_outline(SDL_Renderer *r, int x, int y, int w, int h,
                                  int rad, Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawColor(r, R, G, B, 0xff);
    if (rad <= 0) {
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderDrawRect(r, &rect);
        return;
    }
    SDL_RenderDrawLine(r, x + rad,     y,         x + w - 1 - rad, y);
    SDL_RenderDrawLine(r, x + rad,     y + h - 1, x + w - 1 - rad, y + h - 1);
    SDL_RenderDrawLine(r, x,           y + rad,   x,               y + h - 1 - rad);
    SDL_RenderDrawLine(r, x + w - 1,   y + rad,   x + w - 1,       y + h - 1 - rad);
    int f = 1 - rad, dfx = 0, dfy = -2 * rad, px = 0, py = rad;
    while (px <= py) {
        SDL_RenderDrawPoint(r, x + rad - py,         y + rad - px);
        SDL_RenderDrawPoint(r, x + rad - px,         y + rad - py);
        SDL_RenderDrawPoint(r, x + w - 1 - rad + py, y + rad - px);
        SDL_RenderDrawPoint(r, x + w - 1 - rad + px, y + rad - py);
        SDL_RenderDrawPoint(r, x + rad - py,         y + h - 1 - rad + px);
        SDL_RenderDrawPoint(r, x + rad - px,         y + h - 1 - rad + py);
        SDL_RenderDrawPoint(r, x + w - 1 - rad + py, y + h - 1 - rad + px);
        SDL_RenderDrawPoint(r, x + w - 1 - rad + px, y + h - 1 - rad + py);
        px++; dfx += 2; f += dfx + 1;
        if (f >= 0) { py--; dfy += 2; f += dfy; }
    }
}

static void panel_bg(SDL_Renderer *r, int x, int y, int w, int h,
                     const Theme *theme) {
    int rad = sc(12, w);
    if (rad < 12) rad = 12;
    fill_rounded_rect(r, x, y, w, h, rad,
                      theme->background.r, theme->background.g,
                      theme->background.b, 0xff);
    draw_rounded_outline(r, x, y, w, h, rad,
                         theme->highlight_bg.r, theme->highlight_bg.g,
                         theme->highlight_bg.b);
}

void overlay_panel(SDL_Renderer *r, int x, int y, int w, int h,
                   const Theme *theme) {
    panel_bg(r, x, y, w, h, theme);
}

static void divider(SDL_Renderer *r, int x, int y, int w, const Theme *theme) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, theme->secondary.r, theme->secondary.g,
                           theme->secondary.b, 110);
    SDL_RenderDrawLine(r, x, y, x + w - 1, y);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

static void text_ctr(SDL_Renderer *r, TTF_Font *font, const char *text,
                     int cx, int y, Uint8 R, Uint8 G, Uint8 B) {
    SDL_Color col = { R, G, B, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, col);
    if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if (t) {
        SDL_Rect dst = { cx - s->w / 2, y, s->w, s->h };
        SDL_RenderCopy(r, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    SDL_FreeSurface(s);
}

/* -------------------------------------------------------------------------
 * Scrolling text: clips to [x, x+max_w), scrolls if text is wider.
 * Pause 1.5 s → scroll at 40 px/s → pause 1.5 s → repeat.
 * y is the top of the text line.
 * ---------------------------------------------------------------------- */

static void draw_text_scroll(SDL_Renderer *r, TTF_Font *font, const char *text,
                              int x, int y, int max_w,
                              Uint8 R, Uint8 G, Uint8 B) {
    int tw = 0, th = 0;
    TTF_SizeUTF8(font, text, &tw, &th);

    SDL_Color col = { R, G, B, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, col);
    if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if (!t) { SDL_FreeSurface(s); return; }

    int draw_x;
    if (tw <= max_w) {
        draw_x = x + (max_w - tw) / 2;   /* centred */
    } else {
        int scroll_dist = tw - max_w;
        int pause_ms    = 1500;
        int speed       = 40;             /* px per second */
        int scroll_ms   = scroll_dist * 1000 / speed;
        int cycle_ms    = pause_ms + scroll_ms + pause_ms;

        int phase = (int)(SDL_GetTicks() % (Uint32)cycle_ms);
        int offset;
        if (phase < pause_ms) {
            offset = 0;
        } else if (phase < pause_ms + scroll_ms) {
            offset = (phase - pause_ms) * speed / 1000;
        } else {
            offset = scroll_dist;
        }
        draw_x = x - offset;

        SDL_Rect clip = { x, y, max_w, th };
        SDL_RenderSetClipRect(r, &clip);
    }

    SDL_Rect dst = { draw_x, y, s->w, s->h };
    SDL_RenderCopy(r, t, NULL, &dst);

    SDL_RenderSetClipRect(r, NULL);   /* always reset, even if not clipped */
    SDL_DestroyTexture(t);
    SDL_FreeSurface(s);
}

/* -------------------------------------------------------------------------
 * Help overlay — two-column controls reference
 * ---------------------------------------------------------------------- */

void help_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
               const Theme *theme, int win_w, int win_h) {
    /* ↑↓ = U+2191 U+2193 */
    static const HintItem browser_rows[] = {
        { "A",                         "Open / Play"       },
        { "B",                         "Back"              },
        { "\xe2\x86\x91\xe2\x86\x93", "Navigate"          },
        { "SEL",                       "Layout"            },
        { "X",                         "History"           },
        { "MENU",                      "Hold: this screen" },
    };
    /* ←→ = U+2190 U+2192 ;  − = U+2212 */
    static const HintItem playback_rows[] = {
        { "A",                         "Pause / Resume"    },
        { "\xe2\x86\x90\xe2\x86\x92", "\xc2\xb1" "10 s"  },
        { "L1",  "\xe2\x88\x92" "60 s"                    },
        { "R1",                        "+60 s"             },
        { "L2",                        "Prev chapter"      },
        { "R2",                        "Next chapter"      },
        { "X",                         "Sleep timer"       },
        { "START",                     "Speed cycle"       },
        { "SEL",                       "Speed reset"       },
        { "Y \xc3\x97""2",            "Screensaver"       },
        { "\xe2\x86\x91\xe2\x86\x93", "Brightness"        },
        { "B",                         "Stop"              },
    };
    enum { N_BR = 6, N_PB = 12 };

    draw_dim(r, win_w, win_h);

    /* Panel — tall enough for 13 playback rows */
    int px = sc(20, win_w);
    int pw = win_w - px * 2;
    int ph = sc_h(390, win_h);
    int py = (win_h - ph) / 2;
    panel_bg(r, px, py, pw, ph, theme);

    /* Title */
    int fy  = TTF_FontHeight(font);
    int fsy = TTF_FontHeight(font_small);
    int row_h = fsy * 10 / 7;   /* scaled OVL_ROW_H: 20px at base 14px font */
    int ty  = py + sc(10, win_h);
    text_ctr(r, font, "Controls Reference",
             win_w / 2, ty,
             theme->text.r, theme->text.g, theme->text.b);

    int div1y = ty + fy + 6;
    divider(r, px + 8, div1y, pw - 16, theme);

    /* Column headers */
    int half    = (pw - 16) / 2;
    int lcol_cx = px + 8 + half / 2;
    int rcol_cx = px + 8 + half + half / 2;
    int hdry    = div1y + 5;
    text_ctr(r, font_small, "BROWSER",
             lcol_cx, hdry,
             theme->highlight_bg.r, theme->highlight_bg.g, theme->highlight_bg.b);
    text_ctr(r, font_small, "PLAYBACK",
             rcol_cx, hdry,
             theme->highlight_bg.r, theme->highlight_bg.g, theme->highlight_bg.b);

    /* Vertical separator */
    int vsepx = px + 8 + half;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, theme->secondary.r, theme->secondary.g,
                           theme->secondary.b, 80);
    SDL_RenderDrawLine(r, vsepx, div1y + 1, vsepx, py + ph - 26);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    int div2y = hdry + fsy + 4;
    divider(r, px + 8, div2y, pw - 16, theme);

    int cony = div2y + 5;

    /* Alternating row backgrounds across both columns */
    int max_rows = (N_BR > N_PB) ? N_BR : N_PB;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, theme->text.r, theme->text.g, theme->text.b, 14);
    for (int i = 1; i < max_rows; i += 2) {
        SDL_Rect row_bg = { px + 9, cony + i * row_h, pw - 18, row_h };
        SDL_RenderFillRect(r, &row_bg);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    /* Aligned content columns */
    hintbar_draw_column(r, font_small, browser_rows,  N_BR, theme,
                        px + 12,   cony, row_h, 0);
    hintbar_draw_column(r, font_small, playback_rows, N_PB, theme,
                        vsepx + 8, cony, row_h, 0);

    /* Footer */
    int footery = py + ph - sc_h(18, win_h);
    text_ctr(r, font_small, "Press any button to close",
             win_w / 2, footery,
             theme->secondary.r, theme->secondary.g, theme->secondary.b);
}

/* -------------------------------------------------------------------------
 * Tutorial slides
 * ---------------------------------------------------------------------- */

/* A body line is either a glyph+label (btn != NULL) or plain centred text. */
typedef struct {
    const char *btn;    /* NULL = plain text, drawn centred in secondary color */
    const char *text;
} TutLine;

typedef struct {
    const char *title;
    TutLine     body[5];
    int         n_body;
    const char *action;   /* label next to the A glyph at the bottom */
} Slide;

static const Slide SLIDES[TUTORIAL_SLIDE_COUNT] = {
    {
        "Welcome to StoryBoy",
        {
            { NULL, "An audiobook player for handheld devices." },
            { NULL, "This quick tour covers the main controls." },
        },
        2, "Continue"
    },
    {
        "Browsing Your Library",
        {
            /* ↑↓ = U+2191 U+2193 */
            { "\xe2\x86\x91\xe2\x86\x93", "Navigate folders and files"  },
            { "A",   "Open folder or start playback"  },
            { "B",   "Return to folder list"          },
            { "SEL", "Cycle tile layout"               },
            { "X",   "Open listen history"            },
        },
        5, "Continue"
    },
    {
        "Playback Controls",
        {
            { "A",                         "Pause / resume"           },
            /* ←→ = U+2190 U+2192 ;  ± = U+00B1 */
            { "\xe2\x86\x90\xe2\x86\x92", "Skip \xc2\xb1""10 s"      },
            { "L1",                        "\xe2\x88\x92""60 s"       },
            { "R1",                        "+60 s"                    },
            { "B",                         "Stop & return to browser" },
        },
        5, "Continue"
    },
    {
        "Chapters & Speed",
        {
            { "L2",    "Previous chapter (or \xe2\x88\x92""15 min)"  },
            { "R2",    "Next chapter (or +15 min)"                   },
            { "START", "Cycle speed: 1\xc3\x97 \xe2\x86\x92 2\xc3\x97" },
            { "SEL",   "Reset speed to 1\xc3\x97"                   },
        },
        4, "Continue"
    },
    {
        "Sleep & Screensaver",
        {
            { "X",            "Sleep timer: off / 10 / 30 / 60 / 120 min" },
            { "Y \xc3\x97""2", "Toggle screensaver"                       },
            { NULL,           "Progress is saved automatically."          },
        },
        3, "Continue"
    },
    {
        "That's It!",
        {
            { NULL, "Hold MENU (1 second) for the controls reference." },
            { NULL, "Enjoy your audiobook!"                            },
        },
        2, "Start"
    },
};

void tutorial_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                   const TutorialState *t, const Theme *theme,
                   int win_w, int win_h) {
    /* Use the larger font throughout the tutorial for readability */
    TTF_Font *bf = font;   /* body font — 16 px */
    (void)font_small;

    draw_dim(r, win_w, win_h);

    int px = sc(40, win_w);
    int pw = win_w - px * 2;
    int ph = sc_h(290, win_h);
    int py = (win_h - ph) / 2;
    panel_bg(r, px, py, pw, ph, theme);

    /* Slide-indicator dots */
    int dsz  = sc(8, win_w);
    int dgap = sc(18, win_w);
    int dw   = TUTORIAL_SLIDE_COUNT * dsz + (TUTORIAL_SLIDE_COUNT - 1) * (dgap - dsz);
    int dx   = (win_w - dw) / 2;
    int dy   = py + 12;
    for (int i = 0; i < TUTORIAL_SLIDE_COUNT; i++) {
        int act = (i == t->slide);
        Uint8 R = act ? theme->highlight_bg.r : theme->secondary.r;
        Uint8 G = act ? theme->highlight_bg.g : theme->secondary.g;
        Uint8 B = act ? theme->highlight_bg.b : theme->secondary.b;
        SDL_SetRenderDrawColor(r, R, G, B, 255);
        SDL_Rect dot = { dx + i * dgap, dy, dsz, dsz };
        SDL_RenderFillRect(r, &dot);
    }

    /* Title */
    const Slide *s   = &SLIDES[t->slide];
    int fy   = TTF_FontHeight(font);
    int fby  = TTF_FontHeight(bf);     /* body font height */
    int tity = dy + dsz + 10;
    text_ctr(r, font, s->title, win_w / 2, tity,
             theme->text.r, theme->text.g, theme->text.b);

    int divty = tity + fy + 6;
    divider(r, px + 16, divty, pw - 32, theme);

    /* Measure glyph slot width from all glyph lines so labels align */
    int glyph_h = gh(bf);
    int lh      = fby + fby * 4 / 7;  /* row height ≈ fby * 1.57 */
    int slot_w  = 0;
    int max_lw  = 0;
    for (int i = 0; i < s->n_body; i++) {
        if (!s->body[i].text) continue;
        if (s->body[i].btn) {
            int gw = hintbar_glyph_w(bf, s->body[i].btn, glyph_h);
            if (gw > slot_w) slot_w = gw;
            int lw = 0, dummy = 0;
            TTF_SizeUTF8(bf, s->body[i].text, &lw, &dummy);
            if (lw > max_lw) max_lw = lw;
        }
    }

    /* Centre the glyph+label block horizontally */
    int gap      = fby * 5 / 14;   /* GLYPH_LABEL_GAP, proportional to font */
    int block_w  = (slot_w > 0) ? slot_w + gap + max_lw : 0;
    int start_x  = (block_w > 0) ? (win_w - block_w) / 2 : win_w / 2;

    /* Draw body lines */
    int by = divty + 12;
    for (int i = 0; i < s->n_body; i++) {
        const TutLine *line = &s->body[i];
        if (!line->text) { by += lh / 2; continue; }

        if (line->btn) {
            HintItem hi = { line->btn, line->text };
            hintbar_draw_column(r, bf, &hi, 1, theme, start_x, by, lh, slot_w);
        } else {
            /* Plain note — centred, secondary colour */
            int ty = by + (lh - fby) / 2;
            text_ctr(r, bf, line->text, win_w / 2, ty,
                     theme->secondary.r, theme->secondary.g, theme->secondary.b);
        }
        by += lh;
    }

    /* Action hint at bottom of panel */
    int hd = py + ph - sc_h(34, win_h);
    divider(r, px + 16, hd, pw - 32, theme);

    HintItem hint = { "A", s->action };
    int hint_bar_h = sc_h(26, win_h);
    int hw = hintbar_item_width(bf, &hint, gh(bf));
    int hx = (win_w - hw) / 2;
    hintbar_draw_items(r, bf, &hint, 1, theme, hx, hd + sc(5, win_h), hint_bar_h);
}

int tutorial_next(TutorialState *t) {
    if (!t->active) return 0;
    t->slide++;
    if (t->slide >= TUTORIAL_SLIDE_COUNT) {
        t->active = 0;
        return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Shared helper: format m:ss from a seconds value
 * ---------------------------------------------------------------------- */

static void fmt_time(char *buf, int bufsz, double sec) {
    int s = (int)sec;
    int m = s / 60; s %= 60;
    int h = m / 60; m %= 60;
    if (h > 0)
        snprintf(buf, bufsz, "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, bufsz, "%d:%02d", m, s);
}

/* -------------------------------------------------------------------------
 * Resume-prompt overlay
 * ---------------------------------------------------------------------- */

void resume_prompt_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                        const Theme *theme, int win_w, int win_h,
                        const char *path, double pos_sec) {
    draw_dim(r, win_w, win_h);

    int px = sc(40, win_w);
    int pw = win_w - px * 2;
    int ph = sc_h(160, win_h);
    int py = (win_h - ph) / 2;
    panel_bg(r, px, py, pw, ph, theme);

    int fy  = TTF_FontHeight(font);
    int fsy = TTF_FontHeight(font_small);

    /* Title */
    int ty = py + sc_h(14, win_h);
    text_ctr(r, font, "Resume Playback?",
             win_w / 2, ty,
             theme->text.r, theme->text.g, theme->text.b);

    int div1y = ty + fy + 6;
    divider(r, px + 12, div1y, pw - 24, theme);

    /* Filename (truncate with leading …) */
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    char namebuf[64];
    if ((int)strlen(name) > 40) {
        snprintf(namebuf, sizeof(namebuf), "\xe2\x80\xa6%.37s", name + strlen(name) - 37);
    } else {
        snprintf(namebuf, sizeof(namebuf), "%s", name);
    }
    int ny = div1y + 8;
    text_ctr(r, font_small, namebuf,
             win_w / 2, ny,
             theme->secondary.r, theme->secondary.g, theme->secondary.b);

    /* Saved position */
    char posbuf[32];
    fmt_time(posbuf, sizeof(posbuf), pos_sec);
    char line2[64];
    snprintf(line2, sizeof(line2), "Saved position: %s", posbuf);
    text_ctr(r, font_small, line2,
             win_w / 2, ny + fsy + 4,
             theme->secondary.r, theme->secondary.g, theme->secondary.b);

    /* Hint row */
    int hd = py + ph - sc_h(34, win_h);
    divider(r, px + 12, hd, pw - 24, theme);

    static const HintItem hints[] = {
        { "A",   "Resume" },
        { "X",   "Start over" },
        { "B",   "Cancel" },
    };
    int glyph_h = gh(font_small);
    int gap_items = ig(font_small);
    int total_w = 0;
    for (int i = 0; i < 3; i++) {
        total_w += hintbar_item_width(font_small, &hints[i], glyph_h);
        if (i < 2) total_w += gap_items;
    }
    int hint_bar_h = sc_h(26, win_h);
    hintbar_draw_items(r, font_small, hints, 3, theme,
                       (win_w - total_w) / 2, hd + sc(5, win_h), hint_bar_h);
}

/* -------------------------------------------------------------------------
 * Fetch-art prompt overlay
 * ---------------------------------------------------------------------- */

void fetch_art_prompt_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                           const Theme *theme, int win_w, int win_h,
                           const char *book_name) {
    draw_dim(r, win_w, win_h);

    int px = sc(40, win_w);
    int pw = win_w - px * 2;
    int ph = sc_h(140, win_h);
    int py = (win_h - ph) / 2;
    panel_bg(r, px, py, pw, ph, theme);

    int fy  = TTF_FontHeight(font);
    int fsy = TTF_FontHeight(font_small);

    /* Title */
    int ty = py + sc_h(14, win_h);
    text_ctr(r, font, "Cover Art",
             win_w / 2, ty,
             theme->text.r, theme->text.g, theme->text.b);

    int div1y = ty + fy + 6;
    divider(r, px + 12, div1y, pw - 24, theme);

    /* Book name (truncated) */
    char namebuf[64];
    if ((int)strlen(book_name) > 40)
        snprintf(namebuf, sizeof(namebuf), "\xe2\x80\xa6%.37s",
                 book_name + strlen(book_name) - 37);
    else
        snprintf(namebuf, sizeof(namebuf), "%s", book_name);

    int ny = div1y + sc_h(10, win_h);
    text_ctr(r, font_small, namebuf,
             win_w / 2, ny,
             theme->secondary.r, theme->secondary.g, theme->secondary.b);

    /* Hint row */
    int hd = py + ph - sc_h(34, win_h);
    divider(r, px + 12, hd, pw - 24, theme);

    static const HintItem hints[] = {
        { "A", "Fetch art"  },
        { "X", "Clear art"  },
        { "B", "Cancel"     },
    };
    int glyph_h   = gh(font_small);
    int gap_items = ig(font_small);
    int total_w   = 0;
    for (int i = 0; i < 3; i++) {
        total_w += hintbar_item_width(font_small, &hints[i], glyph_h);
        if (i < 2) total_w += gap_items;
    }
    int hint_bar_h = sc_h(26, win_h);
    hintbar_draw_items(r, font_small, hints, 3, theme,
                       (win_w - total_w) / 2, hd + sc(5, win_h), hint_bar_h);
    (void)fsy;
}

/* -------------------------------------------------------------------------
 * Up-Next countdown overlay
 * ---------------------------------------------------------------------- */

void upnext_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                 const Theme *theme, int win_w, int win_h,
                 const char *path, int seconds_left) {
    draw_dim(r, win_w, win_h);

    int px = sc(40, win_w);
    int pw = win_w - px * 2;
    int ph = sc_h(150, win_h);
    int py = (win_h - ph) / 2;
    panel_bg(r, px, py, pw, ph, theme);

    int fy  = TTF_FontHeight(font);
    int fsy = TTF_FontHeight(font_small);

    /* Title */
    int ty = py + sc_h(14, win_h);
    char title[32];
    snprintf(title, sizeof(title), "Up Next  (%d)", seconds_left);
    text_ctr(r, font, title,
             win_w / 2, ty,
             theme->text.r, theme->text.g, theme->text.b);

    int div1y = ty + fy + 6;
    divider(r, px + 12, div1y, pw - 24, theme);

    /* Filename — scrolls if wider than panel content area */
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    int scroll_x = px + 14;
    int scroll_w = pw - 28;
    int ny = div1y + 10;
    (void)fsy;
    draw_text_scroll(r, font_small, name,
                     scroll_x, ny, scroll_w,
                     theme->secondary.r, theme->secondary.g, theme->secondary.b);

    /* Hint row */
    int hd = py + ph - sc_h(34, win_h);
    divider(r, px + 12, hd, pw - 24, theme);

    static const HintItem hints[] = {
        { "A", "Play now" },
        { "B", "Cancel"   },
    };
    int glyph_h = gh(font_small);
    int total_w = hintbar_item_width(font_small, &hints[0], glyph_h)
                + ig(font_small)
                + hintbar_item_width(font_small, &hints[1], glyph_h);
    int hint_bar_h = sc_h(26, win_h);
    hintbar_draw_items(r, font_small, hints, 2, theme,
                       (win_w - total_w) / 2, hd + sc(5, win_h), hint_bar_h);
}
void error_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                const Theme *theme, int win_w, int win_h,
                const char *path, const char *errmsg) {
    draw_dim(r, win_w, win_h);

    int px = sc(30, win_w);
    int pw = win_w - px * 2;
    int ph = sc_h(160, win_h);
    int py = (win_h - ph) / 2;
    panel_bg(r, px, py, pw, ph, theme);

    int fy  = TTF_FontHeight(font);
    int fsy = TTF_FontHeight(font_small);

    /* Title */
    int ty = py + sc_h(14, win_h);
    text_ctr(r, font, "Cannot Open File",
             win_w / 2, ty,
             theme->text.r, theme->text.g, theme->text.b);

    int div1y = ty + fy + 6;
    divider(r, px + 12, div1y, pw - 24, theme);

    /* Filename */
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    char namebuf[64];
    if ((int)strlen(name) > 44) {
        snprintf(namebuf, sizeof(namebuf), "\xe2\x80\xa6%.41s", name + strlen(name) - 41);
    } else {
        snprintf(namebuf, sizeof(namebuf), "%s", name);
    }
    int ny = div1y + 8;
    text_ctr(r, font_small, namebuf,
             win_w / 2, ny,
             theme->secondary.r, theme->secondary.g, theme->secondary.b);

    /* Error message (truncate) */
    char errbuf[64];
    if ((int)strlen(errmsg) > 54) {
        snprintf(errbuf, sizeof(errbuf), "%.51s\xe2\x80\xa6", errmsg);
    } else {
        snprintf(errbuf, sizeof(errbuf), "%s", errmsg);
    }
    text_ctr(r, font_small, errbuf,
             win_w / 2, ny + fsy + 4,
             theme->secondary.r, theme->secondary.g, theme->secondary.b);

    /* Hint row */
    int hd = py + ph - sc_h(34, win_h);
    divider(r, px + 12, hd, pw - 24, theme);

    HintItem hint = { "B", "Dismiss" };
    int hw = hintbar_item_width(font_small, &hint, gh(font_small));
    int hint_bar_h = sc_h(26, win_h);
    hintbar_draw_items(r, font_small, &hint, 1, theme,
                       (win_w - hw) / 2, hd + sc(5, win_h), hint_bar_h);
}
