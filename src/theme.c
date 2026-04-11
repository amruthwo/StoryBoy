#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL2/SDL_image.h>

/* nanosvg — single-header, compiled exactly once here */
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../thirdparty/nanosvg.h"
#include "../thirdparty/nanosvgrast.h"

/* -------------------------------------------------------------------------
 * Theme table
 * ---------------------------------------------------------------------- */

/*
 * Cover icon color layout:  body / tab / shadow / screen / play
 * Folder color layout:       tab / screen / body
 */
const Theme THEMES[] = {
    /*            bg              text            secondary       hl_bg           hl_text         sbar_fg */
    { "SPRUCE",
      {0x14,0x23,0x1e}, {0xd2,0xe1,0xd2}, {0x64,0x82,0x6e}, {0x32,0x64,0x46}, {0xff,0xff,0xff}, {0xd2,0xe1,0xd2},
      /* cover: body=text, tab=hl_bg, shadow=dark, screen=secondary, play=hl_text */
      {0xd2,0xe1,0xd2}, {0x32,0x64,0x46}, {0x17,0x29,0x2d}, {0x64,0x82,0x6e}, {0xff,0xff,0xff},
      /* folder: tab, screen, body */
      {0x32,0x64,0x46}, {0xd2,0xe1,0xd2}, {0x64,0x82,0x6e} },

    { "monochrome",
      {0x00,0x00,0x00}, {0xf0,0xf0,0xf0}, {0x80,0x80,0x80}, {0xf0,0xf0,0xf0}, {0x00,0x00,0x00}, {0xf0,0xf0,0xf0},
      /* cover */
      {0x80,0x80,0x80}, {0xf0,0xf0,0xf0}, {0x17,0x29,0x2d}, {0xf0,0xf0,0xf0}, {0x80,0x80,0x80},
      /* folder */
      {0x40,0x40,0x40}, {0xf0,0xf0,0xf0}, {0x80,0x80,0x80} },

    { "light_contrast",
      {0xff,0xff,0xff}, {0x00,0x00,0x00}, {0xa0,0xa0,0xa0}, {0xa3,0x51,0xc8}, {0xfa,0xfa,0xfa}, {0xa3,0x51,0xc8},
      /* cover */
      {0xa3,0x51,0xc8}, {0xa0,0xa0,0xa0}, {0x00,0x00,0x00}, {0xfa,0xfa,0xfa}, {0xa3,0x51,0xc8},
      /* folder */
      {0x00,0x00,0x00}, {0xa0,0xa0,0xa0}, {0xa3,0x51,0xc8} },

    { "light_sepia",
      {0xfa,0xf0,0xdc}, {0x00,0x00,0x00}, {0xa0,0xa0,0xa0}, {0x78,0x9c,0x70}, {0xfa,0xf0,0xdc}, {0x78,0x9c,0x70},
      /* cover */
      {0x78,0x9c,0x70}, {0xa0,0xa0,0xa0}, {0x00,0x00,0x00}, {0xfa,0xf0,0xdc}, {0x78,0x9c,0x70},
      /* folder */
      {0xa0,0xa0,0xa0}, {0xe8,0xd5,0xb0}, {0x78,0x9c,0x70} },

    { "vampire",
      {0x00,0x00,0x00}, {0xc0,0x00,0x00}, {0xc0,0x40,0x40}, {0xc0,0x00,0x00}, {0x00,0x00,0x00}, {0xc0,0x00,0x00},
      /* cover */
      {0x60,0x00,0x00}, {0xc0,0x00,0x00}, {0x00,0x00,0x00}, {0x00,0x00,0x00}, {0xc0,0x00,0x00},
      /* folder */
      {0x22,0x22,0x22}, {0xc0,0x00,0x00}, {0x60,0x00,0x00} },

    { "coffee_dark",
      {0x2b,0x1f,0x16}, {0xf5,0xe6,0xd3}, {0xa0,0x8c,0x78}, {0x6f,0x4e,0x37}, {0xff,0xff,0xff}, {0xd2,0xb4,0x8c},
      /* cover */
      {0xf5,0xe6,0xd3}, {0x6f,0x4e,0x37}, {0x17,0x29,0x2d}, {0xa0,0x8c,0x78}, {0xff,0xff,0xff},
      /* folder */
      {0x6f,0x4e,0x37}, {0xf5,0xe6,0xd3}, {0xa0,0x8c,0x78} },

    { "cream_latte",
      {0xf5,0xe6,0xd3}, {0x2b,0x1f,0x16}, {0x78,0x64,0x50}, {0xd2,0xb4,0x8c}, {0x2b,0x1f,0x16}, {0x6f,0x4e,0x37},
      /* cover */
      {0xd2,0xb4,0x8c}, {0x78,0x64,0x50}, {0x2b,0x1f,0x16}, {0x2b,0x1f,0x16}, {0xd2,0xb4,0x8c},
      /* folder */
      {0x2b,0x1f,0x16}, {0x78,0x64,0x50}, {0xd2,0xb4,0x8c} },

    { "nautical",
      {0x0f,0x19,0x2d}, {0xd4,0xaf,0x37}, {0x78,0x8c,0xb4}, {0x38,0x58,0x9a}, {0xff,0xdc,0x64}, {0xd4,0xaf,0x37},
      /* cover */
      {0x78,0x8c,0xb4}, {0x38,0x58,0x9a}, {0x17,0x29,0x2d}, {0xff,0xdc,0x64}, {0x38,0x58,0x9a},
      /* folder */
      {0x38,0x58,0x9a}, {0xff,0xdc,0x64}, {0x78,0x8c,0xb4} },

    { "nordic_frost",
      {0xec,0xef,0xf4}, {0x2e,0x34,0x40}, {0x81,0xa1,0xc1}, {0x88,0xc0,0xd0}, {0x2e,0x34,0x40}, {0x88,0xc0,0xd0},
      /* cover */
      {0x2e,0x34,0x40}, {0x88,0xc0,0xd0}, {0x17,0x29,0x2d}, {0x81,0xa1,0xc1}, {0x2e,0x34,0x40},
      /* folder */
      {0x81,0xa1,0xc1}, {0x88,0xc0,0xd0}, {0x2e,0x34,0x40} },

    { "night",
      {0x0d,0x0d,0x10}, {0xf2,0xf2,0xf2}, {0x88,0x88,0xa0}, {0x7a,0xb2,0xde}, {0x0d,0x0d,0x10}, {0x7a,0xb2,0xde},
      /* cover */
      {0x52,0x52,0x5e}, {0x7a,0xb2,0xde}, {0x0d,0x0d,0x10}, {0x7a,0xb2,0xde}, {0x0d,0x0d,0x10},
      /* folder */
      {0x52,0x52,0x5e}, {0x7a,0xb2,0xde}, {0x88,0x88,0xa0} },
};
const int THEME_COUNT = (int)(sizeof(THEMES) / sizeof(THEMES[0]));

/* -------------------------------------------------------------------------
 * Active theme
 * ---------------------------------------------------------------------- */

static int  g_theme_idx      = 0;  /* default: SPRUCE */
static int  g_firstrun_done  = 0;
static char g_books_api_key[128] = "";
static int  g_layout         = 0;  /* 0=LARGE, 1=SMALL, 2=LIST, 3=SHOWCASE */
static int  g_season_layout  = 0;

const Theme *theme_get(void) {
    return &THEMES[g_theme_idx];
}

void theme_cycle(void) {
    g_theme_idx = (g_theme_idx + 1) % THEME_COUNT;
}

int theme_set(const char *name) {
    for (int i = 0; i < THEME_COUNT; i++) {
        if (strcasecmp(THEMES[i].name, name) == 0) {
            g_theme_idx = i;
            return 1;
        }
    }
    return 0;
}

int  config_firstrun_done(void) { return g_firstrun_done; }
void config_set_firstrun_done(void) { g_firstrun_done = 1; }

const char *config_books_api_key(void) { return g_books_api_key; }
void config_set_books_api_key(const char *key) {
    strncpy(g_books_api_key, key ? key : "", sizeof(g_books_api_key) - 1);
    g_books_api_key[sizeof(g_books_api_key) - 1] = '\0';
}

int  config_get_layout(void)        { return g_layout; }
void config_set_layout(int l)       { g_layout = l; }
int  config_get_season_layout(void) { return g_season_layout; }
void config_set_season_layout(int l){ g_season_layout = l; }

void config_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "theme = %s\n", THEMES[g_theme_idx].name);
    fprintf(f, "firstrun_done = %d\n", g_firstrun_done);
    if (g_books_api_key[0])
        fprintf(f, "books_api_key = %s\n", g_books_api_key);
    fprintf(f, "layout = %d\n", g_layout);
    fprintf(f, "season_layout = %d\n", g_season_layout);
    fclose(f);
}

/* -------------------------------------------------------------------------
 * storyboy.conf  —  key=value, # comments, whitespace trimmed
 * ---------------------------------------------------------------------- */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

void config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcasecmp(key, "theme") == 0) {
            if (!theme_set(val))
                fprintf(stderr, "storyboy.conf: unknown theme '%s'\n", val);
        } else if (strcasecmp(key, "firstrun_done") == 0) {
            g_firstrun_done = (atoi(val) != 0);
        } else if (strcasecmp(key, "books_api_key") == 0) {
            strncpy(g_books_api_key, val, sizeof(g_books_api_key) - 1);
            g_books_api_key[sizeof(g_books_api_key) - 1] = '\0';
        } else if (strcasecmp(key, "layout") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < 4) g_layout = v;
        } else if (strcasecmp(key, "season_layout") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < 4) g_season_layout = v;
        }
    }
    fclose(f);
}

/* Read a single-line key from a file, stripping whitespace. */
static void load_key_file(const char *path, char *dst, size_t dst_sz) {
    if (dst[0]) return;  /* already set — storyboy.conf takes priority */
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (s[0] && s[0] != '#') {
            strncpy(dst, s, dst_sz - 1);
            dst[dst_sz - 1] = '\0';
            break;
        }
    }
    fclose(f);
}

void config_load_api_keys(const char *api_dir) {
    char path[768];
    snprintf(path, sizeof(path), "%s/GoogleBooks_API.txt", api_dir);
    load_key_file(path, g_books_api_key, sizeof(g_books_api_key));
}

/* -------------------------------------------------------------------------
 * SVG recoloring
 *
 * The default_cover.svg style block maps classes to colors:
 *   cls-1  fill  →  cover_body
 *   cls-2  fill  →  cover_tab
 *   cls-3  (shadow opacity — unchanged)
 *   cls-4  (shadow fill — unchanged)
 *   cls-5  fill  →  cover_screen
 *   cls-6  fill  →  cover_play
 * ---------------------------------------------------------------------- */

static void str_replace_inplace(char *buf, size_t buf_size,
                                 const char *old, const char *new_s) {
    size_t old_len = strlen(old);
    size_t new_len = strlen(new_s);
    char *pos = buf;

    while ((pos = strstr(pos, old)) != NULL) {
        if (new_len != old_len) {
            size_t tail = strlen(pos + old_len) + 1;
            size_t used = (size_t)(pos - buf) + new_len + tail;
            if (used > buf_size) break;
            memmove(pos + new_len, pos + old_len, tail);
        }
        memcpy(pos, new_s, new_len);
        pos += new_len;
    }
}

static void rgb_to_hex(const RGB *c, char out[8]) {
    snprintf(out, 8, "#%02x%02x%02x", c->r, c->g, c->b);
}

static const char *SVG_ORIG_BODY   = "#dde5e8";
static const char *SVG_ORIG_TAB    = "#71c6c4";
static const char *SVG_ORIG_SHADOW = "#17292d";
static const char *SVG_ORIG_SCREEN = "#afc3c9";
static const char *SVG_ORIG_PLAY   = "#ffffff";

static char *svg_recolor(const char *svg_orig, const Theme *theme) {
    size_t len = strlen(svg_orig);
    size_t buf_size = len + 256;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;
    memcpy(buf, svg_orig, len + 1);

    char hex[8];
    rgb_to_hex(&theme->cover_body,   hex); str_replace_inplace(buf, buf_size, SVG_ORIG_BODY,   hex);
    rgb_to_hex(&theme->cover_tab,    hex); str_replace_inplace(buf, buf_size, SVG_ORIG_TAB,    hex);
    rgb_to_hex(&theme->cover_shadow, hex); str_replace_inplace(buf, buf_size, SVG_ORIG_SHADOW, hex);
    rgb_to_hex(&theme->cover_screen, hex); str_replace_inplace(buf, buf_size, SVG_ORIG_SCREEN, hex);
    rgb_to_hex(&theme->cover_play,   hex); str_replace_inplace(buf, buf_size, SVG_ORIG_PLAY,   hex);

    return buf;
}

static const char *FOLDER_ORIG_TAB    = "#71c6c4";
static const char *FOLDER_ORIG_SCREEN = "#dde5e8";
static const char *FOLDER_ORIG_BODY   = "#afc3c9";

static char *svg_recolor_folder(const char *svg_orig, const Theme *theme) {
    size_t len = strlen(svg_orig);
    size_t buf_size = len + 64;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;
    memcpy(buf, svg_orig, len + 1);

    char hex[8];
    rgb_to_hex(&theme->folder_tab,    hex); str_replace_inplace(buf, buf_size, FOLDER_ORIG_TAB,    hex);
    rgb_to_hex(&theme->folder_screen, hex); str_replace_inplace(buf, buf_size, FOLDER_ORIG_SCREEN, hex);
    rgb_to_hex(&theme->folder_body,   hex); str_replace_inplace(buf, buf_size, FOLDER_ORIG_BODY,   hex);

    return buf;
}

/* -------------------------------------------------------------------------
 * File helper
 * ---------------------------------------------------------------------- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* -------------------------------------------------------------------------
 * Rasterize SVG → SDL_Texture
 * ---------------------------------------------------------------------- */

static unsigned char *svg_rasterize(const char *svg_path) {
    char *svg_orig = read_file(svg_path);
    if (!svg_orig) {
        fprintf(stderr, "theme: could not read %s\n", svg_path);
        return NULL;
    }

    char *svg_colored = svg_recolor(svg_orig, theme_get());
    free(svg_orig);
    if (!svg_colored) return NULL;

    NSVGimage *img = nsvgParse(svg_colored, "px", 96.0f);
    free(svg_colored);
    if (!img) {
        fprintf(stderr, "theme: nsvgParse failed\n");
        return NULL;
    }

    float scale = (float)COVER_SIZE / img->width;
    unsigned char *pixels = malloc((size_t)(COVER_SIZE * COVER_SIZE * 4));
    if (!pixels) { nsvgDelete(img); return NULL; }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { free(pixels); nsvgDelete(img); return NULL; }

    nsvgRasterize(rast, img, 0, 0, scale, pixels, COVER_SIZE, COVER_SIZE, COVER_SIZE * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    return pixels;
}

SDL_Texture *theme_render_cover(SDL_Renderer *renderer, const char *svg_path) {
    unsigned char *pixels = svg_rasterize(svg_path);
    if (!pixels) return NULL;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, COVER_SIZE, COVER_SIZE, 32, COVER_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        free(pixels);
        fprintf(stderr, "theme: SDL_CreateRGBSurfaceWithFormatFrom: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    free(pixels);
    return tex;
}

SDL_Texture *theme_render_folder_cover(SDL_Renderer *renderer, const char *svg_path) {
    char *svg_orig = read_file(svg_path);
    if (!svg_orig) {
        fprintf(stderr, "theme: could not read %s\n", svg_path);
        return NULL;
    }

    char *svg_colored = svg_recolor_folder(svg_orig, theme_get());
    free(svg_orig);
    if (!svg_colored) return NULL;

    NSVGimage *img = nsvgParse(svg_colored, "px", 96.0f);
    free(svg_colored);
    if (!img) {
        fprintf(stderr, "theme: nsvgParse failed (folder)\n");
        return NULL;
    }

    float scale = (float)COVER_SIZE / img->width;
    unsigned char *pixels = malloc((size_t)(COVER_SIZE * COVER_SIZE * 4));
    if (!pixels) { nsvgDelete(img); return NULL; }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { free(pixels); nsvgDelete(img); return NULL; }

    nsvgRasterize(rast, img, 0, 0, scale, pixels, COVER_SIZE, COVER_SIZE, COVER_SIZE * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, COVER_SIZE, COVER_SIZE, 32, COVER_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) { free(pixels); return NULL; }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    free(pixels);
    return tex;
}
