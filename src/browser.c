#include "browser.h"
#include "hintbar.h"
#include "resume.h"
#include <SDL2/SDL_image.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Layout geometry
 * ---------------------------------------------------------------------- */

static inline int sc(int base, int w) { return (int)(base * w / 640.0f + 0.5f); }

typedef struct {
    int cols;
    int tile_w, tile_h;
    int img_h;
    int name_h;
    int content_h;
    int rows_visible;
    int hint_bar_h;
    int padding;
} LayoutMetrics;

static LayoutMetrics compute_metrics(BrowserLayout layout, int win_w, int win_h) {
    LayoutMetrics m;
    m.hint_bar_h = sc(24, win_w);
    m.padding    = sc(8,  win_w);
    m.content_h  = win_h - m.hint_bar_h;

    switch (layout) {
        case LAYOUT_LARGE:
            m.cols   = 2;
            m.tile_w = (win_w - m.padding * (m.cols + 1)) / m.cols;
            m.name_h = sc(36, win_w);
            m.tile_h = (m.content_h - m.padding * 3) / 2;
            m.img_h  = m.tile_h - m.name_h - m.padding;
            break;
        case LAYOUT_SMALL:
            m.cols   = 4;
            m.tile_w = (win_w - m.padding * (m.cols + 1)) / m.cols;
            m.name_h = sc(28, win_w);
            m.tile_h = (m.content_h - m.padding * 4) / 3;
            m.img_h  = m.tile_h - m.name_h - m.padding;
            break;
        case LAYOUT_SHOWCASE:
            /* Used only for navigation math (cols=1, rows_visible=1).
               Actual rendering is handled by draw_showcase(). */
            m.cols       = 1;
            m.tile_w     = win_w;
            m.tile_h     = m.content_h;
            m.img_h      = m.tile_h;
            m.name_h     = 0;
            m.rows_visible = 1;
            return m;   /* skip common rows_visible computation below */
        default: /* LAYOUT_LIST */
            m.cols        = 1;
            m.tile_w      = win_w - m.padding * 2;
            m.tile_h      = sc(52, win_w);
            m.img_h       = m.tile_h - m.padding * 2;
            m.name_h      = m.tile_h;
            break;
    }
    m.rows_visible = m.content_h / (m.tile_h + m.padding);
    return m;
}

/* -------------------------------------------------------------------------
 * Drawing primitives
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

/* Slightly darken an RGB value for alternating row tint */
static Uint8 dim(Uint8 v, int amt) {
    return (v > amt) ? (Uint8)(v - amt) : 0;
}

/* Filled rounded rectangle.  rad=0 degrades to a plain fill_rect.
   If rad >= h/2 the ends become full semicircles (pill shape).
   Blend mode is set by the caller before this call if A < 255. */
static void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h,
                               int rad, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    if (A != 0xff)
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);

    if (rad <= 0 || rad * 2 >= w || rad * 2 >= h) {
        if (rad * 2 >= h) rad = h / 2;   /* pill clamp */
        if (rad <= 0) {
            SDL_Rect rect = {x, y, w, h};
            SDL_RenderFillRect(r, &rect);
            if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
            return;
        }
    }

    /* Centre vertical strip */
    SDL_Rect c = {x + rad, y, w - 2 * rad, h};
    SDL_RenderFillRect(r, &c);
    /* Left and right strips (between the corner arcs) */
    SDL_Rect left  = {x,         y + rad, rad, h - 2 * rad};
    SDL_Rect right = {x + w - rad, y + rad, rad, h - 2 * rad};
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);

    /* Corner arcs: row-by-row horizontal spans */
    for (int dy = 0; dy < rad; dy++) {
        int dist = rad - dy;                          /* dist above/below arc center */
        int span = (int)sqrtf((float)(rad * rad - dist * dist));
        /* Top-left */
        SDL_RenderDrawLine(r, x + rad - span, y + dy, x + rad - 1, y + dy);
        /* Top-right */
        SDL_RenderDrawLine(r, x + w - rad, y + dy, x + w - rad + span - 1, y + dy);
        /* Bottom-left */
        SDL_RenderDrawLine(r, x + rad - span, y + h - 1 - dy, x + rad - 1, y + h - 1 - dy);
        /* Bottom-right */
        SDL_RenderDrawLine(r, x + w - rad, y + h - 1 - dy, x + w - rad + span - 1, y + h - 1 - dy);
    }

    if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* Pill convenience wrapper (radius = half height) */
static void fill_pill_bg(SDL_Renderer *r, int x, int y, int w, int h,
                         Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    fill_rounded_rect(r, x, y, w, h, h / 2, R, G, B, A);
}

/* Paint the window background colour over the four rounded corners of a
   previously-drawn image to give it the appearance of rounded corners.
   Only correct against a solid, uniform background. */
static void mask_corners(SDL_Renderer *r, int x, int y, int w, int h, int rad,
                          Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, R, G, B, 0xff);
    for (int i = 0; i < rad; i++) {
        float fy   = (float)(rad - i);
        int   dx   = (int)sqrtf((float)(rad * rad) - fy * fy);
        int   fill = rad - dx;
        if (fill <= 0) continue;
        SDL_RenderDrawLine(r, x,            y + i,           x + fill - 1, y + i);
        SDL_RenderDrawLine(r, x + w - fill, y + i,           x + w - 1,   y + i);
        SDL_RenderDrawLine(r, x,            y + h - 1 - i,   x + fill - 1, y + h - 1 - i);
        SDL_RenderDrawLine(r, x + w - fill, y + h - 1 - i,   x + w - 1,   y + h - 1 - i);
    }
}

/* -------------------------------------------------------------------------
 * Cover texture loading (lazy, cached)
 * ---------------------------------------------------------------------- */

/* Render a cover texture center-cropped to fill a square dst rect.
   Handles portrait/landscape covers without letterboxing or stretching. */
static void render_cover_cropped(SDL_Renderer *r, SDL_Texture *tex, SDL_Rect dst) {
    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    SDL_Rect src;
    int use_src = 0;
    if (tw > th) {
        src = (SDL_Rect){ (tw - th) / 2, 0, th, th };
        use_src = 1;
    } else if (th > tw) {
        src = (SDL_Rect){ 0, (th - tw) / 2, tw, tw };
        use_src = 1;
    }
    SDL_RenderCopy(r, tex, use_src ? &src : NULL, &dst);
}

static SDL_Texture *load_cover(SDL_Renderer *renderer, const char *path) {
    SDL_Surface *surf = IMG_Load(path);
    if (!surf) return NULL;
#ifdef SB_A30
    /* On low-memory devices (103MB RAM, no swap) large cover images can exhaust
       memory when cached as full-resolution textures — 4 covers at 1000×1000
       each consume ~16MB decoded.  Scale down to 256×256 max before creating
       the texture; audiobook grid cells are ≤200px wide so quality is retained. */
    if (surf->w > 256 || surf->h > 256) {
        int tw = surf->w > surf->h ? 256 : (surf->w * 256 / surf->h);
        int th = surf->h > surf->w ? 256 : (surf->h * 256 / surf->w);
        SDL_Surface *small = SDL_CreateRGBSurfaceWithFormat(
            0, tw, th, surf->format->BitsPerPixel, surf->format->format);
        if (small) {
            SDL_BlitScaled(surf, NULL, small, NULL);
            SDL_FreeSurface(surf);
            surf = small;
        }
    }
#endif
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
}

static SDL_Texture *get_cover(SDL_Renderer *renderer, CoverCache *cache,
                               const MediaLibrary *lib, int idx,
                               SDL_Texture *default_cover) {
    if (cache->textures[idx]) return cache->textures[idx];
    const MediaFolder *f = &lib->folders[idx];

    const char *cover_path = f->cover;
    char fallback[1280];
    if (!cover_path) {
        /* cover was absent at scan time — check if async fetch has since written it */
        snprintf(fallback, sizeof(fallback), "%s/cover.jpg", f->path);
        if (access(fallback, F_OK) == 0)
            cover_path = fallback;
        else {
            snprintf(fallback, sizeof(fallback), "%s/cover.png", f->path);
            if (access(fallback, F_OK) == 0)
                cover_path = fallback;
        }
    }

    if (cover_path) {
        SDL_Texture *t = load_cover(renderer, cover_path);
        if (t) { cache->textures[idx] = t; return t; }
    }
    return default_cover;
}

/* -------------------------------------------------------------------------
 * Backdrop: blurred cover image for the file-list view
 * ---------------------------------------------------------------------- */

/* Separable box-blur, horizontal pass.  src and dst are flat w×h arrays
   (stride = w).  Uses clamped-edge extension. */
static void blur_h(const Uint32 *src, Uint32 *dst, int w, int h, int r) {
    int div = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        const Uint32 *row = src + y * w;
        int sr = 0, sg = 0, sb = 0;
        /* Initialise window [-r..r] for x=0 */
        for (int k = -r; k <= r; k++) {
            int xi = k < 0 ? 0 : (k >= w ? w - 1 : k);
            Uint32 c = row[xi];
            sr += (c >> 16) & 0xff;
            sg += (c >>  8) & 0xff;
            sb +=  c        & 0xff;
        }
        for (int x = 0; x < w; x++) {
            dst[y * w + x] = 0xFF000000u
                           | ((Uint32)(sr / div) << 16)
                           | ((Uint32)(sg / div) <<  8)
                           |  (Uint32)(sb / div);
            int rem = x - r;     if (rem < 0)  rem = 0;
            int add = x + r + 1; if (add >= w) add = w - 1;
            Uint32 cr = row[rem], ca = row[add];
            sr += ((ca >> 16) & 0xff) - ((cr >> 16) & 0xff);
            sg += ((ca >>  8) & 0xff) - ((cr >>  8) & 0xff);
            sb += ( ca        & 0xff) - ( cr        & 0xff);
        }
    }
}

/* Separable box-blur, vertical pass. */
static void blur_v(const Uint32 *src, Uint32 *dst, int w, int h, int r) {
    int div = 2 * r + 1;
    for (int x = 0; x < w; x++) {
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            int yi = k < 0 ? 0 : (k >= h ? h - 1 : k);
            Uint32 c = src[yi * w + x];
            sr += (c >> 16) & 0xff;
            sg += (c >>  8) & 0xff;
            sb +=  c        & 0xff;
        }
        for (int y = 0; y < h; y++) {
            dst[y * w + x] = 0xFF000000u
                           | ((Uint32)(sr / div) << 16)
                           | ((Uint32)(sg / div) <<  8)
                           |  (Uint32)(sb / div);
            int rem = y - r;     if (rem < 0)  rem = 0;
            int add = y + r + 1; if (add >= h) add = h - 1;
            Uint32 cr = src[rem * w + x], ca = src[add * w + x];
            sr += ((ca >> 16) & 0xff) - ((cr >> 16) & 0xff);
            sg += ((ca >>  8) & 0xff) - ((cr >>  8) & 0xff);
            sb += ( ca        & 0xff) - ( cr        & 0xff);
        }
    }
}

/* 3 passes of separable box blur ≈ Gaussian blur.  Operates in-place on a
   flat w×h ARGB8888 pixel array.  Allocates two scratch buffers then frees. */
static void gaussian_blur(Uint32 *pixels, int w, int h, int r) {
    size_t n = (size_t)w * h;
    Uint32 *A = malloc(n * sizeof(Uint32));
    Uint32 *B = malloc(n * sizeof(Uint32));
    if (!A || !B) { free(A); free(B); return; }
    memcpy(A, pixels, n * sizeof(Uint32));
    for (int p = 0; p < 3; p++) {
        blur_h(A, B, w, h, r);
        blur_v(B, A, w, h, r);
    }
    memcpy(pixels, A, n * sizeof(Uint32));
    free(A); free(B);
}

/* Build a blurred backdrop texture.
   Works at 1/4 resolution (e.g. 160×120 for a 640×480 screen) — the GPU
   scales it back up on render, which is visually identical to a full-res blur
   but ~16× faster on CPU.  Eliminates the 5-8 s freeze on the A30. */
static SDL_Texture *build_backdrop(SDL_Renderer *renderer,
                                   const char *cover_path,
                                   int win_w, int win_h) {
    SDL_Surface *orig = IMG_Load(cover_path);
    if (!orig) return NULL;

    SDL_Surface *src = SDL_ConvertSurfaceFormat(orig, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(orig);
    if (!src) return NULL;

    /* Scale to fill width, preserving aspect ratio, centre-crop vertically.
       Portrait covers on a landscape screen: scaling to fill width makes the
       image taller than the screen, which SDL clips — no squash distortion. */
    int bw = win_w / 4;
    int bh = win_h / 4;
    SDL_Surface *small = SDL_CreateRGBSurfaceWithFormat(0, bw, bh, 32,
                                                         SDL_PIXELFORMAT_ARGB8888);
    if (!small) { SDL_FreeSurface(src); return NULL; }
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    {
        int scaled_h = (src->w > 0)
                       ? (int)((float)src->h * bw / src->w + 0.5f) : bh;
        if (scaled_h < bh) scaled_h = bh;  /* never leave gaps at top/bottom */
        SDL_Rect dst = { 0, (bh - scaled_h) / 2, bw, scaled_h };
        SDL_BlitScaled(src, NULL, small, &dst);
    }
    SDL_FreeSurface(src);

    /* Blur the small surface (radius 3 on 160×120 ≈ radius 12 on 640×480) */
    int     n      = bw * bh;
    Uint32 *pixels = malloc((size_t)n * sizeof(Uint32));
    if (pixels) {
        SDL_LockSurface(small);
        for (int y = 0; y < bh; y++)
            memcpy(pixels + y * bw,
                   (Uint8 *)small->pixels + y * small->pitch,
                   (size_t)bw * sizeof(Uint32));
        SDL_UnlockSurface(small);

        gaussian_blur(pixels, bw, bh, 3);

        SDL_LockSurface(small);
        for (int y = 0; y < bh; y++)
            memcpy((Uint8 *)small->pixels + y * small->pitch,
                   pixels + y * bw,
                   (size_t)bw * sizeof(Uint32));
        SDL_UnlockSurface(small);
        free(pixels);
    }

    /* Upload the small texture — SDL_RenderCopy will stretch it to full screen */
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, small);
    SDL_FreeSurface(small);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    return tex;
}

/* Ensure the cached backdrop matches the current folder; rebuild if not. */
static void ensure_backdrop(SDL_Renderer *renderer, CoverCache *cache,
                             const MediaLibrary *lib, int folder_idx,
                             int win_w, int win_h) {
    if (cache->backdrop_idx == folder_idx) return;
    if (cache->backdrop) { SDL_DestroyTexture(cache->backdrop); cache->backdrop = NULL; }
    cache->backdrop_idx = folder_idx;
    const char *cover = lib->folders[folder_idx].cover;
    if (cover)
        cache->backdrop = build_backdrop(renderer, cover, win_w, win_h);
}

/* -------------------------------------------------------------------------
 * Scroll clamping
 * ---------------------------------------------------------------------- */

static void clamp_scroll(BrowserState *state, int item_count,
                         const LayoutMetrics *m) {
    if (item_count == 0) { state->scroll_row = 0; return; }
    int sel_row = state->selected / m->cols;
    if (sel_row < state->scroll_row)
        state->scroll_row = sel_row;
    if (sel_row >= state->scroll_row + m->rows_visible)
        state->scroll_row = sel_row - m->rows_visible + 1;
    int max_row = (item_count - 1) / m->cols - m->rows_visible + 1;
    if (max_row < 0) max_row = 0;
    if (state->scroll_row > max_row) state->scroll_row = max_row;
    if (state->scroll_row < 0)       state->scroll_row = 0;
}

/* -------------------------------------------------------------------------
 * Folder grid
 * ---------------------------------------------------------------------- */

static void draw_folder_grid(SDL_Renderer *renderer, TTF_Font *font,
                              BrowserState *state, CoverCache *cache,
                              const MediaLibrary *lib, SDL_Texture *default_cover,
                              const Theme *t, int win_w, int win_h) {
    LayoutMetrics m = compute_metrics(state->layout, win_w, win_h);
    clamp_scroll(state, lib->folder_count, &m);

    int first = state->scroll_row * m.cols;
    int last  = first + m.rows_visible * m.cols;
    if (last > lib->folder_count) last = lib->folder_count;

    for (int i = first; i < last; i++) {
        int row = (i / m.cols) - state->scroll_row;
        int col = i % m.cols;
        int x   = m.padding + col * (m.tile_w + m.padding);
        int y   = m.padding + row * (m.tile_h + m.padding);
        int sel = (i == state->selected);

        int rad = sc(10, win_w);
        if (state->layout == LAYOUT_LIST) {
            /* Alternating row tint */
            if (sel) {
                fill_rounded_rect(renderer, x, y, m.tile_w, m.tile_h, rad,
                                  t->highlight_bg.r, t->highlight_bg.g,
                                  t->highlight_bg.b, 0xff);
            } else if (i % 2 == 0) {
                fill_rect(renderer, x, y, m.tile_w, m.tile_h,
                          dim(t->background.r, 8),
                          dim(t->background.g, 8),
                          dim(t->background.b, 8), 0xff);
            }

            /* Thumbnail — fit within thumb box, preserve aspect ratio */
            int thumb_w = (int)(m.img_h * 4.0f / 3.0f);
            SDL_Texture *cover = get_cover(renderer, cache, lib, i, default_cover);
            if (cover) {
                int tw, th;
                int side = (thumb_w < m.img_h) ? thumb_w : m.img_h;
                SDL_Rect dst = { x + m.padding + (thumb_w - side) / 2,
                                 y + m.padding + (m.img_h  - side) / 2,
                                 side, side };
                render_cover_cropped(renderer, cover, dst);
            }

            /* Name */
            RGB tc = sel ? t->highlight_text : t->text;
            draw_text(renderer, font, lib->folders[i].name,
                      x + m.padding + thumb_w + m.padding,
                      y + (m.tile_h - TTF_FontHeight(font)) / 2,
                      m.tile_w - thumb_w - m.padding * 3,
                      tc.r, tc.g, tc.b);

        } else {
            /* Grid tile — rounded highlight glow behind tile */
            if (sel) {
                fill_rounded_rect(renderer, x - 2, y - 2, m.tile_w + 4, m.tile_h + 4, rad + 2,
                                  t->highlight_bg.r, t->highlight_bg.g,
                                  t->highlight_bg.b, 170);
            }

            /* Cover image — fit within tile, preserve aspect ratio */
            SDL_Texture *cover = get_cover(renderer, cache, lib, i, default_cover);
            if (cover) {
                int tw, th;
                SDL_QueryTexture(cover, NULL, NULL, &tw, &th);
                int side = (m.tile_w < m.img_h) ? m.tile_w : m.img_h;
                SDL_Rect dst = { x + (m.tile_w - side) / 2,
                                 y + (m.img_h  - side) / 2,
                                 side, side };
                render_cover_cropped(renderer, cover, dst);
            } else {
                fill_rounded_rect(renderer, x, y, m.tile_w, m.img_h, rad,
                                  t->secondary.r, t->secondary.g, t->secondary.b, 0xff);
            }

            /* Name strip — rounded bottom corners */
            RGB strip = sel ? t->highlight_bg : (RGB){ dim(t->background.r, 10),
                                                        dim(t->background.g, 10),
                                                        dim(t->background.b, 10) };
            fill_rounded_rect(renderer, x, y + m.img_h, m.tile_w, m.name_h, rad,
                              strip.r, strip.g, strip.b, 0xff);

            RGB tc = sel ? t->highlight_text : t->text;
            {
                int name_w = 0, dummy = 0;
                TTF_SizeUTF8(font, lib->folders[i].name, &name_w, &dummy);
                int name_x = x + (m.tile_w - name_w) / 2;
                if (name_x < x + 2) name_x = x + 2; /* clamp if wider than tile */
                draw_text(renderer, font, lib->folders[i].name,
                          name_x, y + m.img_h + (m.name_h - TTF_FontHeight(font)) / 2,
                          m.tile_w - 4, tc.r, tc.g, tc.b);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Active file array for VIEW_FILES — flat folder or selected season
 * ---------------------------------------------------------------------- */

static const AudioFile *current_files(const BrowserState *state,
                                      const MediaLibrary *lib,
                                      int *out_count) {
    const MediaFolder *mf = &lib->folders[state->folder_idx];
    if (mf->is_series && state->season_idx >= 0 &&
            state->season_idx < mf->season_count) {
        *out_count = mf->seasons[state->season_idx].file_count;
        return mf->seasons[state->season_idx].files;
    }
    *out_count = mf->file_count;
    return mf->files;
}

/* -------------------------------------------------------------------------
 * Season cover cache — loaded when entering a show's season list
 * ---------------------------------------------------------------------- */

static void ensure_season_covers(SDL_Renderer *renderer, CoverCache *cache,
                                  const MediaLibrary *lib, int folder_idx,
                                  SDL_Texture *default_cover) {
    (void)default_cover; /* not stored — avoids double-free on cleanup */
    if (cache->season_tex_folder_idx == folder_idx) return;

    /* Free old owned season textures */
    if (cache->season_textures) {
        for (int i = 0; i < cache->season_tex_count; i++) {
            if (cache->season_textures[i])
                SDL_DestroyTexture(cache->season_textures[i]);
        }
        free(cache->season_textures);
        cache->season_textures = NULL;
        cache->season_tex_count = 0;
    }

    cache->season_tex_folder_idx = folder_idx;
    const MediaFolder *show = &lib->folders[folder_idx];
    if (!show->is_series || show->season_count == 0) return;

    cache->season_textures = calloc((size_t)show->season_count,
                                     sizeof(SDL_Texture *));
    cache->season_tex_count = show->season_count;
    if (!cache->season_textures) return;

    /* Load each season's own cover only.  Seasons without a cover get NULL here;
       draw_season_list falls back to the show-level texture via get_cover(),
       which is already cached from the folder grid — avoids loading the same
       (potentially large) show cover JPEG once per season. */
    for (int i = 0; i < show->season_count; i++) {
        if (show->seasons[i].cover)
            cache->season_textures[i] = load_cover(renderer, show->seasons[i].cover);
    }
}

/* -------------------------------------------------------------------------
 * Series mosaic cover — tile season covers, rotate 45°, crop to square
 * ---------------------------------------------------------------------- */

static void generate_series_mosaic(SDL_Renderer *r, MediaLibrary *lib,
                                    int folder_idx,
                                    SDL_Texture **season_tex, int n_season_tex,
                                    SDL_Texture *default_cover) {
    MediaFolder *series = &lib->folders[folder_idx];
    if (!series->is_series) return;

    char cover_path[1280];
    snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", series->path);

    /* If the file already exists (previous run), just wire it up and return */
    struct stat _st;
    if (stat(cover_path, &_st) == 0 && _st.st_size > 1024) {
        if (!series->cover) series->cover = strdup(cover_path);
        return;
    }

    if (!SDL_RenderTargetSupported(r)) return;

    /* Count usable textures (non-NULL, or fall back to default_cover) */
    int usable = 0;
    for (int i = 0; i < n_season_tex; i++)
        if (season_tex[i]) usable++;
    if (usable == 0 && !default_cover) return;
    /* Guard: need at least one texture slot to tile into (avoid % 0) */
    if (n_season_tex <= 0) return;

    /* Grid: 2×2 for ≤4 seasons, 3×3 for more */
    int cols = (n_season_tex <= 4) ? 2 : 3;
    int n_tiles = cols * cols;       /* always square grid */
    int output_size = 512;           /* final cropped square */
    /* Grid must be > output_size*√2 to fully fill the crop after 45° rotation */
    int grid_size = (int)(output_size * 1.5f);  /* 768px — comfortable margin */
    int tile = grid_size / cols;

    /* --- Step 1: render tiled grid --- */
    SDL_Texture *grid_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                               SDL_TEXTUREACCESS_TARGET,
                                               grid_size, grid_size);
    if (!grid_tex) return;

    SDL_SetRenderTarget(r, grid_tex);
    SDL_SetRenderDrawColor(r, 18, 18, 18, 255);
    SDL_RenderClear(r);

    for (int i = 0; i < n_tiles; i++) {
        int idx = i % n_season_tex;
        SDL_Texture *cov = (idx < n_season_tex) ? season_tex[idx] : NULL;
        if (!cov) cov = default_cover;
        if (!cov) continue;

        int col = i % cols, row_i = i / cols;
        SDL_Rect dst = { col * tile, row_i * tile, tile, tile };

        /* Center-crop the season cover into the tile */
        int tw = 0, th = 0;
        SDL_QueryTexture(cov, NULL, NULL, &tw, &th);
        SDL_Rect src;
        if (tw > th)       src = (SDL_Rect){ (tw - th) / 2, 0, th, th };
        else if (th > tw)  src = (SDL_Rect){ 0, (th - tw) / 2, tw, tw };
        else               src = (SDL_Rect){ 0, 0, tw, th };
        SDL_RenderCopy(r, cov, &src, &dst);
    }

    /* --- Step 2: rotate 45° into a larger canvas --- */
    float sq2 = 1.41421356f;
    int big = (int)(grid_size * sq2) + 4;

    SDL_Texture *rot_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                              SDL_TEXTUREACCESS_TARGET, big, big);
    if (!rot_tex) {
        SDL_DestroyTexture(grid_tex);
        SDL_SetRenderTarget(r, NULL);
        return;
    }

    SDL_SetRenderTarget(r, rot_tex);
    SDL_SetRenderDrawColor(r, 18, 18, 18, 255);
    SDL_RenderClear(r);

    SDL_Rect grid_dst = { (big - grid_size) / 2, (big - grid_size) / 2,
                           grid_size, grid_size };
    SDL_RenderCopyEx(r, grid_tex, NULL, &grid_dst, -45.0, NULL, SDL_FLIP_NONE);
    SDL_DestroyTexture(grid_tex);

    /* --- Step 3: crop output_size×output_size from centre --- */
    SDL_Texture *final_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET,
                                                output_size, output_size);
    if (!final_tex) {
        SDL_DestroyTexture(rot_tex);
        SDL_SetRenderTarget(r, NULL);
        return;
    }

    SDL_SetRenderTarget(r, final_tex);
    SDL_Rect crop_src = { (big - output_size) / 2, (big - output_size) / 2,
                           output_size, output_size };
    SDL_RenderCopy(r, rot_tex, &crop_src, NULL);
    SDL_DestroyTexture(rot_tex);

    /* --- Step 4: read pixels → convert → save as JPEG --- */
    SDL_Surface *raw = SDL_CreateRGBSurface(0, output_size, output_size, 32,
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    if (raw) {
        SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888,
                             raw->pixels, raw->pitch);
        SDL_Surface *rgb = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGB24, 0);
        SDL_FreeSurface(raw);
        if (rgb) {
            if (IMG_SaveJPG(rgb, cover_path, 92) == 0)
                series->cover = strdup(cover_path);
            SDL_FreeSurface(rgb);
        }
    }

    SDL_DestroyTexture(final_tex);
    SDL_SetRenderTarget(r, NULL);

    /* Always mark cover as attempted, even if IMG_SaveJPG failed (SD card full,
       read-only mount, etc.).  Prevents repeated GPU readback every frame when
       saving fails — get_cover will return default_cover for a missing file. */
    if (!series->cover)
        series->cover = strdup(cover_path);
}

/* -------------------------------------------------------------------------
 * File list
 * ---------------------------------------------------------------------- */

static void draw_file_list(SDL_Renderer *renderer, TTF_Font *font,
                           BrowserState *state, CoverCache *cache,
                           const MediaLibrary *lib,
                           const Theme *t, int win_w, int win_h) {
    const MediaFolder *folder = &lib->folders[state->folder_idx];
    int fcount;
    const AudioFile *files = current_files(state, lib, &fcount);
    /* Display name: season name when inside a show, folder name otherwise */
    const char *display_name = folder->name;
    if (folder->is_series && state->season_idx >= 0 &&
            state->season_idx < folder->season_count)
        display_name = folder->seasons[state->season_idx].name;

    int has_backdrop = (cache->backdrop != NULL);
    int hint_bar_h = sc(24, win_w);
    int padding    = sc(8,  win_w);
    int row_h      = sc(52, win_w);

    /* Rebuild progress cache when folder or season changes */
    int cache_stale = (state->prog_folder_idx != state->folder_idx) ||
                      (state->prog_season_idx  != state->season_idx);
    if (cache_stale) {
        free(state->file_progress);
        state->prog_folder_idx = state->folder_idx;
        state->prog_season_idx = state->season_idx;
        state->file_progress   = NULL;
        if (fcount > 0) {
            state->file_progress = malloc((size_t)fcount * sizeof(float));
            if (state->file_progress) {
                for (int i = 0; i < fcount; i++)
                    state->file_progress[i] = -1.0f;

                if (fcount > 1) {
                    /* Multi-file book: use book-level resume for overall progress.
                       Show the current file as in-progress; others as done or unstarted. */
                    const MediaFolder *fol = &lib->folders[state->folder_idx];
                    const char *book_dir = (fol->is_series &&
                                           state->season_idx >= 0 &&
                                           state->season_idx < fol->season_count)
                                          ? fol->seasons[state->season_idx].path
                                          : fol->path;
                    int   bfi  = 0;
                    double bpos = 0.0, btot = 0.0, bbp = 0.0;
                    if (resume_load_book(book_dir, &bfi, &bpos, &btot, &bbp) &&
                        btot > 5.0) {
                        for (int fi = 0; fi < fcount; fi++) {
                            if (fi < bfi)
                                state->file_progress[fi] = 1.0f;  /* completed */
                            else if (fi == bfi && bpos > 0)
                                state->file_progress[fi] = (float)(bbp / btot);
                            /* else: unstarted, stays -1 */
                        }
                    }
                } else {
                    /* Single-file: use per-file resume */
                    ResumeEntry *entries = NULL;
                    int ec = resume_load_all(&entries);
                    for (int ei = 0; ei < ec; ei++) {
                        if (entries[ei].duration < 5.0) continue;
                        if (entries[ei].file_idx >= 0) continue; /* skip book entries */
                        for (int fi = 0; fi < fcount; fi++) {
                            if (strcmp(entries[ei].path, files[fi].path) == 0) {
                                state->file_progress[fi] =
                                    (float)(entries[ei].position / entries[ei].duration);
                                break;
                            }
                        }
                    }
                    free(entries);
                }
            }
        }
    }
    int content_h = win_h - hint_bar_h - row_h; /* reserve bottom row for name */
    int visible   = content_h / row_h;

    /* Draw blurred backdrop + dim overlay */
    if (has_backdrop) {
        SDL_RenderCopy(renderer, cache->backdrop, NULL, NULL);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fill_rect(renderer, 0, 0, win_w, win_h, 0, 0, 0, 120);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    /* Clamp scroll */
    if (state->selected < state->scroll_row)
        state->scroll_row = state->selected;
    if (state->selected >= state->scroll_row + visible)
        state->scroll_row = state->selected - visible + 1;
    if (state->scroll_row < 0) state->scroll_row = 0;

    for (int i = state->scroll_row; i < state->scroll_row + visible; i++) {
        if (i >= fcount) break;
        int row = i - state->scroll_row;
        int y   = row * row_h;
        int sel = (i == state->selected);

        int row_rad = sc(10, win_w);
        if (has_backdrop) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            if (sel) {
                fill_rounded_rect(renderer, padding, y, win_w - padding * 2, row_h, row_rad,
                                  t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 200);
            } else if (row % 2 == 0) {
                fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                          0, 0, 0, 40);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        } else {
            if (sel) {
                fill_rounded_rect(renderer, padding, y, win_w - padding * 2, row_h, row_rad,
                                  t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 0xff);
            } else if (row % 2 == 0) {
                fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                          dim(t->background.r, 8),
                          dim(t->background.g, 8),
                          dim(t->background.b, 8), 0xff);
            }
        }

        /* Pill background behind filename when a backdrop is active */
        if (has_backdrop && !sel) {
            int fh = TTF_FontHeight(font);
            int tw = 0, dummy = 0;
            TTF_SizeUTF8(font, files[i].name, &tw, &dummy);
            int pill_h = fh + 10;
            int pill_pad = sc(8, win_w);
            int pill_x = padding * 2 - pill_pad;
            int pill_w = tw + pill_pad * 2;
            int max_pill_w = win_w - padding * 2 - pill_pad;
            if (pill_w > max_pill_w) pill_w = max_pill_w;
            int pill_y = y + (row_h - pill_h) / 2;
            fill_pill_bg(renderer, pill_x, pill_y, pill_w, pill_h,
                         t->background.r, t->background.g, t->background.b, 170);
        }

        RGB tc = sel ? t->highlight_text : t->text;
        draw_text(renderer, font, files[i].name,
                  padding * 2,
                  y + (row_h - TTF_FontHeight(font)) / 2,
                  win_w - padding * 4,
                  tc.r, tc.g, tc.b);

        /* Progress bar — thin strip at the bottom of the row */
        if (state->file_progress && state->file_progress[i] >= 0.0f) {
            float prog = state->file_progress[i];
            int bar_y  = y + row_h - 3;
            int full_w = win_w - padding * 2;
            if (prog >= 0.95f) {
                /* Completed — faint full-width bar in secondary color */
                fill_rect(renderer, padding, bar_y, full_w, 3,
                          t->secondary.r, t->secondary.g, t->secondary.b, 120);
            } else if (prog > 0.0f) {
                /* In-progress — filled portion in highlight_bg */
                int fill_w = (int)(full_w * prog);
                fill_rect(renderer, padding, bar_y, fill_w, 3,
                          t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 200);
            }
        }
    }

    /* Folder name header at bottom — pill label */
    int header_y = win_h - hint_bar_h - row_h;
    {
        int fh = TTF_FontHeight(font);
        int pill_pad = sc(14, win_w);
        int pill_h = fh + sc(10, win_w);
        int pill_w, name_w = 0, dummy = 0;
        TTF_SizeUTF8(font, display_name, &name_w, &dummy);
        pill_w = name_w + pill_pad * 2;
        if (pill_w > win_w - padding * 2) pill_w = win_w - padding * 2;
        int pill_x = (win_w - pill_w) / 2;
        int pill_y = header_y + (row_h - pill_h) / 2;
        Uint8 bg_r, bg_g, bg_b, bg_a;
        if (has_backdrop) {
            bg_r = 0; bg_g = 0; bg_b = 0; bg_a = 180;
        } else {
            bg_r = dim(t->background.r, 15);
            bg_g = dim(t->background.g, 15);
            bg_b = dim(t->background.b, 15);
            bg_a = 0xff;
        }
        fill_pill_bg(renderer, pill_x, pill_y, pill_w, pill_h, bg_r, bg_g, bg_b, bg_a);
        draw_text(renderer, font, display_name,
                  pill_x + pill_pad, pill_y + (pill_h - fh) / 2,
                  pill_w - pill_pad * 2,
                  t->secondary.r, t->secondary.g, t->secondary.b);
    }
}

/* -------------------------------------------------------------------------
 * Season list (VIEW_SEASONS)
 * ---------------------------------------------------------------------- */

static void draw_season_list(SDL_Renderer *renderer, TTF_Font *font,
                             TTF_Font *font_small __attribute__((unused)),
                             BrowserState *state,
                             CoverCache *cache, const MediaLibrary *lib,
                             SDL_Texture *default_cover,
                             const Theme *t, int win_w, int win_h) {
    const MediaFolder *show = &lib->folders[state->folder_idx];
    int count        = show->season_count;
    int has_backdrop = (cache->backdrop != NULL);
    int hint_bar_h   = sc(24, win_w);
    int header_h     = sc(56, win_w);  /* bottom show-name bar */

    /* Draw backdrop + dim (common to all layouts) */
    if (has_backdrop) {
        SDL_RenderCopy(renderer, cache->backdrop, NULL, NULL);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fill_rect(renderer, 0, 0, win_w, win_h, 0, 0, 0, 120);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    if (state->season_layout == LAYOUT_LIST) {
        /* ---- List view ---- */
        int padding = sc(8, win_w);
        int row_h   = header_h;
        int thumb_w = (int)(row_h * 2.0f / 3.0f);
        int content_h = win_h - hint_bar_h - row_h;
        int visible   = content_h / row_h;

        /* Clamp scroll (item-based; cols=1) */
        if (state->season_idx < state->season_scroll)
            state->season_scroll = state->season_idx;
        if (state->season_idx >= state->season_scroll + visible)
            state->season_scroll = state->season_idx - visible + 1;
        if (state->season_scroll < 0) state->season_scroll = 0;

        for (int i = state->season_scroll; i < state->season_scroll + visible; i++) {
            if (i >= count) break;
            int row = i - state->season_scroll;
            int y   = row * row_h;
            int sel = (i == state->season_idx);

            if (has_backdrop) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                if (sel)
                    fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                              t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 200);
                else if (row % 2 == 0)
                    fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                              0, 0, 0, 40);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            } else {
                if (sel)
                    fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                              t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 0xff);
                else if (row % 2 == 0)
                    fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                              dim(t->background.r, 8), dim(t->background.g, 8),
                              dim(t->background.b, 8), 0xff);
            }

            /* Season thumbnail — fall back to show cover if no season-specific art */
            SDL_Texture *thumb = (cache->season_textures && i < cache->season_tex_count)
                                 ? cache->season_textures[i] : NULL;
            if (!thumb)
                thumb = get_cover(renderer, cache, lib, state->folder_idx, default_cover);
            if (thumb) {
                int tw, th;
                SDL_QueryTexture(thumb, NULL, NULL, &tw, &th);
                int avail_h = row_h - padding;
                int side = (thumb_w < avail_h) ? thumb_w : avail_h;
                SDL_Rect dst = { padding + (thumb_w - side) / 2,
                                 y + padding / 2 + (avail_h - side) / 2,
                                 side, side };
                render_cover_cropped(renderer, thumb, dst);
            }

            int text_x = padding + thumb_w + padding;
            int text_w = win_w - text_x - padding;

            char season_label[128];
            snprintf(season_label, sizeof(season_label), "%s",
                     show->seasons[i].name);

            RGB tc = sel ? t->highlight_text : t->text;
            int fh = TTF_FontHeight(font);
            int label_y = y + (row_h - fh) / 2;

            if (has_backdrop && !sel) {
                int pill_pad = sc(8, win_w);
                int tw = 0, dummy = 0;
                TTF_SizeUTF8(font, season_label, &tw, &dummy);
                int pw = tw + pill_pad * 2;
                if (pw > win_w - (text_x - pill_pad) - padding) pw = win_w - (text_x - pill_pad) - padding;
                fill_pill_bg(renderer, text_x - pill_pad, label_y - sc(3, win_w),
                             pw, fh + sc(6, win_w),
                             t->background.r, t->background.g, t->background.b, 170);
            }

            draw_text(renderer, font, season_label,
                      text_x, label_y, text_w, tc.r, tc.g, tc.b);
        }

    } else {
        /* ---- Grid view (LAYOUT_LARGE / LAYOUT_SMALL) ---- */
        /* Pass reduced height so tiles don't overlap the show-name header */
        LayoutMetrics m = compute_metrics(state->season_layout, win_w, win_h - header_h);

        /* Scroll clamping (row-based) */
        int sel_row = state->season_idx / m.cols;
        if (sel_row < state->season_scroll)
            state->season_scroll = sel_row;
        if (sel_row >= state->season_scroll + m.rows_visible)
            state->season_scroll = sel_row - m.rows_visible + 1;
        if (state->season_scroll < 0) state->season_scroll = 0;

        int first = state->season_scroll * m.cols;
        int last  = first + m.rows_visible * m.cols;
        if (last > count) last = count;

        for (int i = first; i < last; i++) {
            int row = i / m.cols - state->season_scroll;
            int col = i % m.cols;
            int x   = m.padding + col * (m.tile_w + m.padding);
            int y   = m.padding + row * (m.tile_h + m.padding);
            int sel = (i == state->season_idx);

            int srad = sc(10, win_w);
            if (sel) {
                fill_rounded_rect(renderer, x - 2, y - 2, m.tile_w + 4, m.tile_h + 4, srad + 2,
                                  t->highlight_bg.r, t->highlight_bg.g,
                                  t->highlight_bg.b, 170);
            }

            SDL_Texture *thumb = (cache->season_textures && i < cache->season_tex_count)
                                 ? cache->season_textures[i] : NULL;
            if (!thumb)
                thumb = get_cover(renderer, cache, lib, state->folder_idx, default_cover);
            if (thumb) {
                int tw, th;
                SDL_QueryTexture(thumb, NULL, NULL, &tw, &th);
                int side = (m.tile_w < m.img_h) ? m.tile_w : m.img_h;
                SDL_Rect dst = { x + (m.tile_w - side) / 2,
                                 y + (m.img_h  - side) / 2,
                                 side, side };
                render_cover_cropped(renderer, thumb, dst);
            } else {
                fill_rounded_rect(renderer, x, y, m.tile_w, m.img_h, srad,
                                  t->secondary.r, t->secondary.g, t->secondary.b, 0xff);
            }

            /* Name strip — rounded bottom corners */
            RGB strip = sel ? t->highlight_bg : (RGB){ dim(t->background.r, 10),
                                                        dim(t->background.g, 10),
                                                        dim(t->background.b, 10) };
            fill_rounded_rect(renderer, x, y + m.img_h, m.tile_w, m.name_h, srad,
                              strip.r, strip.g, strip.b, 0xff);

            RGB tc = sel ? t->highlight_text : t->text;
            {
                int name_w = 0, dummy = 0;
                TTF_SizeUTF8(font, show->seasons[i].name, &name_w, &dummy);
                int name_x = x + (m.tile_w - name_w) / 2;
                if (name_x < x + 2) name_x = x + 2;
                draw_text(renderer, font, show->seasons[i].name,
                          name_x, y + m.img_h + (m.name_h - TTF_FontHeight(font)) / 2,
                          m.tile_w - 4, tc.r, tc.g, tc.b);
            }
        }
    }

    /* Show name header at bottom — pill label */
    int header_y = win_h - hint_bar_h - header_h;
    int padding = sc(8, win_w);
    {
        int fh = TTF_FontHeight(font);
        int pill_pad = sc(14, win_w);
        int pill_h = fh + sc(10, win_w);
        int pill_w, name_w = 0, dummy = 0;
        TTF_SizeUTF8(font, show->name, &name_w, &dummy);
        pill_w = name_w + pill_pad * 2;
        if (pill_w > win_w - padding * 2) pill_w = win_w - padding * 2;
        int pill_x = (win_w - pill_w) / 2;
        int pill_y = header_y + (header_h - pill_h) / 2;
        Uint8 bg_r, bg_g, bg_b, bg_a;
        if (has_backdrop) {
            bg_r = 0; bg_g = 0; bg_b = 0; bg_a = 180;
        } else {
            bg_r = dim(t->background.r, 15);
            bg_g = dim(t->background.g, 15);
            bg_b = dim(t->background.b, 15);
            bg_a = 0xff;
        }
        fill_pill_bg(renderer, pill_x, pill_y, pill_w, pill_h, bg_r, bg_g, bg_b, bg_a);
        draw_text(renderer, font, show->name,
                  pill_x + pill_pad, pill_y + (pill_h - fh) / 2,
                  pill_w - pill_pad * 2,
                  t->secondary.r, t->secondary.g, t->secondary.b);
    }
}

/* -------------------------------------------------------------------------
 * Hint bar
 * ---------------------------------------------------------------------- */

static void draw_hint_bar(SDL_Renderer *renderer, TTF_Font *font,
                          TTF_Font *font_small, const BrowserState *state,
                          const Theme *t, int win_w, int win_h) {
    static const HintItem folder_hints[] = {
        { "A",   "Open"      },
        { "B",   "Exit"      },
        { "SEL", "Layout"    },
        { "X",   "History"   },
        { "Y",   "Fetch Art" },
        { "R1",  "Theme"     },
    };
    /* Showcase: swap SEL/X for a ← → navigation hint so it's obvious
       the D-pad left/right navigates the carousel. */
    static const HintItem showcase_hints[] = {
        { "\xe2\x86\x90\xe2\x86\x92", "Browse" },
        { "A",   "Open"      },
        { "B",   "Exit"      },
        { "Y",   "Fetch Art" },
        { "R1",  "Theme"     },
    };
    static const HintItem season_hints[] = {
        { "A",   "Open"      },
        { "B",   "Back"      },
        { "SEL", "Layout"    },
        { "X",   "History"   },
        { "Y",   "Fetch Art" },
    };
    static const HintItem file_hints[] = {
        { "A",    "Play"    },
        { "B",    "Back"    },
        { "L2",   "Prev"    },
        { "R2",   "Next"    },
        { "X",    "History" },
        { "MENU", "Exit"    },
    };
    const HintItem *items;
    int item_count;
    if (state->view == VIEW_FOLDERS && state->layout == LAYOUT_SHOWCASE) {
        items = showcase_hints; item_count = 5;
    } else if (state->view == VIEW_FOLDERS) {
        items = folder_hints; item_count = 6;
    } else if (state->view == VIEW_SEASONS) {
        items = season_hints; item_count = 5;
    } else {
        items = file_hints;   item_count = 6;
    }
    hintbar_draw_row(renderer, font, font_small, items, item_count, t, win_w, win_h);
}

/* -------------------------------------------------------------------------
 * Showcase carousel (LAYOUT_SHOWCASE)
 * ---------------------------------------------------------------------- */

static void draw_showcase(SDL_Renderer *renderer, TTF_Font *font, TTF_Font *font_small,
                          BrowserState *state, CoverCache *cache,
                          const MediaLibrary *lib, SDL_Texture *default_cover,
                          const Theme *t, int win_w, int win_h) {
    int count = lib->folder_count;
    if (count == 0) return;
    if (state->selected < 0)       state->selected = 0;
    if (state->selected >= count)  state->selected = count - 1;

    int sel        = state->selected;
    int hint_bar_h = sc(24, win_w);
    int content_h  = win_h - hint_bar_h;

    /* Geometry */
    int peek_w = sc(80, win_w);   /* visible slice of neighbouring cover */
    int gap    = sc(8,  win_w);   /* space between peeking edge and main cover */
    int cover_w = win_w - 2 * (peek_w + gap);
    int cover_x = (win_w - cover_w) / 2;
    int rad     = sc(16, win_w);

    int fh     = TTF_FontHeight(font);
    int fsh    = TTF_FontHeight(font_small);
    int pad_t  = sc(14, win_h);
    int name_h = fh  + sc(6, win_h);
    int ctr_h  = fsh + sc(4, win_h);
    int cover_h = content_h - pad_t - sc(8, win_h) - name_h - ctr_h - sc(4, win_h);
    int cover_y = pad_t;
    int name_y  = cover_y + cover_h + sc(8, win_h);
    int ctr_y   = name_y + name_h;

    /* Left peek (sel−1): fit-scale to slot height, right-align inside edge.
       No card background — theme background shows in any gap areas.
       Same treatment for all cover types (downloaded or default). */
    if (sel > 0) {
        int pidx = sel - 1;
        int lx   = cover_x - gap - cover_w;
        SDL_Texture *tex = get_cover(renderer, cache, lib, pidx, default_cover);
        int side_p = (cover_w < cover_h) ? cover_w : cover_h;
        /* Right-align the cover so its right edge butts against the gap — this
           gives peek_w visible pixels on the left edge, matching the right side. */
        SDL_Rect dst = { cover_x - gap - side_p,
                         cover_y + (cover_h - side_p) / 2, side_p, side_p };
        render_cover_cropped(renderer, tex, dst);
        mask_corners(renderer, lx, cover_y, cover_w, cover_h, rad,
                     t->background.r, t->background.g, t->background.b);
    }
    /* Right peek (sel+1) — mirror: left-align inside edge */
    if (sel < count - 1) {
        int pidx = sel + 1;
        int rx   = cover_x + cover_w + gap;
        SDL_Texture *tex = get_cover(renderer, cache, lib, pidx, default_cover);
        int side_p = (cover_w < cover_h) ? cover_w : cover_h;
        SDL_Rect dst = { rx, cover_y + (cover_h - side_p) / 2, side_p, side_p };
        render_cover_cropped(renderer, tex, dst);
        mask_corners(renderer, rx, cover_y, cover_w, cover_h, rad,
                     t->background.r, t->background.g, t->background.b);
    }

    /* Main cover — center-cropped square */
    SDL_Texture *main_tex = get_cover(renderer, cache, lib, sel, default_cover);
    {
        int side_m = (cover_w < cover_h) ? cover_w : cover_h;
        int ix = cover_x + (cover_w - side_m) / 2;
        int iy = cover_y + (cover_h - side_m) / 2;
        SDL_Rect dst = { ix, iy, side_m, side_m };
        render_cover_cropped(renderer, main_tex, dst);
        mask_corners(renderer, ix, iy, side_m, side_m, rad,
                     t->background.r, t->background.g, t->background.b);
    }

    /* Folder name — centred below cover */
    {
        const char *name = lib->folders[sel].name;
        int tw = 0, dummy = 0;
        TTF_SizeUTF8(font, name, &tw, &dummy);
        int nx = (win_w - (tw < cover_w ? tw : cover_w)) / 2;
        draw_text(renderer, font, name, nx, name_y, cover_w,
                  t->text.r, t->text.g, t->text.b);
    }

    /* Counter "X / Y" — centred, secondary colour */
    {
        char ctr[32];
        snprintf(ctr, sizeof(ctr), "%d / %d", sel + 1, count);
        int tw = 0, dummy = 0;
        TTF_SizeUTF8(font_small, ctr, &tw, &dummy);
        draw_text(renderer, font_small, ctr, (win_w - tw) / 2, ctr_y, win_w,
                  t->secondary.r, t->secondary.g, t->secondary.b);
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void browser_init(BrowserState *state, CoverCache *cache,
                  const MediaLibrary *lib) {
    memset(state, 0, sizeof(*state));
    state->view             = VIEW_FOLDERS;
    state->layout           = LAYOUT_LARGE;
    state->season_layout    = LAYOUT_LARGE;
    state->season_idx       = 0;
    state->prog_folder_idx  = -1;
    state->prog_season_idx  = -1;

    cache->count                 = lib->folder_count;
    cache->textures              = calloc((size_t)(lib->folder_count ? lib->folder_count : 1),
                                          sizeof(SDL_Texture *));
    cache->backdrop              = NULL;
    cache->backdrop_idx          = -1;
    cache->season_textures       = NULL;
    cache->season_tex_count      = 0;
    cache->season_tex_folder_idx = -1;
}

void browser_draw(SDL_Renderer *renderer, TTF_Font *font, TTF_Font *font_small,
                  BrowserState *state, CoverCache *cache, MediaLibrary *lib,
                  SDL_Texture *default_cover, const Theme *theme,
                  int win_w, int win_h) {
    SDL_SetRenderDrawColor(renderer,
        theme->background.r, theme->background.g, theme->background.b, 0xff);
    SDL_RenderClear(renderer);

    if (state->view == VIEW_FOLDERS) {
        if (lib->folder_count == 0) {
            int padding = sc(8, win_w);
            draw_text(renderer, font, "No media found.",
                      padding, padding, win_w - padding * 2,
                      theme->secondary.r, theme->secondary.g, theme->secondary.b);
        } else if (state->layout == LAYOUT_SHOWCASE) {
            draw_showcase(renderer, font, font_small, state, cache, lib,
                          default_cover, theme, win_w, win_h);
        } else {
            draw_folder_grid(renderer, font, state, cache, lib,
                             default_cover, theme, win_w, win_h);
        }
    } else if (state->view == VIEW_SEASONS) {
        ensure_backdrop(renderer, cache, lib, state->folder_idx, win_w, win_h);
        ensure_season_covers(renderer, cache, lib, state->folder_idx, default_cover);
        /* Generate mosaic cover for the series if it doesn't have one yet */
        if (!lib->folders[state->folder_idx].cover)
            generate_series_mosaic(renderer, lib, state->folder_idx,
                                   cache->season_textures, cache->season_tex_count,
                                   default_cover);
        draw_season_list(renderer, font, font_small, state, cache, lib,
                         default_cover, theme, win_w, win_h);
    } else {
        ensure_backdrop(renderer, cache, lib, state->folder_idx, win_w, win_h);
        draw_file_list(renderer, font, state, cache, lib, theme, win_w, win_h);
    }

    /* Exit-confirm toast — auto-clears after 3 s */
    if (state->exit_confirm) {
        if (SDL_GetTicks() - state->exit_confirm_at > 3000) {
            state->exit_confirm = 0;
        } else {
            static const HintItem toast_hint = { "B", "again to exit" };
            int hint_h  = sc(24, win_w);
            int toast_h = sc(30, win_w);
            int toast_w = win_w / 2;
            int toast_x = (win_w - toast_w) / 2;
            int toast_y = win_h - hint_h - sc(6, win_w) - toast_h;

            int toast_rad = sc(12, win_w);
            fill_rounded_rect(renderer, toast_x, toast_y, toast_w, toast_h, toast_rad,
                              0, 0, 0, 210);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            int glyph_h = TTF_FontHeight(font_small)
                          + TTF_FontHeight(font_small) * 2 / 7;
            int iw = hintbar_item_width(font_small, &toast_hint, glyph_h);
            int ix = toast_x + (toast_w - iw) / 2;
            hintbar_draw_items(renderer, font_small, &toast_hint, 1,
                               theme, ix, toast_y, toast_h);
        }
    }

    draw_hint_bar(renderer, font, font_small, state, theme, win_w, win_h);
}

int browser_handle_event(BrowserState *state, const MediaLibrary *lib,
                         const SDL_Event *ev) {
    if (ev->type != SDL_KEYDOWN) return 0;

    SDL_Keycode key = ev->key.keysym.sym;

    /* Allow held-key repeat only for navigation */
    if (ev->key.repeat &&
        key != SDLK_UP && key != SDLK_DOWN &&
        key != SDLK_LEFT && key != SDLK_RIGHT)
        return 0;

    if (state->view == VIEW_FOLDERS) {
        int count = lib->folder_count;
        LayoutMetrics m = compute_metrics(state->layout, 640, 480);

        if (state->exit_confirm && key != SDLK_LCTRL && key != SDLK_BACKSPACE)
            state->exit_confirm = 0;

        if (key == SDLK_LCTRL || key == SDLK_BACKSPACE) {
            if (state->exit_confirm) {
                state->action = BROWSER_ACTION_QUIT;
            } else {
                state->exit_confirm    = 1;
                state->exit_confirm_at = SDL_GetTicks();
            }
            return 1;
        }

        if (key == SDLK_UP)    { state->selected -= m.cols; if (state->selected < 0) state->selected = 0; return 1; }
        if (key == SDLK_DOWN)  { state->selected += m.cols; if (state->selected >= count) state->selected = count - 1; return 1; }
        if (key == SDLK_LEFT)  { if (state->selected > 0)         state->selected--; return 1; }
        if (key == SDLK_RIGHT) { if (state->selected < count - 1) state->selected++; return 1; }

        if (key == SDLK_RETURN || key == SDLK_SPACE) {
            if (count > 0) {
                state->folder_idx = state->selected;
                state->selected   = 0;
                state->scroll_row = 0;
                const MediaFolder *mf = &lib->folders[state->folder_idx];
                if (mf->is_series) {
                    state->season_idx    = 0;
                    state->season_scroll = 0;
                    state->view = VIEW_SEASONS;
                } else {
                    /* Audiobook: play directly from first file */
                    state->season_idx = -1;
                    int fcount;
                    const AudioFile *files = current_files(state, lib, &fcount);
                    if (fcount > 0) {
                        snprintf(state->action_path, sizeof(state->action_path),
                                 "%s", files[0].path);
                        state->action = BROWSER_ACTION_PLAY;
                    }
                }
            }
            return 1;
        }
        if (key == SDLK_RCTRL || key == SDLK_TAB) {
            state->layout = (state->layout + 1) % LAYOUT_COUNT;
            state->action = BROWSER_ACTION_LAYOUT_CHANGED;
            return 1;
        }
        if (key == SDLK_LSHIFT && count > 0) {
            state->folder_idx = state->selected;
            state->action = BROWSER_ACTION_FETCH_ART;
            return 1;
        }

    } else if (state->view == VIEW_SEASONS) {
        const MediaFolder *show = &lib->folders[state->folder_idx];
        int count = show->season_count;
        int cols  = compute_metrics(state->season_layout, 640, 480).cols;

        if (key == SDLK_UP) {
            if (state->season_idx >= cols) state->season_idx -= cols;
            else state->season_idx = 0;
            return 1;
        }
        if (key == SDLK_DOWN) {
            if (state->season_idx + cols < count) state->season_idx += cols;
            else state->season_idx = count - 1;
            return 1;
        }
        if (key == SDLK_LEFT)  { if (state->season_idx > 0)          state->season_idx--; return 1; }
        if (key == SDLK_RIGHT) { if (state->season_idx < count - 1)  state->season_idx++; return 1; }

        if (key == SDLK_RETURN || key == SDLK_SPACE) {
            if (count > 0) {
                const Season *s = &show->seasons[state->season_idx];
                if (s->file_count == 1) {
                    /* Single-file season — play directly, skip file list */
                    snprintf(state->action_path, sizeof(state->action_path),
                             "%s", s->files[0].path);
                    state->action = BROWSER_ACTION_PLAY;
                } else {
                    state->selected   = 0;
                    state->scroll_row = 0;
                    state->view       = VIEW_FILES;
                }
            }
            return 1;
        }
        if (key == SDLK_LCTRL || key == SDLK_BACKSPACE) {
            /* Back to folder grid — restore selection position */
            state->selected   = state->folder_idx;
            state->scroll_row = state->folder_idx /
                                compute_metrics(state->layout, 640, 480).cols;
            state->view = VIEW_FOLDERS;
            return 1;
        }
        if (key == SDLK_RCTRL || key == SDLK_TAB) {
            state->season_layout = (state->season_layout + 1) % LAYOUT_COUNT;
            if (state->season_layout == LAYOUT_SHOWCASE)   /* showcase is folders-only */
                state->season_layout = (state->season_layout + 1) % LAYOUT_COUNT;
            state->action = BROWSER_ACTION_LAYOUT_CHANGED;
            return 1;
        }
        if (key == SDLK_LSHIFT && count > 0) {
            state->action = BROWSER_ACTION_FETCH_ART;
            return 1;
        }

    } else { /* VIEW_FILES */
        const MediaFolder *folder = &lib->folders[state->folder_idx];
        int fcount;
        const AudioFile *files = current_files(state, lib, &fcount);

        if (key == SDLK_UP)   { if (state->selected > 0)           state->selected--; return 1; }
        if (key == SDLK_DOWN) { if (state->selected < fcount - 1)  state->selected++; return 1; }

        if (key == SDLK_RETURN || key == SDLK_SPACE) {
            if (fcount > 0) {
                snprintf(state->action_path, sizeof(state->action_path),
                         "%s", files[state->selected].path);
                state->action = BROWSER_ACTION_PLAY;
            }
            return 1;
        }
        if (key == SDLK_LCTRL || key == SDLK_BACKSPACE) {
            if (folder->is_series) {
                /* Back to season list */
                state->view = VIEW_SEASONS;
            } else {
                state->selected   = state->folder_idx;
                state->scroll_row = state->folder_idx /
                                    compute_metrics(state->layout, 640, 480).cols;
                state->view = VIEW_FOLDERS;
            }
            return 1;
        }
        if (key == SDLK_RCTRL || key == SDLK_TAB) {
            state->layout = (state->layout + 1) % LAYOUT_COUNT;
            return 1;
        }
        /* L2 / R2 — prev/next season (in show) or prev/next folder (flat) */
        if (key == SDLK_COMMA) {
            if (folder->is_series) {
                if (state->season_idx > 0) {
                    state->season_idx--;
                    state->selected   = 0;
                    state->scroll_row = 0;
                }
            } else if (state->folder_idx > 0) {
                state->folder_idx--;
                state->selected   = 0;
                state->scroll_row = 0;
            }
            return 1;
        }
        if (key == SDLK_PERIOD) {
            if (folder->is_series) {
                if (state->season_idx < folder->season_count - 1) {
                    state->season_idx++;
                    state->selected   = 0;
                    state->scroll_row = 0;
                }
            } else if (state->folder_idx < lib->folder_count - 1) {
                state->folder_idx++;
                state->selected   = 0;
                state->scroll_row = 0;
            }
            return 1;
        }
    }

    return 0;
}

void browser_state_free(BrowserState *state) {
    free(state->file_progress);
    state->file_progress   = NULL;
    state->prog_folder_idx = -1;
    state->prog_season_idx = -1;
}

void cover_cache_free(CoverCache *cache) {
    for (int i = 0; i < cache->count; i++) {
        if (cache->textures[i]) SDL_DestroyTexture(cache->textures[i]);
    }
    free(cache->textures);
    if (cache->backdrop) SDL_DestroyTexture(cache->backdrop);
    /* Free season cover textures (skip any borrowed default_cover pointers) */
    if (cache->season_textures) {
        for (int i = 0; i < cache->season_tex_count; i++) {
            /* Season textures loaded via load_cover() are owned; borrowed
               default_cover pointers must NOT be freed here.  We can't
               distinguish them easily, so we simply leak any borrowed pointer
               (it gets freed by the main caller as default_cover). */
        }
        free(cache->season_textures);
    }
    memset(cache, 0, sizeof(*cache));
}
