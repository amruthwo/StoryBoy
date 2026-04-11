#ifdef SB_A30
/* -------------------------------------------------------------------------
 * a30_screen.c — Miyoo A30 framebuffer + evdev input
 *
 * Display topology
 * ----------------
 *   Physical panel  : 480 × 640, landscape-mounted (wired as 480 wide × 640 tall)
 *   GVU canvas      : 640 × 480 (landscape game-like layout)
 *   Required rotation: 90° CCW to map GVU's (x,y) → panel's (col, row)
 *
 *     GVU pixel (x, y)  →  panel pixel (y, W-1-x)
 *     where W = GVU canvas width = 640
 *
 *   fb0 is the overlay layer; PyUI leaves yoffset = 640 (page 1 shown).
 *   We write to page 1 (offset = fb_yoffset * fb_stride_px).
 *   Every pixel must have alpha = 0xFF (overlay transparency).
 *
 * Button / key mapping (A30 SpruceOS)
 * ------------------------------------
 *   A       KEY_SPACE      (57)  → SDLK_SPACE
 *   B       KEY_LEFTCTRL   (29)  → SDLK_LCTRL
 *   X       KEY_LEFTSHIFT  (42)  → SDLK_LSHIFT
 *   Y       KEY_LEFTALT    (56)  → SDLK_LALT
 *   L1      KEY_TAB        (15)  → SDLK_PAGEUP
 *   R1      KEY_BACKSPACE  (14)  → SDLK_PAGEDOWN
 *   L2      KEY_E          (18)  → SDLK_COMMA
 *   R2      KEY_T          (20)  → SDLK_PERIOD
 *   SELECT  KEY_RIGHTCTRL  (97)  → SDLK_RCTRL
 *   START   KEY_ENTER      (28)  → SDLK_RETURN
 *   MENU    KEY_ESC         (1)  → SDLK_ESCAPE
 *   UP      KEY_UP         (103) → SDLK_UP
 *   DOWN    KEY_DOWN       (108) → SDLK_DOWN
 *   LEFT    KEY_LEFT       (105) → SDLK_LEFT
 *   RIGHT   KEY_RIGHT      (106) → SDLK_RIGHT
 * ---------------------------------------------------------------------- */

#include "a30_screen.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <errno.h>
#include <SDL2/SDL.h>
#ifdef __ARM_NEON__
#include <arm_neon.h>
#endif

/* -------------------------------------------------------------------------
 * Framebuffer state
 * ---------------------------------------------------------------------- */

#define FB_DEVICE   "/dev/fb0"
/* Input device: resolved at runtime from g_input_dev (set by platform_init_from_env) */

/* GVU canvas dimensions: g_display_w × g_display_h (set by platform_init_from_env) */
/* Physical panel dimensions: g_panel_w × g_panel_h (set by platform_init_from_env) */

static int      s_fb_fd          = -1;
static Uint32  *s_fb_mem         = NULL;   /* mmap base */
static size_t   s_fb_size        = 0;
static int      s_fb_stride      = 0;      /* pixels per row; set by a30_screen_init */
static int      s_fb_yoffset     = 0;       /* currently displayed page (rows) */
static int      s_fb_back_yoff   = 0;       /* back buffer page we write to (rows) */
/* FBIOPAN_DISPLAY blocks for a full display-scan period (~28ms) when called
 * shortly after vsync, because the fast rotation finishes near the TOP of the
 * scan rather than the bottom.  Disabling pan (always writing to the
 * displayed page) eliminates this wait entirely: flip drops from ~28ms to
 * ~3ms, the main loop runs at 60fps, and the pipeline reaches ~87% speed on
 * 720p 60fps content.  Tearing is the trade-off; for video it is subtle. */
/* s_fb_pan_disabled is set in a30_screen_init() based on the platform:
 *   rotation=270 (A30): disabled — FBIOPAN_DISPLAY blocks ~28ms on A30's sunxi driver,
 *                        worse than tearing. Direct write to displayed page instead.
 *   rotation=180 (Mini Flip): enabled — V3s/SigmaStar driver is fast; page-flip
 *                        eliminates tearing. */
static int      s_fb_pan_disabled = 1;      /* default; overridden by a30_screen_init */

static int      s_input_fd    = -1;

/* -------------------------------------------------------------------------
 * a30_screen_init
 * ---------------------------------------------------------------------- */

int a30_screen_init(void) {
    /* --- Open framebuffer --- */
    s_fb_fd = open(FB_DEVICE, O_RDWR);
    if (s_fb_fd < 0) {
        perror("a30_screen_init: open " FB_DEVICE);
        return -1;
    }

    /* Query variable screen info for yoffset + actual stride */
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("a30_screen_init: ioctl FBIOGET_*SCREENINFO");
        close(s_fb_fd); s_fb_fd = -1;
        return -1;
    }

    /* stride in pixels; finfo.line_length is bytes per line */
    s_fb_stride = (int)(finfo.line_length / (vinfo.bits_per_pixel / 8));

    s_fb_size = (size_t)finfo.smem_len;
    s_fb_mem  = (Uint32 *)mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, s_fb_fd, 0);
    if (s_fb_mem == MAP_FAILED) {
        perror("a30_screen_init: mmap fb0");
        close(s_fb_fd); s_fb_fd = -1; s_fb_mem = NULL;
        return -1;
    }

    /* PyUI leaves yoffset = 640 (page 1 displayed).
       We write to the hidden page and use FBIOPAN_DISPLAY to flip —
       this eliminates tearing without touching FBIOPUT_VSCREENINFO. */
    s_fb_yoffset   = (int)vinfo.yoffset;
    s_fb_back_yoff = (s_fb_yoffset == 0) ? g_panel_h : 0;

    /* FBIOPAN_DISPLAY fails silently on V3s/SigmaStar (Mini Flip) — the display
     * controller uses a different pan mechanism. Keep direct write (pan disabled)
     * for all A30-family platforms. Tearing is acceptable for video; NEON flip
     * keeps it under 2ms so the frame budget impact is minimal. */
    s_fb_pan_disabled = 1;

    fprintf(stderr, "a30_screen: fb0 %dx%d bpp=%d stride=%d yoff=%d back_yoff=%d pan=%s\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
            s_fb_stride, s_fb_yoffset, s_fb_back_yoff,
            s_fb_pan_disabled ? "off" : "on");

    /* --- Open evdev input --- */
    s_input_fd = open(g_input_dev, O_RDONLY | O_NONBLOCK);
    if (s_input_fd < 0) {
        /* Non-fatal: fall back to no hardware input */
        fprintf(stderr, "a30_screen_init: open %s: %s (non-fatal)\n",
                g_input_dev, strerror(errno));
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * a30_flip  — rotate 90° CCW into the back buffer, then FBIOPAN_DISPLAY
 *
 * Double-buffering: we always write to the hidden page and pan the display
 * controller to it only after the write is complete. This eliminates tearing
 * without touching FBIOPUT_VSCREENINFO (which would break the overlay).
 *
 * GVU pixel (x, y)  →  panel pixel at row=(SB_W-1-x), col=y
 *   panel index = (SB_W-1-x) * s_fb_stride + y
 * ---------------------------------------------------------------------- */

void a30_flip(SDL_Surface *surface) {
    if (!s_fb_mem || !surface) return;

    SDL_LockSurface(surface);

    const Uint32 *src   = (const Uint32 *)surface->pixels;
    const int     pitch = surface->pitch / 4;   /* pixels per row */

    int dst_yoff = s_fb_pan_disabled ? s_fb_yoffset : s_fb_back_yoff;
    Uint32 *dst  = s_fb_mem + (size_t)dst_yoff * (size_t)s_fb_stride;

    if (g_display_rotation == 0) {
        /* Landscape direct blit (Miyoo Mini / rotation=0 devices).
         * g_panel_w == g_display_w, g_panel_h == g_display_h.
         * Panel widths on supported devices (640) are divisible by 8. */
#ifdef __ARM_NEON__
        const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
        for (int r = 0; r < g_panel_h; r++) {
            const Uint32 *in  = src + (size_t)r * (size_t)pitch;
            Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
            for (int c = 0; c < g_panel_w; c += 8) {
                vst1q_u32(out + c,     vorrq_u32(vld1q_u32(in + c),     alpha_v));
                vst1q_u32(out + c + 4, vorrq_u32(vld1q_u32(in + c + 4), alpha_v));
            }
        }
#else
        for (int r = 0; r < g_panel_h; r++) {
            const Uint32 *in  = src + (size_t)r * (size_t)pitch;
            Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
            for (int c = 0; c < g_panel_w; c++)
                out[c] = in[c] | 0xFF000000u;
        }
#endif
    } else if (g_display_rotation == 180) {
        /* 180° rotation (upside-down panels, e.g. Miyoo Mini Flip/V4).
         * dst row r = src row (g_panel_h-1-r), pixels reversed left-to-right.
         * 752 = 8×94 — inner loop is exact with no tail. */
#ifdef __ARM_NEON__
        const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
        for (int r = 0; r < g_panel_h; r++) {
            const Uint32 *in  = src + (size_t)(g_panel_h - 1 - r) * (size_t)pitch;
            Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
            /* Read 8 pixels from the end of the source row, reverse them,
             * store to the front of the destination row. Repeat backwards. */
            int c = g_panel_w - 8;
            while (c >= 0) {
                uint32x4_t lo = vld1q_u32(in + c);      /* [a,b,c,d] */
                uint32x4_t hi = vld1q_u32(in + c + 4);  /* [e,f,g,h] */
                /* Reverse each group of 4: vrev64q_u32 swaps pairs within 64-bit lanes,
                 * then vcombine swaps the two lanes → full reversal. */
                uint32x4_t rlo = vrev64q_u32(lo);
                rlo = vcombine_u32(vget_high_u32(rlo), vget_low_u32(rlo)); /* [d,c,b,a] */
                uint32x4_t rhi = vrev64q_u32(hi);
                rhi = vcombine_u32(vget_high_u32(rhi), vget_low_u32(rhi)); /* [h,g,f,e] */
                vst1q_u32(out,     vorrq_u32(rhi, alpha_v)); /* out[0..3] = [h,g,f,e] */
                vst1q_u32(out + 4, vorrq_u32(rlo, alpha_v)); /* out[4..7] = [d,c,b,a] */
                out += 8;
                c   -= 8;
            }
        }
#else
        for (int r = 0; r < g_panel_h; r++) {
            const Uint32 *in  = src + (size_t)(g_panel_h - 1 - r) * (size_t)pitch;
            Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
            for (int c = 0; c < g_panel_w; c++)
                out[c] = in[g_panel_w - 1 - c] | 0xFF000000u;
        }
#endif
    } else {
        /* 90° CCW rotation (A30 and other portrait-panel devices).
         * src: g_display_w × g_display_h (landscape)
         * dst: g_panel_w  × g_panel_h   (portrait, g_panel_w=480, g_panel_h=640)
         *
         * dst pixel (r, c)  =  src pixel (c, g_display_w-1-r)
         *
         * Outer loop over dst rows → sequential writes to uncached fb memory. */
#ifdef __ARM_NEON__
        const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
        for (int r = 0; r < g_panel_h; r++) {
            const int src_x = g_display_w - 1 - r;
            Uint32 *out = dst + (size_t)r * (size_t)s_fb_stride;
            /* g_panel_w=480 divisible by 8 — no tail loop needed. */
            for (unsigned int c = 0; c < (unsigned)g_panel_w; c += 8) {
                __builtin_prefetch(&src[(c+8) * pitch + src_x], 0, 0);
                uint32_t tmp[8];
                tmp[0] = src[(c+0) * pitch + src_x];
                tmp[1] = src[(c+1) * pitch + src_x];
                tmp[2] = src[(c+2) * pitch + src_x];
                tmp[3] = src[(c+3) * pitch + src_x];
                tmp[4] = src[(c+4) * pitch + src_x];
                tmp[5] = src[(c+5) * pitch + src_x];
                tmp[6] = src[(c+6) * pitch + src_x];
                tmp[7] = src[(c+7) * pitch + src_x];
                vst1q_u32(out + c,     vorrq_u32(vld1q_u32(tmp),   alpha_v));
                vst1q_u32(out + c + 4, vorrq_u32(vld1q_u32(tmp+4), alpha_v));
            }
        }
#else
        for (int r = 0; r < g_panel_h; r++) {
            const int src_x = g_display_w - 1 - r;
            Uint32 *out = dst + (size_t)r * (size_t)s_fb_stride;
            for (int c = 0; c < g_panel_w; c++)
                out[c] = src[c * pitch + src_x] | 0xFF000000u;
        }
#endif
    }

    SDL_UnlockSurface(surface);

    if (!s_fb_pan_disabled) {
        struct fb_var_screeninfo vinfo;
        if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
            vinfo.yoffset = (uint32_t)s_fb_back_yoff;
            if (ioctl(s_fb_fd, FBIOPAN_DISPLAY, &vinfo) == 0) {
                s_fb_yoffset   = s_fb_back_yoff;
                s_fb_back_yoff = (s_fb_yoffset == 0) ? g_panel_h : 0;
            }
        }
    }
}


void a30_screen_wake(void) {
    if (!s_fb_pan_disabled)
        fprintf(stderr, "a30_screen_wake: FBIOPAN_DISPLAY disabled for rest of session\n");
    s_fb_pan_disabled = 1;
}

/* -------------------------------------------------------------------------
 * Key mapping tables
 *
 * A30 and Miyoo Mini family share the same evdev key codes for most buttons
 * but differ for the shoulder buttons:
 *   A30:        L1=KEY_TAB  R1=KEY_BACKSPACE  L2=KEY_E  R2=KEY_T
 *   Mini/Flip:  L1=KEY_E    R1=KEY_T          L2=KEY_TAB  R2=KEY_BACKSPACE
 *
 * s_keymap is set to the appropriate table in a30_screen_init() based on
 * g_display_rotation (270 = A30 portrait, else Mini landscape/180).
 * ---------------------------------------------------------------------- */

typedef struct {
    int       linux_code;
    SDL_Keycode sdl_sym;
} KeyMap;

static const KeyMap s_keymap_a30[] = {
    { KEY_SPACE,      SDLK_SPACE    },   /* A        */
    { KEY_LEFTCTRL,   SDLK_LCTRL   },   /* B        */
    { KEY_LEFTSHIFT,  SDLK_LALT    },   /* X (north on A30 portrait; StoryBoy uses LALT for X) */
    { KEY_LEFTALT,    SDLK_LSHIFT  },   /* Y (west on A30 portrait;  StoryBoy uses LSHIFT for Y) */
    { KEY_TAB,        SDLK_PAGEUP  },   /* L1       */
    { KEY_BACKSPACE,  SDLK_PAGEDOWN},   /* R1       */
    { KEY_E,          SDLK_COMMA   },   /* L2       */
    { KEY_T,          SDLK_PERIOD  },   /* R2       */
    { KEY_RIGHTCTRL,  SDLK_RCTRL   },   /* SELECT   */
    { KEY_ENTER,      SDLK_RETURN  },   /* START    */
    { KEY_ESC,        SDLK_ESCAPE  },   /* MENU     */
    { KEY_UP,         SDLK_UP      },   /* D-pad UP */
    { KEY_DOWN,       SDLK_DOWN    },   /* D-pad DN */
    { KEY_LEFT,       SDLK_LEFT    },   /* D-pad LT */
    { KEY_RIGHT,      SDLK_RIGHT   },   /* D-pad RT */
    { KEY_VOLUMEUP,   SDLK_EQUALS  },   /* Vol+     */
    { KEY_VOLUMEDOWN, SDLK_MINUS   },   /* Vol-     */
    { 0,              0            },   /* sentinel */
};

/* Miyoo Mini family (Mini, Plus, V4, Flip): L1/R1 and L2/R2 evdev codes
 * are swapped compared to A30. Physical L1 → KEY_E, R1 → KEY_T,
 * L2 → KEY_TAB, R2 → KEY_BACKSPACE. */
static const KeyMap s_keymap_mini[] = {
    { KEY_SPACE,      SDLK_SPACE    },   /* A        */
    { KEY_LEFTCTRL,   SDLK_LCTRL   },   /* B        */
    { KEY_LEFTSHIFT,  SDLK_LALT    },   /* X (StoryBoy uses LALT for X) */
    { KEY_LEFTALT,    SDLK_LSHIFT  },   /* Y (StoryBoy uses LSHIFT for Y) */
    { KEY_E,          SDLK_PAGEUP  },   /* L1       */
    { KEY_T,          SDLK_PAGEDOWN},   /* R1       */
    { KEY_TAB,        SDLK_COMMA   },   /* L2       */
    { KEY_BACKSPACE,  SDLK_PERIOD  },   /* R2       */
    { KEY_RIGHTCTRL,  SDLK_RCTRL   },   /* SELECT   */
    { KEY_ENTER,      SDLK_RETURN  },   /* START    */
    { KEY_ESC,        SDLK_ESCAPE  },   /* MENU     */
    { KEY_UP,         SDLK_UP      },   /* D-pad UP */
    { KEY_DOWN,       SDLK_DOWN    },   /* D-pad DN */
    { KEY_LEFT,       SDLK_LEFT    },   /* D-pad LT */
    { KEY_RIGHT,      SDLK_RIGHT   },   /* D-pad RT */
    { KEY_VOLUMEUP,   SDLK_EQUALS  },   /* Vol+     */
    { KEY_VOLUMEDOWN, SDLK_MINUS   },   /* Vol-     */
    { 0,              0            },   /* sentinel */
};

static const KeyMap *s_keymap = NULL;

static SDL_Keycode lookup_keycode(int linux_code) {
    if (!s_keymap) return SDLK_UNKNOWN;
    for (int i = 0; s_keymap[i].linux_code; i++) {
        if (s_keymap[i].linux_code == linux_code)
            return s_keymap[i].sdl_sym;
    }
    return SDLK_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * a30_poll_events — drain evdev, inject SDL2 keyboard events
 *
 * gpio-keys-polled never generates EV_REP (value=2), so we implement
 * key-repeat ourselves: 300ms initial delay, then one repeat every 80ms.
 * Only navigation keys repeat; action buttons fire once per physical press.
 * ---------------------------------------------------------------------- */

#define KEY_REPEAT_DELAY_MS  300
#define KEY_REPEAT_PERIOD_MS  80

static SDL_Keycode s_held_key    = SDLK_UNKNOWN;
static Uint32      s_held_since  = 0;
static Uint32      s_last_repeat = 0;

static int is_repeatable(SDL_Keycode sym) {
    return sym == SDLK_UP || sym == SDLK_DOWN ||
           sym == SDLK_LEFT || sym == SDLK_RIGHT;
}

static void push_key(SDL_Keycode sym, int repeat) {
    SDL_Event sdl_ev;
    memset(&sdl_ev, 0, sizeof(sdl_ev));
    sdl_ev.type            = SDL_KEYDOWN;
    sdl_ev.key.type        = SDL_KEYDOWN;
    sdl_ev.key.state       = SDL_PRESSED;
    sdl_ev.key.repeat      = repeat;
    sdl_ev.key.keysym.sym      = sym;
    sdl_ev.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
    sdl_ev.key.keysym.mod      = KMOD_NONE;
    SDL_PushEvent(&sdl_ev);
}

void a30_poll_events(void) {
    if (s_input_fd < 0) return;

    /* One-time keymap selection: 270° = A30 layout, else Mini family layout. */
    if (!s_keymap) {
        s_keymap = (g_display_rotation == 270) ? s_keymap_a30 : s_keymap_mini;
        fprintf(stderr, "a30_screen: using %s keymap\n",
                (g_display_rotation == 270) ? "A30" : "Mini");
    }

    struct input_event ev;
    while (read(s_input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type != EV_KEY) continue;
        if (ev.value != 0 && ev.value != 1) continue;  /* ignore OS repeats */

        SDL_Keycode sym = lookup_keycode(ev.code);
        if (sym == SDLK_UNKNOWN) continue;

        if (ev.value == 1) {  /* key down */
            push_key(sym, 0);
            if (is_repeatable(sym)) {
                s_held_key    = sym;
                s_held_since  = SDL_GetTicks();
                s_last_repeat = s_held_since;
            }
        } else {              /* key up */
            SDL_Event sdl_ev;
            memset(&sdl_ev, 0, sizeof(sdl_ev));
            sdl_ev.type            = SDL_KEYUP;
            sdl_ev.key.type        = SDL_KEYUP;
            sdl_ev.key.state       = SDL_RELEASED;
            sdl_ev.key.keysym.sym      = sym;
            sdl_ev.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
            sdl_ev.key.keysym.mod      = KMOD_NONE;
            SDL_PushEvent(&sdl_ev);
            if (s_held_key == sym)
                s_held_key = SDLK_UNKNOWN;
        }
    }

    /* Inject software repeats for held navigation keys */
    if (s_held_key != SDLK_UNKNOWN) {
        Uint32 now = SDL_GetTicks();
        if (now - s_held_since >= KEY_REPEAT_DELAY_MS &&
            now - s_last_repeat >= KEY_REPEAT_PERIOD_MS) {
            push_key(s_held_key, 1);
            s_last_repeat = now;
        }
    }
}

/* -------------------------------------------------------------------------
 * a30_screen_close
 * ---------------------------------------------------------------------- */

void a30_screen_close(void) {
    if (s_input_fd >= 0) { close(s_input_fd); s_input_fd = -1; }
    if (s_fb_mem && s_fb_mem != MAP_FAILED) {
        munmap(s_fb_mem, s_fb_size);
        s_fb_mem = NULL;
    }
    if (s_fb_fd >= 0) { close(s_fb_fd); s_fb_fd = -1; }
}

#endif /* SB_A30 */
