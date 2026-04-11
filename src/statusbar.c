#include "statusbar.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static inline int sc(int base, int w) {
    return (int)(base * w / 640.0f + 0.5f);
}

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawBlendMode(r, A == 0xff ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* Rounded rectangle outline using Bresenham midpoint circle for corners. */
static void draw_rounded_outline(SDL_Renderer *r, int x, int y, int w, int h,
                                  int rad, Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawColor(r, R, G, B, 0xff);
    if (rad <= 0) {
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderDrawRect(r, &rect);
        return;
    }
    /* Straight edges */
    SDL_RenderDrawLine(r, x + rad,     y,         x + w - 1 - rad, y);
    SDL_RenderDrawLine(r, x + rad,     y + h - 1, x + w - 1 - rad, y + h - 1);
    SDL_RenderDrawLine(r, x,           y + rad,   x,               y + h - 1 - rad);
    SDL_RenderDrawLine(r, x + w - 1,   y + rad,   x + w - 1,       y + h - 1 - rad);
    /* Corner arcs — midpoint circle per quadrant */
    int f = 1 - rad, dfx = 0, dfy = -2 * rad, px = 0, py = rad;
    while (px <= py) {
        SDL_RenderDrawPoint(r, x + rad - py,         y + rad - px);         /* TL */
        SDL_RenderDrawPoint(r, x + rad - px,         y + rad - py);
        SDL_RenderDrawPoint(r, x + w - 1 - rad + py, y + rad - px);         /* TR */
        SDL_RenderDrawPoint(r, x + w - 1 - rad + px, y + rad - py);
        SDL_RenderDrawPoint(r, x + rad - py,         y + h - 1 - rad + px); /* BL */
        SDL_RenderDrawPoint(r, x + rad - px,         y + h - 1 - rad + py);
        SDL_RenderDrawPoint(r, x + w - 1 - rad + py, y + h - 1 - rad + px); /* BR */
        SDL_RenderDrawPoint(r, x + w - 1 - rad + px, y + h - 1 - rad + py);
        px++; dfx += 2; f += dfx + 1;
        if (f >= 0) { py--; dfy += 2; f += dfy; }
    }
}

/* -------------------------------------------------------------------------
 * Cached system state
 * ---------------------------------------------------------------------- */

static int    s_battery_pct    = -1;   /* 0-100, -1 = unknown */
static int    s_charging       = 0;
static Uint32 s_battery_tick   = 0;
#define BATTERY_INTERVAL_MS 30000

static float  s_wifi_link      = -1.f; /* 0-70 link quality, -1 = unknown */
static Uint32 s_wifi_tick      = 0;
#define WIFI_INTERVAL_MS 5000

static void update_battery(void) {
    Uint32 now = SDL_GetTicks();
    if (s_battery_pct >= 0 && now - s_battery_tick < BATTERY_INTERVAL_MS) return;
    s_battery_tick = now;

    /* Try SB_BATTERY_PATH from SpruceOS env first, then known device paths */
    const char * const cap_paths[] = {
        g_battery_path[0] ? g_battery_path : NULL,          /* from SpruceOS env */
        "/sys/class/power_supply/axp2202-battery/capacity",  /* Trimui Brick */
        "/sys/class/power_supply/battery/capacity",          /* A30 / generic */
        NULL
    };
    /* Derive status path by replacing "capacity" with "status" in env path */
    char bat_status[256] = "";
    if (g_battery_path[0]) {
        const char *cap = strstr(g_battery_path, "/capacity");
        if (cap) {
            int base = (int)(cap - g_battery_path);
            if (base + 8 < (int)sizeof(bat_status)) {
                memcpy(bat_status, g_battery_path, (size_t)base);
                memcpy(bat_status + base, "/status", 8);
            }
        }
    }
    const char * const sta_paths[] = {
        bat_status[0] ? bat_status : NULL,
        "/sys/class/power_supply/axp2202-battery/status",
        "/sys/class/power_supply/battery/status",
        NULL
    };

    for (int i = 0; cap_paths[i]; i++) {
        FILE *f = fopen(cap_paths[i], "r");
        if (!f) continue;
        int pct = -1;
        if (fscanf(f, "%d", &pct) == 1 && pct >= 0 && pct <= 100)
            s_battery_pct = pct;
        fclose(f);
        break;
    }

    for (int i = 0; sta_paths[i]; i++) {
        FILE *f = fopen(sta_paths[i], "r");
        if (!f) continue;
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), f))
            s_charging = (strncmp(buf, "Charging", 8) == 0);
        fclose(f);
        break;
    }
}

static void update_wifi(void) {
    Uint32 now = SDL_GetTicks();
    if (s_wifi_link >= 0.f && now - s_wifi_tick < WIFI_INTERVAL_MS) return;
    s_wifi_tick = now;
    s_wifi_link = -1.f;  /* reset each poll; re-set below only if interface is up */

    /* Try /proc/net/wireless first (A30/generic), then sysfs carrier (Brick) */
    FILE *f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            /* Line: " wlan0: 0000   70.  -40.  -256 ..." */
            int status = 0;
            float link = 0.f;
            if (sscanf(colon + 1, " %d %f", &status, &link) == 2) {
                if (link < 0.f) link = 0.f;
                if (link > 70.f) link = 70.f;
                s_wifi_link = link;
                break;
            }
        }
        fclose(f);
        return;
    }

    /* Fallback: sysfs carrier (1=connected) — treat connected as full signal */
    f = fopen("/sys/class/net/wlan0/carrier", "r");
    if (f) {
        int carrier = 0;
        if (fscanf(f, "%d", &carrier) == 1 && carrier == 1)
            s_wifi_link = 70.f;  /* show as connected */
        fclose(f);
    }
}

/* -------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------- */

/* Cached text texture — rebuilt only when text or colour changes. */
typedef struct {
    SDL_Texture *tex;
    int          w, h;
    char         text[16];
    Uint8        cr, cg, cb;
} TextCache;

static TextCache s_clock_cache = {0};
static TextCache s_title_cache = {0};
static TextCache s_batt_cache  = {0};

static SDL_Texture *get_text_tex(SDL_Renderer *r, TTF_Font *font,
                                  const char *text, Uint8 R, Uint8 G, Uint8 B,
                                  TextCache *c) {
    if (c->tex && c->cr == R && c->cg == G && c->cb == B &&
            strncmp(c->text, text, sizeof(c->text)) == 0)
        return c->tex;
    if (c->tex) SDL_DestroyTexture(c->tex);
    SDL_Color col = {R, G, B, 0xff};
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, col);
    if (!s) { c->tex = NULL; return NULL; }
    c->tex = SDL_CreateTextureFromSurface(r, s);
    c->w = s->w; c->h = s->h;
    SDL_FreeSurface(s);
    strncpy(c->text, text, sizeof(c->text) - 1);
    c->text[sizeof(c->text) - 1] = '\0';
    c->cr = R; c->cg = G; c->cb = B;
    return c->tex;
}

static void draw_cached_text(SDL_Renderer *r, TTF_Font *font,
                              const char *text, int x, int y_center,
                              Uint8 R, Uint8 G, Uint8 B, TextCache *cache) {
    SDL_Texture *tex = get_text_tex(r, font, text, R, G, B, cache);
    if (!tex) return;
    SDL_Rect dst = {x, y_center - cache->h / 2, cache->w, cache->h};
    SDL_RenderCopy(r, tex, NULL, &dst);
}

/* Returns rendered text width without drawing. */
static int text_width(TTF_Font *font, const char *text) {
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text, &w, &h);
    return w;
}

/* Draw 4-bar WiFi signal indicator. Right edge at x_right, vertically
   centered at y_center. Returns the left edge x used. */
static int draw_wifi_bars(SDL_Renderer *r, int x_right, int y_center,
                           int bar_h, Uint8 R, Uint8 G, Uint8 B) {
    int bars_filled = 0;
    if (s_wifi_link >= 0.f) {
        bars_filled = (int)(s_wifi_link / 70.f * 4.f + 0.5f);
        if (bars_filled > 4) bars_filled = 4;
    }

    int bw   = sc(3, bar_h * 30);   /* bar width ~3px at 640 */
    if (bw < 2) bw = 2;
    int gap  = sc(1, bar_h * 30);
    if (gap < 1) gap = 1;
    int total_w = 4 * bw + 3 * gap;

    /* Bar heights (tallest on right) */
    int heights[4];
    int max_h = bar_h - 2;
    for (int i = 0; i < 4; i++)
        heights[i] = max_h * (i + 1) / 4;

    int x = x_right - total_w;
    for (int i = 0; i < 4; i++) {
        int bx = x + i * (bw + gap);
        int bht = heights[i];
        int by  = y_center + max_h / 2 - bht;

        if (i < bars_filled) {
            fill_rect(r, bx, by, bw, bht, R, G, B, 0xff);
        } else {
            fill_rect(r, bx, by, bw, bht, R, G, B, 60);
        }
    }
    return x;
}

/* Draw battery icon. Right edge at x_right, vertically centered at y_center.
   Returns the left edge x used. */
static int draw_battery(SDL_Renderer *r, int x_right, int y_center,
                         int bar_h, Uint8 R, Uint8 G, Uint8 B) {
    int bh  = bar_h - 4;    /* icon height */
    if (bh < 6) bh = 6;
    int bw  = bh * 2;       /* icon width (2:1 aspect) */
    int nub = sc(2, bh * 30); /* nub width */
    if (nub < 2) nub = 2;
    int nubh = bh / 2;

    int ix = x_right - bw - nub;
    int iy = y_center - bh / 2;

    int brad = (bh > 6) ? 2 : 1;  /* subtle corner radius for battery body */

    /* Nub (right side) — small rounded cap */
    fill_rect(r, ix + bw, iy + (bh - nubh) / 2, nub, nubh, R, G, B, 0xff);

    /* Outline — rounded */
    draw_rounded_outline(r, ix, iy, bw, bh, brad, R, G, B);

    /* Fill level */
    int pad  = 2;
    int fill_w = s_battery_pct >= 0
                 ? (int)((bw - pad * 2) * s_battery_pct / 100.f + 0.5f)
                 : 0;
    if (fill_w > 0) {
        /* Low battery: use dim fill; charging: slightly brighter */
        Uint8 fa = s_battery_pct < 20 ? 140 : 0xff;
        fill_rect(r, ix + pad, iy + pad, fill_w, bh - pad * 2, R, G, B, fa);
    }

    /* Charging indicator: "+" drawn inside */
    if (s_charging && bw > 8) {
        int cx = ix + bw / 2;
        int cy = iy + bh / 2;
        int arm = bh / 4;
        SDL_SetRenderDrawColor(r, 0, 0, 0, 0xff);
        SDL_RenderDrawLine(r, cx - arm, cy, cx + arm, cy);
        SDL_RenderDrawLine(r, cx, cy - arm, cx, cy + arm);
    }

    return ix;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int statusbar_height(int win_w) {
    return sc(40, win_w);
}

void statusbar_draw(SDL_Renderer *renderer, TTF_Font *font,
                    const Theme *theme, int win_w, int win_h) {
    (void)win_h;

    update_battery();
    update_wifi();

    int bar_h = statusbar_height(win_w);
    int pad   = sc(6, win_w);
    int mid_y = bar_h / 2;

    /* Background */
    fill_rect(renderer, 0, 0, win_w, bar_h,
              theme->background.r, theme->background.g, theme->background.b, 0xff);

    /* Separator line at bottom of bar */
    SDL_SetRenderDrawColor(renderer,
        theme->secondary.r, theme->secondary.g, theme->secondary.b, 0x60);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderDrawLine(renderer, 0, bar_h - 1, win_w - 1, bar_h - 1);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    Uint8 fr = theme->statusbar_fg.r, fg = theme->statusbar_fg.g, fb = theme->statusbar_fg.b;

    /* ---- Left: clock ---- */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char clock_buf[8];
    strftime(clock_buf, sizeof(clock_buf), "%H:%M", tm);
    draw_cached_text(renderer, font, clock_buf, pad, mid_y, fr, fg, fb,
                     &s_clock_cache);

    /* ---- Right: wifi bars + battery ---- */
    int x = win_w - pad;
    int icon_h = bar_h * 4 / 5;  /* icons at 80% of bar height */

    /* Battery */
    x -= (icon_h - 4) * 2 + sc(2, win_w);  /* rough space for icon+nub */
    draw_battery(renderer, x + (icon_h - 4) * 2 + sc(2, win_w), mid_y,
                 icon_h, fr, fg, fb);

    /* Battery percentage text */
    if (s_battery_pct >= 0) {
        char pct_buf[8];
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", s_battery_pct);
        SDL_Texture *ptex = get_text_tex(renderer, font, pct_buf, fr, fg, fb,
                                         &s_batt_cache);
        int pw = ptex ? s_batt_cache.w : text_width(font, pct_buf);
        x -= pw + sc(4, win_w);
        if (ptex) {
            SDL_Rect dst = {x, mid_y - s_batt_cache.h / 2,
                            s_batt_cache.w, s_batt_cache.h};
            SDL_RenderCopy(renderer, ptex, NULL, &dst);
        }
    }

    /* Wifi bars — hidden when wifi is off (s_wifi_link < 0 after a poll cycle) */
    if (s_wifi_link >= 0.f) {
        int wifi_w = sc(4 * 3 + 3, win_w);   /* approx 4 bars + gaps */
        x -= wifi_w + sc(8, win_w);
        draw_wifi_bars(renderer, x + wifi_w, mid_y, icon_h, fr, fg, fb);
    }

    /* ---- Center: "StoryBoy" ---- */
    SDL_Texture *ttex = get_text_tex(renderer, font, "StoryBoy", fr, fg, fb,
                                      &s_title_cache);
    int title_w = ttex ? s_title_cache.w : text_width(font, "StoryBoy");
    if (ttex) {
        SDL_Rect dst = {(win_w - title_w) / 2, mid_y - s_title_cache.h / 2,
                        title_w, s_title_cache.h};
        SDL_RenderCopy(renderer, ttex, NULL, &dst);
    }
}
