#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "theme.h"

/* One hint item: a button glyph followed by a short description.
   btn  — button name drawn inside the glyph:
          single letters "A" "B" "X" "Y"  → coloured circle
          everything else ("SEL" "MENU" "L1" "◄►" …) → neutral pill
   label — text drawn immediately after the glyph, e.g. "Play" */
typedef struct {
    const char *btn;
    const char *label;
} HintItem;

/* Draw a full hint-bar row at the bottom of the window:
   fills the background strip, then draws all items centred horizontally.
   font       — used for the label text
   font_small — used for the text inside the button glyph */
void hintbar_draw_row(SDL_Renderer *renderer, TTF_Font *font,
                      TTF_Font *font_small, const HintItem *items, int count,
                      const Theme *theme, int win_w, int win_h);

/* Draw items only (no background), left-aligned from (x, y).
   bar_h   — height of the containing strip (items are vertically centred).
   Returns the x coordinate after the last item (useful for measuring). */
int hintbar_draw_items(SDL_Renderer *renderer, TTF_Font *font_small,
                       const HintItem *items, int count,
                       const Theme *theme, int x, int y, int bar_h);

/* Return the pixel width of one item (glyph + gap + label).
   glyph_h should be TTF_FontHeight(font_small) + 4. */
int hintbar_item_width(TTF_Font *font_small, const HintItem *item, int glyph_h);

/* Return the pixel width of just the glyph shape for btn. */
int hintbar_glyph_w(TTF_Font *font_small, const char *btn, int glyph_h);

/* Draw a vertical column with all labels left-aligned at the same x offset.
   Glyphs are right-aligned within a reserved slot of slot_w pixels;
   labels start at x + slot_w + gap.  Pass slot_w = 0 to auto-compute. */
void hintbar_draw_column(SDL_Renderer *r, TTF_Font *font_small,
                          const HintItem *items, int count,
                          const Theme *theme,
                          int x, int y, int row_h, int slot_w);

/* Total pixel width of an aligned column: max_glyph_w + gap + max_label_w. */
int hintbar_column_width(TTF_Font *font_small, const HintItem *items,
                          int count, int glyph_h);
