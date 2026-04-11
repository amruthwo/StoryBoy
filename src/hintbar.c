#include "hintbar.h"
#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Layout constants
 * ---------------------------------------------------------------------- */

/* All spacing constants are derived from font height so they scale
   automatically when font sizes are set proportionally to win_w.
   Base sizes (at 14 px small font / 640-wide window):
     glyph_padding=4  h_pad=6  glyph_label_gap=5  item_gap=18  hint_bar_h=24 */
static inline int glyph_pad(TTF_Font *f)  { int h = TTF_FontHeight(f); return h * 2 / 7; }
static inline int h_pad(TTF_Font *f)      { int h = TTF_FontHeight(f); return h * 3 / 7; }
static inline int glabel_gap(TTF_Font *f) { int h = TTF_FontHeight(f); return h * 5 / 14; }
static inline int item_gap(TTF_Font *f)   { int h = TTF_FontHeight(f); return h * 9 / 7; }
static inline int sc(int base, int w)     { return (int)(base * w / 640.0f + 0.5f); }

/* Face buttons that use a circle glyph */
static const char *CIRCLE_BTNS[] = { "A", "B", "X", "Y" };
#define N_CIRCLE (int)(sizeof(CIRCLE_BTNS)/sizeof(CIRCLE_BTNS[0]))

static int is_circle(const char *btn) {
    for (int i = 0; i < N_CIRCLE; i++)
        if (strcmp(btn, CIRCLE_BTNS[i]) == 0) return 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Primitive: filled circle (scan-line rasteriser, no SDL_gfx required)
 * ---------------------------------------------------------------------- */

static void fill_circle(SDL_Renderer *r, int cx, int cy, int rad,
                        Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawColor(r, R, G, B, 0xff);
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)sqrtf((float)(rad * rad - dy * dy));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

/* -------------------------------------------------------------------------
 * Primitive: filled pill (rounded-rect with semicircular end-caps)
 * ---------------------------------------------------------------------- */

static void fill_pill(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 R, Uint8 G, Uint8 B) {
    int rad = h / 2;
    if (w - 2 * rad > 0) {
        SDL_SetRenderDrawColor(r, R, G, B, 0xff);
        SDL_Rect rect = { x + rad, y, w - 2 * rad, h };
        SDL_RenderFillRect(r, &rect);
    }
    fill_circle(r, x + rad,         y + rad, rad, R, G, B);
    fill_circle(r, x + w - rad - 1, y + rad, rad, R, G, B);
}

/* -------------------------------------------------------------------------
 * Primitive: text centred on (cx, cy)
 * ---------------------------------------------------------------------- */

static void draw_text_centred(SDL_Renderer *r, TTF_Font *font,
                               const char *text, int cx, int cy,
                               Uint8 R, Uint8 G, Uint8 B) {
    SDL_Color col = { R, G, B, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = { cx - surf->w / 2, cy - surf->h / 2, surf->w, surf->h };
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void draw_text_at(SDL_Renderer *r, TTF_Font *font, const char *text,
                          int x, int y, Uint8 R, Uint8 G, Uint8 B) {
    SDL_Color col = { R, G, B, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = { x, y, surf->w, surf->h };
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

/* -------------------------------------------------------------------------
 * Glyph helpers — all colours come from the active theme
 * ---------------------------------------------------------------------- */

/* Width of a single (non-compound) glyph. */
static int single_glyph_width(TTF_Font *f, const char *btn, int glyph_h) {
    if (is_circle(btn)) {
        return glyph_h;
    } else {
        int tw = 0, dummy = 0;
        TTF_SizeUTF8(f, btn, &tw, &dummy);
        int w = tw + h_pad(f) * 2;
        if (w < glyph_h) w = glyph_h;
        return w;
    }
}

/* Width of a possibly-compound glyph ("START+X" → two pills + "+" connector). */
static int glyph_width(TTF_Font *f, const char *btn, int glyph_h) {
    if (!strchr(btn, '+') || is_circle(btn))
        return single_glyph_width(f, btn, glyph_h);
    char buf[64];
    strncpy(buf, btn, 63); buf[63] = '\0';
    int gap = h_pad(f) / 2, pw = 0, dummy = 0;
    TTF_SizeUTF8(f, "+", &pw, &dummy);
    int w = 0, first = 1;
    for (char *p = buf; ; ) {
        char *sep = strchr(p, '+');
        if (sep) *sep = '\0';
        if (!first) w += gap + pw + gap;
        w += single_glyph_width(f, p, glyph_h);
        first = 0;
        if (sep) p = sep + 1; else break;
    }
    return w;
}

/* Draw one single (non-compound) glyph at (x, cy), return x after it. */
static int draw_single_glyph(SDL_Renderer *r, TTF_Font *f,
                              const char *btn, int x, int cy, int glyph_h,
                              const Theme *theme) {
    int rad = glyph_h / 2;
    Uint8 bgR = theme->highlight_bg.r, bgG = theme->highlight_bg.g, bgB = theme->highlight_bg.b;
    Uint8 fgR = theme->highlight_text.r, fgG = theme->highlight_text.g, fgB = theme->highlight_text.b;
    if (is_circle(btn)) {
        fill_circle(r, x + rad, cy, rad, bgR, bgG, bgB);
        draw_text_centred(r, f, btn, x + rad, cy, fgR, fgG, fgB);
        return x + glyph_h;
    } else {
        int tw = 0, dummy = 0;
        TTF_SizeUTF8(f, btn, &tw, &dummy);
        int w = tw + h_pad(f) * 2;
        if (w < glyph_h) w = glyph_h;
        fill_pill(r, x, cy - rad, w, glyph_h, bgR, bgG, bgB);
        draw_text_centred(r, f, btn, x + w / 2, cy, fgR, fgG, fgB);
        return x + w;
    }
}

/* Draw one glyph at (x, cy); handles compound "A+B" as two pills with "+" between.
   Returns x after the glyph. */
static int draw_glyph(SDL_Renderer *r, TTF_Font *f,
                      const char *btn, int x, int cy, int glyph_h,
                      const Theme *theme) {
    if (!strchr(btn, '+') || is_circle(btn))
        return draw_single_glyph(r, f, btn, x, cy, glyph_h, theme);
    char buf[64];
    strncpy(buf, btn, 63); buf[63] = '\0';
    int gap = h_pad(f) / 2, pw = 0, dummy = 0;
    TTF_SizeUTF8(f, "+", &pw, &dummy);
    int cx = x, first = 1;
    for (char *p = buf; ; ) {
        char *sep = strchr(p, '+');
        if (sep) *sep = '\0';
        if (!first) {
            draw_text_centred(r, f, "+", cx + gap + pw / 2, cy,
                theme->secondary.r, theme->secondary.g, theme->secondary.b);
            cx += gap + pw + gap;
        }
        cx = draw_single_glyph(r, f, p, cx, cy, glyph_h, theme);
        first = 0;
        if (sep) p = sep + 1; else break;
    }
    return cx;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int hintbar_glyph_w(TTF_Font *font_small, const char *btn, int glyph_h) {
    return glyph_width(font_small, btn, glyph_h);
}

void hintbar_draw_column(SDL_Renderer *r, TTF_Font *font_small,
                          const HintItem *items, int count,
                          const Theme *theme,
                          int x, int y, int row_h, int slot_w) {
    int glyph_h = TTF_FontHeight(font_small) + glyph_pad(font_small);
    /* Auto-compute slot width from the widest glyph */
    if (slot_w == 0) {
        for (int i = 0; i < count; i++) {
            int gw = glyph_width(font_small, items[i].btn, glyph_h);
            if (gw > slot_w) slot_w = gw;
        }
    }
    int label_x = x + slot_w + glabel_gap(font_small);
    int fsy = TTF_FontHeight(font_small);
    for (int i = 0; i < count; i++) {
        int gw = glyph_width(font_small, items[i].btn, glyph_h);
        int gx = x + slot_w - gw;             /* right-align glyph in slot */
        int cy = y + i * row_h + row_h / 2;
        draw_glyph(r, font_small, items[i].btn, gx, cy, glyph_h, theme);
        draw_text_at(r, font_small, items[i].label,
                     label_x, cy - fsy / 2,
                     theme->secondary.r, theme->secondary.g, theme->secondary.b);
    }
}

int hintbar_column_width(TTF_Font *font_small, const HintItem *items,
                          int count, int glyph_h) {
    int max_gw = 0, max_lw = 0;
    for (int i = 0; i < count; i++) {
        int gw = glyph_width(font_small, items[i].btn, glyph_h);
        if (gw > max_gw) max_gw = gw;
        int lw = 0, dummy = 0;
        TTF_SizeUTF8(font_small, items[i].label, &lw, &dummy);
        if (lw > max_lw) max_lw = lw;
    }
    return max_gw + glabel_gap(font_small) + max_lw;
}

int hintbar_item_width(TTF_Font *font_small, const HintItem *item, int glyph_h) {
    int gw = glyph_width(font_small, item->btn, glyph_h);
    int lw = 0, dummy = 0;
    TTF_SizeUTF8(font_small, item->label, &lw, &dummy);
    return gw + glabel_gap(font_small) + lw;
}

int hintbar_draw_items(SDL_Renderer *renderer, TTF_Font *font_small,
                       const HintItem *items, int count,
                       const Theme *theme, int x, int y, int bar_h) {
    int glyph_h = TTF_FontHeight(font_small) + glyph_pad(font_small);
    int cy      = y + bar_h / 2;

    for (int i = 0; i < count; i++) {
        x = draw_glyph(renderer, font_small, items[i].btn, x, cy, glyph_h, theme);

        x += glabel_gap(font_small);
        int lh = TTF_FontHeight(font_small);
        draw_text_at(renderer, font_small, items[i].label,
                     x, cy - lh / 2,
                     theme->secondary.r, theme->secondary.g, theme->secondary.b);

        int lw = 0, dummy = 0;
        TTF_SizeUTF8(font_small, items[i].label, &lw, &dummy);
        x += lw;

        if (i < count - 1) x += item_gap(font_small);
    }
    return x;
}

void hintbar_draw_row(SDL_Renderer *renderer, TTF_Font *font,
                      TTF_Font *font_small, const HintItem *items, int count,
                      const Theme *theme, int win_w, int win_h) {
    (void)font;

    int bar_h = sc(24, win_w);
    int y = win_h - bar_h;

    /* Background strip */
    Uint8 br = theme->background.r > 15 ? (Uint8)(theme->background.r - 15) : 0;
    Uint8 bg = theme->background.g > 15 ? (Uint8)(theme->background.g - 15) : 0;
    Uint8 bb = theme->background.b > 15 ? (Uint8)(theme->background.b - 15) : 0;
    SDL_SetRenderDrawColor(renderer, br, bg, bb, 0xff);
    SDL_Rect strip = { 0, y, win_w, bar_h };
    SDL_RenderFillRect(renderer, &strip);

    if (count == 0) return;

    /* Measure total width to centre the row */
    int glyph_h = TTF_FontHeight(font_small) + glyph_pad(font_small);
    int total_w = 0;
    for (int i = 0; i < count; i++) {
        total_w += glyph_width(font_small, items[i].btn, glyph_h);
        total_w += glabel_gap(font_small);
        int lw = 0, dummy = 0;
        TTF_SizeUTF8(font_small, items[i].label, &lw, &dummy);
        total_w += lw;
        if (i < count - 1) total_w += item_gap(font_small);
    }

    int start_x = (win_w - total_w) / 2;
    if (start_x < sc(8, win_w)) start_x = sc(8, win_w);

    hintbar_draw_items(renderer, font_small, items, count, theme,
                       start_x, y, bar_h);
}
