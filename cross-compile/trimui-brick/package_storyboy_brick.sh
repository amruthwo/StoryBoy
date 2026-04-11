#!/bin/sh
# Host-side packaging script for Trimui Brick.
# Assembles the SpruceOS app package from:
#   build/storyboy64          — main binary (from build_inside_docker.sh)
#   build/fetch_cover64       — cover fetch helper (from build_inside_docker.sh)
#   build/libs64/             — collected .so files (from build_inside_docker.sh)
#   cross-compile/universal/  — launch.sh, config.json
#   resources/                — fonts, default cover, icon
#
# Usage: sh cross-compile/trimui-brick/package_storyboy_brick.sh [version]
set -e

VERSION=${1:-test}
SCRIPT_DIR=$(dirname "$0")
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
UNIVERSAL="$REPO_ROOT/cross-compile/universal"
BUILD="$REPO_ROOT/build"

for f in storyboy64 fetch_cover64; do
    if [ ! -f "$BUILD/$f" ]; then
        echo "ERROR: $BUILD/$f not found. Run 'sh cross-compile/trimui-brick/build_inside_docker.sh' first."
        exit 1
    fi
done

STAGE=$(mktemp -d)
trap "rm -rf $STAGE" EXIT

APP="$STAGE/App/StoryBoy"
mkdir -p "$APP/bin64"
mkdir -p "$APP/lib64"
mkdir -p "$APP/resources/fonts"

# SpruceOS infrastructure
cp "$UNIVERSAL/launch.sh"   "$APP/"
cp "$UNIVERSAL/config.json" "$APP/"
chmod +x "$APP/launch.sh"

# Icon
cp "$REPO_ROOT/resources/icon.png" "$APP/"

# Binaries
cp "$BUILD/storyboy64"    "$APP/bin64/storyboy"
cp "$BUILD/fetch_cover64" "$APP/bin64/fetch_cover"
chmod +x "$APP/bin64/storyboy" "$APP/bin64/fetch_cover"

# Bundled shared libraries
if [ -d "$BUILD/libs64" ]; then
    cp "$BUILD/libs64/"* "$APP/lib64/" 2>/dev/null || true
fi

# Resources
cp "$REPO_ROOT/resources/fonts/DejaVuSans.ttf" "$APP/resources/fonts/"
cp "$REPO_ROOT/resources/default_cover.png"     "$APP/resources/"
cp "$REPO_ROOT/resources/default_cover.svg"     "$APP/resources/"

OUTFILE="$REPO_ROOT/build/storyboy_spruce_brick_v${VERSION}.zip"
(cd "$STAGE" && zip -r "$OUTFILE" App/)
echo ""
echo "Package: $OUTFILE"
unzip -l "$OUTFILE" | tail -n +4 | head -40
