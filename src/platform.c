#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Platform g_platform = PLATFORM_UNKNOWN;
static int      g_detected  = 0;

Platform detect_platform(void) {
    if (g_detected) return g_platform;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        g_platform = PLATFORM_UNKNOWN;
        g_detected = 1;
        return g_platform;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Miyoo A30 — Allwinner H700 */
        if (strstr(line, "sun8i")) {
            g_platform = PLATFORM_A30;
            break;
        }
        /* Miyoo Flip V1/V2, GKD Pixel2 — RK3566 */
        if (strstr(line, "0xd05")) {
            /* Both report 0xd05; treat as FLIP for now.
               If Pixel2 needs separate handling, distinguish via /proc/cpuinfo model name. */
            g_platform = PLATFORM_FLIP;
            break;
        }
        /* TrimUI Brick / Hammer */
        if (strstr(line, "TG3040")) {
            g_platform = PLATFORM_BRICK;
            break;
        }
        /* TrimUI Smart Pro */
        if (strstr(line, "TG5040")) {
            g_platform = PLATFORM_SMART_PRO;
            break;
        }
        /* TrimUI Smart Pro S */
        if (strstr(line, "TG5050")) {
            g_platform = PLATFORM_SMART_PRO_S;
            break;
        }
        /* Miyoo Mini / Mini Flip — fallback; appears in Hardware field */
        if (strstr(line, "Allwinner") || strstr(line, "V3s")) {
            g_platform = PLATFORM_MIYOO_MINI;
            break;
        }
    }

    fclose(f);
    g_detected = 1;
    return g_platform;
}

Platform get_platform(void) {
    return detect_platform();
}

const char *platform_name(Platform p) {
    switch (p) {
        case PLATFORM_A30:         return "Miyoo A30";
        case PLATFORM_FLIP:        return "Miyoo Flip / GKD Pixel2";
        case PLATFORM_MIYOO_MINI:  return "Miyoo Mini / Mini Flip";
        case PLATFORM_BRICK:       return "TrimUI Brick";
        case PLATFORM_SMART_PRO:   return "TrimUI Smart Pro";
        case PLATFORM_SMART_PRO_S: return "TrimUI Smart Pro S";
        default:                   return "Unknown";
    }
}

/* -------------------------------------------------------------------------
 * Runtime platform globals
 * ---------------------------------------------------------------------- */

int  g_display_w        = 640;
int  g_display_h        = 480;
int  g_display_rotation = 0;
int  g_panel_w          = 640;
int  g_panel_h          = 480;
char g_input_dev[256]   = "/dev/input/event3";
char g_python_bin[512]  = "";
char g_python_home[512] = "";
char g_battery_path[256]= "";
char g_app_dir[512]     = "";

void platform_init_from_env(void) {
    /* --- 1. Fast platform detection via SB_PLATFORM env var --- */
    const char *plat_str = getenv("SB_PLATFORM");
    if (plat_str && !g_detected) {
        if (!strcmp(plat_str, "Brick")) {
            g_platform = PLATFORM_BRICK;
        } else if (!strcmp(plat_str, "Flip")) {
            g_platform = PLATFORM_FLIP; /* Flip shares Brick HW but has no hw volume OSD */
        } else if (!strcmp(plat_str, "A30"))         g_platform = PLATFORM_A30;
        else if (!strcmp(plat_str, "SmartPro"))    g_platform = PLATFORM_SMART_PRO;
        else if (!strcmp(plat_str, "SmartProS"))   g_platform = PLATFORM_SMART_PRO_S;
        else if (!strcmp(plat_str, "MiyooMini"))   g_platform = PLATFORM_MIYOO_MINI;
        else if (!strcmp(plat_str, "AnbernicRG28XX")  ||
                 !strcmp(plat_str, "AnbernicRG34XXSP") ||
                 !strcmp(plat_str, "AnbernicXX640480") ||
                 !strcmp(plat_str, "AnbernicRGCubeXX"))
            g_platform = PLATFORM_UNKNOWN; /* Anbernic: detect by dims */
        if (g_platform != PLATFORM_UNKNOWN) g_detected = 1;
    }
    if (!g_detected) detect_platform();

    /* --- 2. Platform-specific display defaults --- */
    switch (g_platform) {
        case PLATFORM_BRICK:
            g_display_w = 1024; g_display_h = 768; g_display_rotation = 0;
            break;
        case PLATFORM_SMART_PRO:
        case PLATFORM_SMART_PRO_S:
            g_display_w = 1280; g_display_h = 720; g_display_rotation = 0;
            break;
        case PLATFORM_FLIP:
            g_display_w = 1024; g_display_h = 768; g_display_rotation = 0;
            break;
        case PLATFORM_A30:
            g_display_w = 640; g_display_h = 480; g_display_rotation = 270;
            break;
        case PLATFORM_MIYOO_MINI:
        default:
            g_display_w = 640; g_display_h = 480; g_display_rotation = 0;
            break;
    }

    /* --- 3. Override with SpruceOS env vars if present --- */
    const char *w   = getenv("SB_DISPLAY_W");
    const char *h   = getenv("SB_DISPLAY_H");
    const char *rot = getenv("SB_DISPLAY_ROTATION");
    if (w   && atoi(w)   > 0) g_display_w        = atoi(w);
    if (h   && atoi(h)   > 0) g_display_h        = atoi(h);
    if (rot)                   g_display_rotation = atoi(rot);

    /* --- 4. Derive physical fb0 dimensions from rotation --- */
    if (g_display_rotation == 90 || g_display_rotation == 270) {
        /* fb0 is portrait/transposed: swap logical w/h */
        g_panel_w = g_display_h;
        g_panel_h = g_display_w;
    } else {
        /* 0° and 180°: panel dimensions match the logical canvas */
        g_panel_w = g_display_w;
        g_panel_h = g_display_h;
    }

    /* --- 5. Input device --- */
    const char *inp = getenv("SB_INPUT_DEV");
    if (inp && inp[0])
        snprintf(g_input_dev, sizeof(g_input_dev), "%s", inp);
    /* else keep default /dev/input/event3 */

    /* --- 6. Battery path --- */
    const char *bat = getenv("SB_BATTERY_PATH");
    if (bat && bat[0])
        snprintf(g_battery_path, sizeof(g_battery_path), "%s", bat);

    /* --- 7. Python binary --- */
    const char *py = getenv("SB_PYTHON");
    if (py && py[0]) {
        snprintf(g_python_bin, sizeof(g_python_bin), "%s", py);
    } else {
        /* Platform-specific fallbacks */
        switch (g_platform) {
            case PLATFORM_BRICK:
            case PLATFORM_SMART_PRO:
            case PLATFORM_SMART_PRO_S:
            case PLATFORM_FLIP:
                snprintf(g_python_bin, sizeof(g_python_bin),
                         "/mnt/SDCARD/spruce/flip/bin/python3");
                break;
            default:
                snprintf(g_python_bin, sizeof(g_python_bin),
                         "/mnt/SDCARD/spruce/bin/python/bin/python3.10");
                break;
        }
    }

    /* --- 8. Derive PYTHONHOME as dirname(dirname(g_python_bin)) --- */
    if (g_python_bin[0]) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", g_python_bin);
        char *sl = strrchr(tmp, '/');
        if (sl) { *sl = '\0'; sl = strrchr(tmp, '/'); if (sl) *sl = '\0'; }
        snprintf(g_python_home, sizeof(g_python_home), "%s", tmp);
    }

    /* --- 9. App directory: dirname(dirname(/proc/self/exe)) --- */
    /* e.g. /mnt/SDCARD/App/GVU/bin32/gvu → /mnt/SDCARD/App/GVU */
    {
        char exe_path[512] = {0};
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
            char *sl = strrchr(exe_path, '/');   /* strip binary name */
            if (sl) { *sl = '\0';
            sl = strrchr(exe_path, '/');          /* strip bin32 / bin64 */
            if (sl) *sl = '\0'; }
            snprintf(g_app_dir, sizeof(g_app_dir), "%s", exe_path);
        }
        if (!g_app_dir[0]) {
            /* Fallback to CWD (launch.sh does cd "$APPDIR") */
            char cwd[512];
            if (getcwd(cwd, sizeof(cwd)))
                snprintf(g_app_dir, sizeof(g_app_dir), "%s", cwd);
        }
    }

    fprintf(stderr, "platform_init: %s  canvas=%dx%d  rot=%d  panel=%dx%d\n",
            platform_name(g_platform),
            g_display_w, g_display_h, g_display_rotation,
            g_panel_w, g_panel_h);
    fprintf(stderr, "  input=%s  python=%s  app_dir=%s\n",
            g_input_dev, g_python_bin, g_app_dir);
}
