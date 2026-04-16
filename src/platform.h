#pragma once

typedef enum {
    PLATFORM_A30,
    PLATFORM_FLIP,
    PLATFORM_MIYOO_MINI,
    PLATFORM_BRICK,
    PLATFORM_SMART_PRO,
    PLATFORM_SMART_PRO_S,
    PLATFORM_UNKNOWN
} Platform;

Platform detect_platform(void);
Platform get_platform(void);
const char *platform_name(Platform p);

/* -------------------------------------------------------------------------
 * Runtime platform globals — set by platform_init_from_env() at startup.
 *
 * g_display_w/h      : logical canvas size (SpruceOS DISPLAY_WIDTH/HEIGHT).
 *                      Always the landscape orientation — e.g. 640×480 on A30.
 * g_display_rotation : panel rotation relative to the logical canvas.
 *                      0   = fb0 is landscape (Brick, Flip, Mini, Smart Pro)
 *                      270 = fb0 is portrait / transposed (A30, Anbernic RG28XX)
 * g_panel_w/h        : physical fb0 dimensions.
 *                      rotation=0   → same as g_display_w / g_display_h
 *                      rotation=270 → transposed (g_display_h / g_display_w)
 * g_input_dev        : evdev path  (SpruceOS EVENT_PATH_READ_INPUTS_SPRUCE)
 * g_python_bin       : Python 3 binary (SpruceOS DEVICE_PYTHON3_PATH)
 * g_battery_path     : sysfs capacity path (SpruceOS $BATTERY + "/capacity")
 * ---------------------------------------------------------------------- */
extern int  g_display_w;
extern int  g_display_h;
extern int  g_display_rotation;
extern int  g_panel_w;
extern int  g_panel_h;
extern char g_input_dev[256];
extern char g_python_bin[512];
extern char g_python_home[512];
extern char g_battery_path[256];
extern char g_app_dir[512];     /* absolute path to app directory (dirname of dirname of exe) */

/* Call once at startup, before any screen_init(). */
void platform_init_from_env(void);
