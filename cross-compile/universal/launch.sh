#!/bin/sh

APPDIR=/mnt/SDCARD/App/StoryBoy
LOG="$APPDIR/storyboy.log"

echo "launch.sh start" > "$LOG"

. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
echo "helperFunctions sourced, PLATFORM=$PLATFORM ARCH=$PLATFORM_ARCHITECTURE" >> "$LOG"

# Select binary and library directory based on CPU architecture.
if [ "$PLATFORM_ARCHITECTURE" = "aarch64" ]; then
    BIN="$APPDIR/bin64/storyboy"
    LIBDIR="$APPDIR/lib64"
    echo "using bin64" >> "$LOG"
else
    BIN="$APPDIR/bin32/storyboy"
    LIBDIR="$APPDIR/lib32"
    echo "using bin32" >> "$LOG"
fi

# Detect Miyoo Mini V4 (Mini Flip) vs V2/V3 early via fb0 resolution.
# SpruceOS uses a shared MiyooMini.cfg for all Mini variants (reports 640x480
# rot=0), so we distinguish V4 by its unique screen size.
MIYOO_V4=0
if [ "$PLATFORM" = "MiyooMini" ] && grep -q "752x560p" /sys/class/graphics/fb0/modes 2>/dev/null; then
    MIYOO_V4=1
    echo "MiyooMini V4 (Mini Flip) detected" >> "$LOG"
elif [ "$PLATFORM" = "MiyooMini" ]; then
    echo "MiyooMini V2/V3 detected" >> "$LOG"
fi

# A30 needs a GLIBC_2.23-compatible SDL2 (patched VERNEED); all other armhf
# devices (MiyooMini family) have glibc 2.28+ and use the unpatched build.
if [ "$PLATFORM" = "A30" ]; then
    export LD_LIBRARY_PATH="$APPDIR/lib32_a30:$LIBDIR:/usr/lib:$LD_LIBRARY_PATH"
else
    export LD_LIBRARY_PATH="$LIBDIR:/usr/lib:$LD_LIBRARY_PATH"
fi
export SDL_VIDEODRIVER=dummy

# On Miyoo Flip and Miyoo Mini family, audioserver or PyUI holds the default
# audio device. Override .asoundrc to use plughw:0,0 which opens hw:0,0
# directly, bypassing dmix and audioserver's OSS claim on /dev/dsp.
# On Brick and A30, force OSS (/dev/dsp) which is the working audio path.
export HOME=/tmp/storyboy_home
mkdir -p "$HOME"
if [ "$PLATFORM" = "Flip" ]; then
    # Miyoo Flip: PyUI holds hw:0,0 via ALSA exclusively. Use plughw:0,0.
    cat > "$HOME/.asoundrc" << 'ASOUND_EOF'
pcm.!default {
    type plug
    slave.pcm "hw:0,0"
}
ctl.!default {
    type hw
    card 0
}
ASOUND_EOF
elif [ "$PLATFORM" = "MiyooMini" ] && [ "$MIYOO_V4" = "1" ]; then
    # Miyoo Mini V4 (Mini Flip): libpadsp.so bridges SDL2's OSS calls to the
    # SigmaStar MI_AO proprietary audio HW. audioserver must stay running.
    export LD_PRELOAD="/customer/lib/libpadsp.so"
    export SDL_AUDIODRIVER=dsp
elif [ "$PLATFORM" = "MiyooMini" ]; then
    # V2/V3: mmiyoo audio is only in SpruceOS's SDL2, which lacks dummy video.
    # We can't use both simultaneously without building our own SDL2 with the
    # mmiyoo backend. Run silent — video works, no audio.
    echo "V2/V3 audio: silent (mmiyoo requires SpruceOS SDL2, incompatible with dummy video)" >> "$LOG"
    export SDL_AUDIODRIVER=dummy
else
    # Brick, A30, etc.: Force OSS (/dev/dsp).
    export SDL_AUDIODRIVER=dsp
fi

# Forward SpruceOS platform vars to GVU runtime.
export SB_PLATFORM="$PLATFORM"
export SB_DISPLAY_W="$DISPLAY_WIDTH"
export SB_DISPLAY_H="$DISPLAY_HEIGHT"
export SB_DISPLAY_ROTATION="$DISPLAY_ROTATION"

# All Miyoo Mini variants are physically mounted upside-down but SpruceOS
# MiyooMini.cfg reports rot=0. Override to 180° for all variants.
# V4 also gets the correct screen dimensions (752x560 vs reported 640x480).
if [ "$MIYOO_V4" = "1" ]; then
    export SB_DISPLAY_W=752
    export SB_DISPLAY_H=560
    export SB_DISPLAY_ROTATION=180
    echo "overriding display: 752x560 rot=180" >> "$LOG"
elif [ "$PLATFORM" = "MiyooMini" ]; then
    export SB_DISPLAY_ROTATION=180
    echo "overriding rotation: 640x480 rot=180" >> "$LOG"
fi

export SB_INPUT_DEV="$EVENT_PATH_READ_INPUTS_SPRUCE"

# MiyooMini battery: no standard sysfs node.
# V3/V4 (axp_test present): parse JSON output from axp_test.
# V2/og (no axp_test): use read_battery binary from SpruceOS miyoomini/bin.
# Both poll every 30s and write to a tmpfile GVU reads.
if [ "$PLATFORM" = "MiyooMini" ] && [ -x "/customer/app/axp_test" ]; then
    mkdir -p /tmp/storyboy_bat
    update_battery() {
        out=$(/customer/app/axp_test)
        printf '%s' "$out" | jq -r '.battery' > /tmp/storyboy_bat/capacity
        charging=$(printf '%s' "$out" | jq -r '.charging')
        if [ "$charging" = "1" ]; then echo "Charging"; else echo "Discharging"; fi > /tmp/storyboy_bat/status
    }
    update_battery
    ( while true; do sleep 30; update_battery; done ) &
    BATTERY_POLLER_PID=$!
    export SB_BATTERY_PATH="/tmp/storyboy_bat/capacity"
elif [ "$PLATFORM" = "MiyooMini" ] && [ -x "/mnt/SDCARD/spruce/miyoomini/bin/read_battery" ]; then
    # Original Mini / V2: read_battery outputs a plain percent integer.
    mkdir -p /tmp/storyboy_bat
    update_battery() {
        /mnt/SDCARD/spruce/miyoomini/bin/read_battery > /tmp/storyboy_bat/capacity 2>/dev/null || echo 0 > /tmp/storyboy_bat/capacity
        echo "Discharging" > /tmp/storyboy_bat/status
    }
    update_battery
    ( while true; do sleep 30; update_battery; done ) &
    BATTERY_POLLER_PID=$!
    export SB_BATTERY_PATH="/tmp/storyboy_bat/capacity"
else
    export SB_BATTERY_PATH="${BATTERY}/capacity"
fi

cd "$APPDIR"
echo "launching $BIN" >> "$LOG"

sleep 0.5

# Clear fb0 to black before StoryBoy starts so no stale content is visible.
dd if=/dev/zero of=/dev/fb0 2>/dev/null || true

"$BIN" >> "$LOG" 2>&1
echo "storyboy exited: $?" >> "$LOG"

# Clean up battery poller if started
if [ -n "$BATTERY_POLLER_PID" ]; then
    kill "$BATTERY_POLLER_PID" 2>/dev/null || true
fi

# If the sleep timer fired, StoryBoy writes a sentinel file asking us to
# suspend.  Delegate to SpruceOS's sleep_helper so its watchdog/wakelock
# machinery coordinates the suspend rather than us writing /sys/power/state
# directly (which caused a second, spurious sleep after wake).
SUSPEND_FLAG="/tmp/storyboy_suspend"
if [ -f "$SUSPEND_FLAG" ]; then
    rm -f "$SUSPEND_FLAG"
    echo "sleep sentinel found, suspending via sleep_helper" >> "$LOG"
    /mnt/SDCARD/spruce/scripts/sleep_helper.sh >> "$LOG" 2>&1 || true
fi
