#!/bin/sh
# StoryBoy launcher for NextUI (tg5040 / tg5050)
# Supports: TrimUI Brick, Smart Pro, Smart Pro S
# Platform and display geometry are auto-detected by the binary from /proc/cpuinfo.

PAK_DIR="$(dirname "$0")"
cd "$PAK_DIR" || exit 1

LOG="${LOGS_PATH:-/tmp}/StoryBoy.txt"
exec >>"$LOG" 2>&1
echo "=== StoryBoy launch.sh start ==="

BIN="$PAK_DIR/bin64/storyboy"
LIBDIR="$PAK_DIR/lib64"

export LD_LIBRARY_PATH="$LIBDIR:/usr/lib:$LD_LIBRARY_PATH"

# HOME dir for .asoundrc and SDL prefs
export HOME=/tmp/storyboy_home
mkdir -p "$HOME"

# Audio: OSS (/dev/dsp) confirmed present on NextUI Brick — matches SpruceOS behaviour.
export SDL_AUDIODRIVER=dsp

# Use NextUI's system font (Rounded Mplus 1c Bold) if present; falls back to
# bundled DejaVuSans automatically if the path doesn't exist.
export SB_FONT_PATH="/mnt/SDCARD/.system/res/font1.ttf"

# Battery: scan sysfs for a Battery-type power supply
SB_BATTERY_PATH=""
for bat_dir in /sys/class/power_supply/*/; do
    if [ -f "${bat_dir}capacity" ]; then
        bat_type=$(cat "${bat_dir}type" 2>/dev/null)
        if [ "$bat_type" = "Battery" ]; then
            SB_BATTERY_PATH="${bat_dir}capacity"
            break
        fi
    fi
done
export SB_BATTERY_PATH
echo "battery: ${SB_BATTERY_PATH:-not found}"

# Keep the screen on while StoryBoy is running
echo "1" > /tmp/stay_awake

echo "launching $BIN"
"$BIN"
EXIT_CODE=$?
echo "storyboy exited: $EXIT_CODE"

rm -f /tmp/stay_awake

# Sleep timer: if StoryBoy wrote the suspend sentinel, honour it
if [ -f /tmp/storyboy_suspend ]; then
    rm -f /tmp/storyboy_suspend
    echo "sleep sentinel found, suspending"
    echo mem > /sys/power/state 2>/dev/null || true
fi
