#!/bin/sh

APPDIR=/mnt/SDCARD/App/StoryBoy
sysdir=/mnt/SDCARD/.tmp_update
LOG="$APPDIR/storyboy.log"

echo "launch.sh start" > "$LOG"

# CPU performance mode
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null

# -------------------------------------------------------------------------
# Device detection
# -------------------------------------------------------------------------
MIYOO_V4=0
MIYOO_PLUS=0

if grep -q "752x560p" /sys/class/graphics/fb0/modes 2>/dev/null; then
    MIYOO_V4=1
    echo "Device: MiyooMini V4 (Mini Flip)" >> "$LOG"
elif $sysdir/bin/axp 0 > /dev/null 2>&1; then
    MIYOO_PLUS=1
    echo "Device: MiyooMini Plus" >> "$LOG"
else
    echo "Device: MiyooMini V2/V3" >> "$LOG"
fi

# -------------------------------------------------------------------------
# Log available input devices — helps identify correct event node
# -------------------------------------------------------------------------
echo "--- input devices ---" >> "$LOG"
cat /proc/bus/input/devices 2>/dev/null >> "$LOG" || ls /dev/input/ >> "$LOG"
echo "---------------------" >> "$LOG"

# -------------------------------------------------------------------------
# Library path
# Use lib32_a30 (VERNEED-patched SDL2) — OnionOS ships an older glibc than
# SpruceOS on the same hardware; the unpatched lib32 SDL2 requires
# GLIBC_2.29 which is not available here.
# lib32_a30 must come FIRST — parasyte's SDL2 has no ALSA support.
# -------------------------------------------------------------------------
export LD_LIBRARY_PATH=$APPDIR/lib32_a30:$sysdir/lib/parasyte:$LD_LIBRARY_PATH

# -------------------------------------------------------------------------
# Audio
# -------------------------------------------------------------------------
export HOME=/tmp/storyboy_home
mkdir -p "$HOME"

if [ "$MIYOO_V4" = "1" ]; then
    # V4 (Mini Flip): libpadsp bridges SDL's OSS calls to MI_AO.
    # audioserver must stay running.
    export LD_PRELOAD=/miyoo/lib/libpadsp.so
    export SDL_AUDIODRIVER=dsp
    echo "Audio: libpadsp + dsp (V4)" >> "$LOG"
else
    # V2/V3 and Plus: kill audioserver and use ALSA directly.
    # stop_audioserver.sh saves/restores volume via MI_AO.
    . $sysdir/script/stop_audioserver.sh
    export SDL_AUDIODRIVER=alsa
    export AUDIODEV=hw:0,0
    echo "Audio: ALSA hw:0,0 (audioserver stopped)" >> "$LOG"
fi

export SDL_VIDEODRIVER=dummy

# -------------------------------------------------------------------------
# Display and platform
# -------------------------------------------------------------------------
export SB_PLATFORM=MiyooMini

if [ "$MIYOO_V4" = "1" ]; then
    export SB_DISPLAY_W=752
    export SB_DISPLAY_H=560
    export SB_DISPLAY_ROTATION=180
else
    export SB_DISPLAY_W=640
    export SB_DISPLAY_H=480
    export SB_DISPLAY_ROTATION=180
fi

echo "Display: ${SB_DISPLAY_W}x${SB_DISPLAY_H} rot=${SB_DISPLAY_ROTATION}" >> "$LOG"

# -------------------------------------------------------------------------
# Input — event0 is our best guess; check the input device log above
# if buttons are not working and adjust accordingly
# -------------------------------------------------------------------------
export SB_INPUT_DEV=/dev/input/event0
echo "SB_INPUT_DEV=$SB_INPUT_DEV" >> "$LOG"

# -------------------------------------------------------------------------
# Battery — search class path, known device paths, then find as last resort
# -------------------------------------------------------------------------
echo "--- power supply ---" >> "$LOG"

# OnionOS: batmon daemon writes battery % to /tmp/percBat (read from axp_test)
if [ -f /tmp/percBat ]; then
    export SB_BATTERY_PATH=/tmp/percBat
    echo "  batmon: /tmp/percBat = $(cat /tmp/percBat)" >> "$LOG"
fi

# Fallback: standard sysfs paths
if [ -z "$SB_BATTERY_PATH" ]; then
    for ps in \
        /sys/class/power_supply/battery/capacity \
        /sys/class/power_supply/axp20x-battery/capacity \
        /sys/class/power_supply/axp2202-battery/capacity; do
        if [ -f "$ps" ]; then
            export SB_BATTERY_PATH="$ps"
            echo "  sysfs: $ps = $(cat $ps)" >> "$LOG"
            break
        fi
    done
fi

echo "SB_BATTERY_PATH=${SB_BATTERY_PATH:-not found}" >> "$LOG"
echo "--------------------" >> "$LOG"

# -------------------------------------------------------------------------
# V2/V3 swap — same 32MB swap as SpruceOS build; allows FFmpeg to parse
# large M4B stsz tables that exceed available RAM
# -------------------------------------------------------------------------
if [ "$MIYOO_V4" = "0" ] && [ "$MIYOO_PLUS" = "0" ]; then
    SWAP_FILE="$APPDIR/storyboy.swap"
    if [ ! -f "$SWAP_FILE" ]; then
        echo "Creating 32MB swap file..." >> "$LOG"
        dd if=/dev/zero of="$SWAP_FILE" bs=1M count=32 2>/dev/null
        chmod 600 "$SWAP_FILE"
        mkswap "$SWAP_FILE" >> "$LOG" 2>&1
    fi
    swapon "$SWAP_FILE" 2>/dev/null && echo "Swap enabled" >> "$LOG"
fi

# -------------------------------------------------------------------------
# Launch
# -------------------------------------------------------------------------
dd if=/dev/zero of=/dev/fb0 2>/dev/null || true

cd "$APPDIR"
echo "Launching bin32/storyboy" >> "$LOG"
./bin32/storyboy >> "$LOG" 2>&1
echo "storyboy exited: $?" >> "$LOG"

# -------------------------------------------------------------------------
# Cleanup
# -------------------------------------------------------------------------
if [ "$MIYOO_V4" = "0" ] && [ "$MIYOO_PLUS" = "0" ]; then
    swapoff "$APPDIR/storyboy.swap" 2>/dev/null
fi
