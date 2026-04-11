#ifdef SB_TRIMUI_BRICK
/* -------------------------------------------------------------------------
 * brick_screen.c — Trimui Brick framebuffer + evdev input
 *
 * Display topology
 * ----------------
 *   Physical panel  : 1024 × 768, landscape
 *   StoryBoy canvas : 1024 × 768 (matches panel — no rotation required)
 *   Pixel format    : ARGB8888 (alpha at [31:24], R [23:16], G [15:8], B [7:0])
 *                     In memory (LE): B G R A
 *
 *   fb0 is the primary display layer.
 *   PyUI leaves yoffset = 768 (page 1 shown).
 *   We write directly to the displayed page (no FBIOPAN_DISPLAY).
 *   Every pixel must have alpha = 0xFF.
 *
 * Button / key mapping (Trimui Brick — Nintendo-style labels, Xbox evdev codes)
 * ---------------------------------------------------------------------------
 *   D-pad  ABS_HAT0X / ABS_HAT0Y  →  SDLK_LEFT/RIGHT / SDLK_UP/DOWN
 *   A      BTN_EAST   (0x131)      →  SDLK_SPACE   (confirm — right button)
 *   B      BTN_SOUTH  (0x130)      →  SDLK_LCTRL   (back — bottom button)
 *   X      BTN_WEST   (0x134)      →  SDLK_LALT    (left button)
 *   Y      BTN_NORTH  (0x133)      →  SDLK_LSHIFT  (top button)
 *   L1     BTN_TL     (0x136)      →  SDLK_PAGEUP
 *   R1     BTN_TR     (0x137)      →  SDLK_PAGEDOWN
 *   L2     ABS_Z analog (>64)      →  SDLK_COMMA
 *   R2     ABS_RZ analog (>64)     →  SDLK_PERIOD
 *   SELECT BTN_SELECT (0x13A)      →  SDLK_RCTRL
 *   START  BTN_START  (0x13B)      →  SDLK_RETURN
 *   MENU   BTN_MODE   (0x13C)      →  SDLK_ESCAPE
 *   Vol+   KEY_VOLUMEUP   (0x73)   →  SDLK_EQUALS
 *   Vol-   KEY_VOLUMEDOWN (0x72)   →  SDLK_MINUS
 * ---------------------------------------------------------------------- */

#include "brick_screen.h"

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
#include <poll.h>
#include <SDL2/SDL.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

/* -------------------------------------------------------------------------
 * Framebuffer state
 * ---------------------------------------------------------------------- */

#define FB_DEVICE   "/dev/fb0"

static int      s_fb_fd         = -1;
static Uint32  *s_fb_mem        = NULL;
static size_t   s_fb_size       = 0;
static int      s_fb_stride     = 0;
static int      s_fb_yoffset    = 0;
static int      s_fb_back_yoff  = 0;
static int      s_fb_pan_disabled = 0;

static int      s_use_drm     = 0;

static int      s_input_fd    = -1;

static void brick_pageflip(void);   /* forward declaration */

/* =========================================================================
 * DRM/KMS page-flip path
 * ========================================================================= */

#define SB_DRM_DISPLAY_MODE_LEN   32
#define SB_DRM_MODE_CONNECTED      1
#define SB_DRM_MODE_PAGE_FLIP_EVENT 0x01u
#define SB_DRM_EVENT_FLIP_COMPLETE  0x02u

struct sb_drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[SB_DRM_DISPLAY_MODE_LEN];
};
struct sb_drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct sb_drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct sb_drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};
struct sb_drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct sb_drm_mode_modeinfo mode;
};
struct sb_drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct sb_drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
struct sb_drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};
struct sb_drm_mode_destroy_dumb {
    uint32_t handle;
};
struct sb_drm_mode_page_flip {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};
struct sb_drm_event {
    uint32_t type, length;
};

#define SB_DRM_IOCTL_MODE_GETRESOURCES \
    _IOWR('d', 0xA0, struct sb_drm_mode_card_res)
#define SB_DRM_IOCTL_MODE_GETCRTC \
    _IOWR('d', 0xA1, struct sb_drm_mode_crtc)
#define SB_DRM_IOCTL_MODE_SETCRTC \
    _IOWR('d', 0xA2, struct sb_drm_mode_crtc)
#define SB_DRM_IOCTL_MODE_GETENCODER \
    _IOWR('d', 0xA6, struct sb_drm_mode_get_encoder)
#define SB_DRM_IOCTL_MODE_GETCONNECTOR \
    _IOWR('d', 0xA7, struct sb_drm_mode_get_connector)
#define SB_DRM_IOCTL_MODE_ADDFB \
    _IOWR('d', 0xAE, struct sb_drm_mode_fb_cmd)
#define SB_DRM_IOCTL_MODE_RMFB \
    _IOWR('d', 0xAF, unsigned int)
#define SB_DRM_IOCTL_MODE_PAGE_FLIP \
    _IOWR('d', 0xB0, struct sb_drm_mode_page_flip)
#define SB_DRM_IOCTL_MODE_CREATE_DUMB \
    _IOWR('d', 0xB2, struct sb_drm_mode_create_dumb)
#define SB_DRM_IOCTL_MODE_MAP_DUMB \
    _IOWR('d', 0xB3, struct sb_drm_mode_map_dumb)
#define SB_DRM_IOCTL_MODE_DESTROY_DUMB \
    _IOWR('d', 0xB4, struct sb_drm_mode_destroy_dumb)

static int      s_drm_fd         = -1;
static uint32_t s_drm_crtc_id    = 0;
static uint32_t s_drm_conn_id    = 0;
static struct sb_drm_mode_modeinfo s_drm_mode;
static uint32_t s_drm_fb[2]      = {0, 0};
static uint32_t s_drm_handle[2]  = {0, 0};
static Uint32  *s_drm_buf[2]     = {NULL, NULL};
static uint32_t s_drm_pitch      = 0;
static int      s_drm_back       = 1;
static size_t   s_drm_buf_size   = 0;

static void flip_drm_close(void) {
    for (int b = 0; b < 2; b++) {
        if (s_drm_buf[b] && s_drm_buf[b] != MAP_FAILED) {
            munmap(s_drm_buf[b], s_drm_buf_size);
            s_drm_buf[b] = NULL;
        }
        if (s_drm_fb[b]) {
            unsigned int fbid = s_drm_fb[b];
            ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_RMFB, &fbid);
            s_drm_fb[b] = 0;
        }
        if (s_drm_handle[b]) {
            struct sb_drm_mode_destroy_dumb dd = { s_drm_handle[b] };
            ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            s_drm_handle[b] = 0;
        }
    }
    if (s_drm_fd >= 0) { close(s_drm_fd); s_drm_fd = -1; }
}

static void flip_drm_pageflip(void) {
    struct sb_drm_mode_page_flip pf = {
        .crtc_id  = s_drm_crtc_id,
        .fb_id    = s_drm_fb[s_drm_back],
        .flags    = SB_DRM_MODE_PAGE_FLIP_EVENT,
    };
    if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_PAGE_FLIP, &pf) < 0) return;

    struct pollfd pfd = { .fd = s_drm_fd, .events = POLLIN };
    if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
        char evbuf[64];
        (void)read(s_drm_fd, evbuf, sizeof(evbuf));
    }
    s_drm_back ^= 1;
}

static int flip_drm_init(void) {
    s_drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (s_drm_fd < 0) {
        fprintf(stderr, "flip_drm_init: open /dev/dri/card0 failed: %s\n", strerror(errno));
        return -1;
    }

    struct sb_drm_mode_card_res res = {0};
    if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "flip_drm_init: GETRESOURCES failed: %s\n", strerror(errno));
        goto fail;
    }

    fprintf(stderr, "flip_drm_init: res: conns=%u crtcs=%u\n",
            res.count_connectors, res.count_crtcs);
    uint32_t nc = res.count_connectors ? res.count_connectors : 1;
    uint32_t nt = res.count_crtcs      ? res.count_crtcs      : 1;
    uint32_t *conn_ids = (uint32_t *)calloc(nc, sizeof(uint32_t));
    uint32_t *crtc_ids = (uint32_t *)calloc(nt, sizeof(uint32_t));
    if (!conn_ids || !crtc_ids) {
        free(conn_ids); free(crtc_ids); goto fail;
    }
    {
        uint32_t nc2 = res.count_connectors, nt2 = res.count_crtcs;
        memset(&res, 0, sizeof(res));
        res.count_connectors   = nc2;
        res.count_crtcs        = nt2;
        res.connector_id_ptr   = (uint64_t)(uintptr_t)conn_ids;
        res.crtc_id_ptr        = (uint64_t)(uintptr_t)crtc_ids;
    }
    if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        free(conn_ids); free(crtc_ids); goto fail;
    }

    for (uint32_t i = 0; i < res.count_connectors && !s_drm_conn_id; i++) {
        struct sb_drm_mode_get_connector c = { .connector_id = conn_ids[i] };
        if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) continue;
        if (c.connection != SB_DRM_MODE_CONNECTED) continue;

        s_drm_conn_id = conn_ids[i];

        uint32_t nm = c.count_modes;
        struct sb_drm_mode_modeinfo *modes =
            (struct sb_drm_mode_modeinfo *)calloc(nm, sizeof(*modes));
        if (modes) {
            struct sb_drm_mode_get_connector c2 = {
                .connector_id = s_drm_conn_id,
                .modes_ptr    = (uint64_t)(uintptr_t)modes,
                .count_modes  = nm,
            };
            ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_GETCONNECTOR, &c2);
        }

        if (c.encoder_id) {
            struct sb_drm_mode_get_encoder enc = { .encoder_id = c.encoder_id };
            if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_GETENCODER, &enc) == 0
                    && enc.crtc_id) {
                s_drm_crtc_id = enc.crtc_id;
                struct sb_drm_mode_crtc crtc = { .crtc_id = s_drm_crtc_id };
                if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_GETCRTC, &crtc) == 0
                        && crtc.mode_valid)
                    s_drm_mode = crtc.mode;
            }
        }
        if (!s_drm_mode.hdisplay && modes && nm > 0)
            s_drm_mode = modes[0];
        if (!s_drm_crtc_id && res.count_crtcs > 0)
            s_drm_crtc_id = crtc_ids[0];

        free(modes);
    }
    free(conn_ids);
    free(crtc_ids);

    if (!s_drm_conn_id || !s_drm_crtc_id || !s_drm_mode.hdisplay) {
        fprintf(stderr, "flip_drm_init: no usable connector/CRTC/mode\n");
        goto fail;
    }
    fprintf(stderr, "flip_drm_init: conn=%u crtc=%u mode=%ux%u@%u\n",
            s_drm_conn_id, s_drm_crtc_id,
            s_drm_mode.hdisplay, s_drm_mode.vdisplay, s_drm_mode.vrefresh);

    for (int b = 0; b < 2; b++) {
        struct sb_drm_mode_create_dumb cd = {
            .width  = s_drm_mode.hdisplay,
            .height = s_drm_mode.vdisplay,
            .bpp    = 32,
        };
        if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) goto fail;
        s_drm_handle[b] = cd.handle;
        s_drm_pitch     = cd.pitch;
        s_drm_buf_size  = (size_t)cd.height * (size_t)cd.pitch;

        struct sb_drm_mode_fb_cmd fb = {
            .width  = cd.width,
            .height = cd.height,
            .pitch  = cd.pitch,
            .bpp    = 32,
            .depth  = 24,
            .handle = cd.handle,
        };
        if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_ADDFB, &fb) < 0) goto fail;
        s_drm_fb[b] = fb.fb_id;

        struct sb_drm_mode_map_dumb md = { .handle = cd.handle };
        if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) goto fail;
        s_drm_buf[b] = (Uint32 *)mmap(NULL, s_drm_buf_size,
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       s_drm_fd, (off_t)md.offset);
        if (s_drm_buf[b] == MAP_FAILED) { s_drm_buf[b] = NULL; goto fail; }
        memset(s_drm_buf[b], 0, s_drm_buf_size);
    }

    {
        struct sb_drm_mode_crtc set = {
            .crtc_id              = s_drm_crtc_id,
            .fb_id                = s_drm_fb[0],
            .set_connectors_ptr   = (uint64_t)(uintptr_t)&s_drm_conn_id,
            .count_connectors     = 1,
            .mode                 = s_drm_mode,
            .mode_valid           = 1,
        };
        if (ioctl(s_drm_fd, SB_DRM_IOCTL_MODE_SETCRTC, &set) < 0) goto fail;
    }

    s_drm_back = 1;
    fprintf(stderr, "flip_drm_init: DRM/KMS ready, pitch=%u\n", s_drm_pitch);
    return 0;

fail:
    flip_drm_close();
    return -1;
}

/* -------------------------------------------------------------------------
 * brick_screen_init
 * ---------------------------------------------------------------------- */

int brick_screen_init(void) {
    if (flip_drm_init() == 0) {
        s_use_drm   = 1;
        s_fb_stride = (int)(s_drm_pitch / 4);
        s_input_fd  = open(g_input_dev, O_RDONLY | O_NONBLOCK);
        if (s_input_fd < 0)
            fprintf(stderr, "brick_screen_init: open %s: %s (non-fatal)\n",
                    g_input_dev, strerror(errno));
        return 0;
    }

    /* --- fb0 fallback --- */
    s_fb_fd = open(FB_DEVICE, O_RDWR);
    if (s_fb_fd < 0) {
        perror("brick_screen_init: open " FB_DEVICE);
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("brick_screen_init: ioctl FBIOGET_*SCREENINFO");
        close(s_fb_fd); s_fb_fd = -1;
        return -1;
    }

    s_fb_stride = (int)(finfo.line_length / (vinfo.bits_per_pixel / 8));

    {
        size_t page_bytes = (size_t)vinfo.yres * (size_t)finfo.line_length;
        if ((size_t)finfo.smem_len < 2 * page_bytes) {
            vinfo.yres_virtual = vinfo.yres * 2;
            if (ioctl(s_fb_fd, FBIOPUT_VSCREENINFO, &vinfo) == 0) {
                ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &finfo);
            } else {
                vinfo.yres_virtual = vinfo.yres;
            }
        }
    }

    s_fb_size   = (size_t)finfo.smem_len;
    s_fb_mem    = (Uint32 *)mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, s_fb_fd, 0);
    if (s_fb_mem == MAP_FAILED) {
        perror("brick_screen_init: mmap fb0");
        close(s_fb_fd); s_fb_fd = -1; s_fb_mem = NULL;
        return -1;
    }

    s_fb_yoffset   = (int)vinfo.yoffset;
    s_fb_back_yoff = (s_fb_yoffset == 0) ? BRICK_H : 0;

    {
        size_t page_bytes = (size_t)vinfo.yres * (size_t)finfo.line_length;
        if (s_fb_size < 2 * page_bytes) {
            s_fb_pan_disabled = 1;
            s_fb_back_yoff    = s_fb_yoffset;
        }
    }

    /* FBIOPAN_DISPLAY on the sunxi H700 legacy framebuffer can block
       indefinitely waiting for a VSYNC interrupt that never arrives,
       freezing the entire process.  The DRM path avoids this with a
       poll() timeout, but DRM is unavailable here (0 connectors/CRTCs).
       Disable pan: write directly to the displayed page instead.
       This matches the original design intent stated in the file header. */
    s_fb_pan_disabled = 1;
    s_fb_back_yoff    = s_fb_yoffset;

    fprintf(stderr, "brick_screen: fb0 %dx%d bpp=%d stride=%d yoff=%d back_yoff=%d (pan disabled)\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
            s_fb_stride, s_fb_yoffset, s_fb_back_yoff);

    memset(s_fb_mem, 0, s_fb_size);

    s_input_fd = open(g_input_dev, O_RDONLY | O_NONBLOCK);
    if (s_input_fd < 0)
        fprintf(stderr, "brick_screen_init: open %s: %s (non-fatal)\n",
                g_input_dev, strerror(errno));
    return 0;
}

/* -------------------------------------------------------------------------
 * brick_pageflip — internal, called by brick_flip
 * ---------------------------------------------------------------------- */

static void brick_pageflip(void) {
    if (s_use_drm) { flip_drm_pageflip(); return; }
    if (s_fb_pan_disabled) return;
    struct fb_var_screeninfo vinfo;
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        vinfo.yoffset  = (uint32_t)s_fb_back_yoff;
        vinfo.activate = 0;
        if (ioctl(s_fb_fd, FBIOPAN_DISPLAY, &vinfo) == 0) {
            s_fb_yoffset   = s_fb_back_yoff;
            s_fb_back_yoff = (s_fb_yoffset == 0) ? BRICK_H : 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * brick_flip — blit SDL surface to fb0 (no rotation, landscape panel)
 * ---------------------------------------------------------------------- */

void brick_flip(SDL_Surface *surface) {
    if (!surface || (!s_fb_mem && !s_use_drm)) return;

    SDL_LockSurface(surface);

    const Uint32 *src   = (const Uint32 *)surface->pixels;
    const int     pitch = surface->pitch / 4;
    Uint32 *dst;
    if (s_use_drm) {
        dst = s_drm_buf[s_drm_back];
    } else {
        int dst_yoff = s_fb_pan_disabled ? s_fb_yoffset : s_fb_back_yoff;
        dst = s_fb_mem + (size_t)dst_yoff * (size_t)s_fb_stride;
    }

#ifdef __aarch64__
    const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
    for (int r = 0; r < BRICK_H; r++) {
        const Uint32 *in  = src + (size_t)r * (size_t)pitch;
        Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
        for (int c = 0; c < BRICK_W; c += 8) {
            vst1q_u32(out + c,     vorrq_u32(vld1q_u32(in + c),     alpha_v));
            vst1q_u32(out + c + 4, vorrq_u32(vld1q_u32(in + c + 4), alpha_v));
        }
    }
#else
    for (int r = 0; r < BRICK_H; r++) {
        const Uint32 *in  = src + (size_t)r * (size_t)pitch;
        Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
        for (int c = 0; c < BRICK_W; c++)
            out[c] = in[c] | 0xFF000000u;
    }
#endif

    SDL_UnlockSurface(surface);
    brick_pageflip();
}

/* -------------------------------------------------------------------------
 * Input handling — Xbox gamepad evdev (EV_KEY + EV_ABS)
 * ---------------------------------------------------------------------- */

#ifndef BTN_SOUTH
#define BTN_SOUTH   0x130
#endif
#ifndef BTN_EAST
#define BTN_EAST    0x131
#endif
#ifndef BTN_NORTH
#define BTN_NORTH   0x133
#endif
#ifndef BTN_WEST
#define BTN_WEST    0x134
#endif
#ifndef BTN_TL
#define BTN_TL      0x136
#endif
#ifndef BTN_TR
#define BTN_TR      0x137
#endif

#define TRIGGER_THRESHOLD 64

typedef struct { int linux_code; SDL_Keycode sdl_sym; } KeyMap;

static const KeyMap s_keymap[] = {
    { BTN_EAST,         SDLK_SPACE    },  /* A (right)    */
    { BTN_SOUTH,        SDLK_LCTRL   },  /* B (bottom)   */
    { BTN_NORTH,        SDLK_LSHIFT  },  /* Y (top)      */
    { BTN_WEST,         SDLK_LALT   },  /* X (left)     */
    { BTN_TL,           SDLK_PAGEUP  },  /* L1           */
    { BTN_TR,           SDLK_PAGEDOWN},  /* R1           */
    { BTN_SELECT,       SDLK_RCTRL   },  /* SELECT       */
    { BTN_START,        SDLK_RETURN  },  /* START        */
    { BTN_MODE,         SDLK_ESCAPE  },  /* MENU         */
    { KEY_VOLUMEUP,     SDLK_EQUALS  },  /* Vol+         */
    { KEY_VOLUMEDOWN,   SDLK_MINUS   },  /* Vol-         */
    { 0, 0 },
};

static SDL_Keycode lookup_keycode(int linux_code) {
    for (int i = 0; s_keymap[i].linux_code; i++)
        if (s_keymap[i].linux_code == linux_code)
            return s_keymap[i].sdl_sym;
    return SDLK_UNKNOWN;
}

#define KEY_REPEAT_DELAY_MS  300
#define KEY_REPEAT_PERIOD_MS  80

static SDL_Keycode s_held_key    = SDLK_UNKNOWN;
static Uint32      s_held_since  = 0;
static Uint32      s_last_repeat = 0;

static int s_l2_pressed = 0;
static int s_r2_pressed = 0;

static int is_repeatable(SDL_Keycode sym) {
    return sym == SDLK_UP || sym == SDLK_DOWN ||
           sym == SDLK_LEFT || sym == SDLK_RIGHT;
}

static void push_key(SDL_Keycode sym, int down, int repeat) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type            = ev.type;
    ev.key.state           = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.repeat          = repeat;
    ev.key.keysym.sym      = sym;
    ev.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
    ev.key.keysym.mod      = KMOD_NONE;
    SDL_PushEvent(&ev);
}

static void handle_hat(int axis, int value) {
    SDL_Keycode neg = (axis == ABS_HAT0X) ? SDLK_LEFT  : SDLK_UP;
    SDL_Keycode pos = (axis == ABS_HAT0X) ? SDLK_RIGHT : SDLK_DOWN;

    if (value < 0) {
        push_key(neg, 1, 0);
        if (is_repeatable(neg)) {
            s_held_key    = neg;
            s_held_since  = SDL_GetTicks();
            s_last_repeat = s_held_since;
        }
    } else if (value > 0) {
        push_key(pos, 1, 0);
        if (is_repeatable(pos)) {
            s_held_key    = pos;
            s_held_since  = SDL_GetTicks();
            s_last_repeat = s_held_since;
        }
    } else {
        if (s_held_key == neg || s_held_key == pos) {
            push_key(s_held_key, 0, 0);
            s_held_key = SDLK_UNKNOWN;
        }
    }
}

static void handle_trigger(int axis, int value) {
    int *pressed = (axis == ABS_Z) ? &s_l2_pressed : &s_r2_pressed;
    SDL_Keycode sym = (axis == ABS_Z) ? SDLK_COMMA : SDLK_PERIOD;
    int now_pressed = (value > TRIGGER_THRESHOLD);
    if (now_pressed && !*pressed) {
        push_key(sym, 1, 0);
        *pressed = 1;
    } else if (!now_pressed && *pressed) {
        push_key(sym, 0, 0);
        *pressed = 0;
    }
}

void brick_poll_events(void) {
    if (s_input_fd < 0) return;

    struct input_event ev;
    while (read(s_input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
            if (ev.value != 0 && ev.value != 1) continue;
            SDL_Keycode sym = lookup_keycode(ev.code);
            if (sym == SDLK_UNKNOWN) continue;

            push_key(sym, ev.value, 0);
            if (ev.value == 1 && is_repeatable(sym)) {
                s_held_key    = sym;
                s_held_since  = SDL_GetTicks();
                s_last_repeat = s_held_since;
            } else if (ev.value == 0 && s_held_key == sym) {
                s_held_key = SDLK_UNKNOWN;
            }

        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)
                handle_hat(ev.code, ev.value);
            else if (ev.code == ABS_Z || ev.code == ABS_RZ)
                handle_trigger(ev.code, ev.value);
        }
    }

    if (s_held_key != SDLK_UNKNOWN) {
        Uint32 now = SDL_GetTicks();
        if (now - s_held_since >= KEY_REPEAT_DELAY_MS &&
            now - s_last_repeat >= KEY_REPEAT_PERIOD_MS) {
            push_key(s_held_key, 1, 1);
            s_last_repeat = now;
        }
    }
}

/* -------------------------------------------------------------------------
 * brick_screen_close
 * ---------------------------------------------------------------------- */

void brick_screen_close(void) {
    if (s_input_fd >= 0) { close(s_input_fd); s_input_fd = -1; }
    if (s_use_drm) {
        for (int b = 0; b < 2; b++)
            if (s_drm_buf[b]) memset(s_drm_buf[b], 0, s_drm_buf_size);
        flip_drm_close();
        s_use_drm = 0;
    } else {
        if (s_fb_mem && s_fb_mem != MAP_FAILED) {
            memset(s_fb_mem, 0, s_fb_size);
            munmap(s_fb_mem, s_fb_size);
            s_fb_mem = NULL;
        }
        if (s_fb_fd >= 0) { close(s_fb_fd); s_fb_fd = -1; }
    }
}

#endif /* SB_TRIMUI_BRICK */
