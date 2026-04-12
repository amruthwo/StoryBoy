#!/bin/sh
# package_storyboy_onion.sh — assemble a StoryBoy OnionOS zip
#
# Usage:  sh cross-compile/onion/package_storyboy_onion.sh [VERSION]
# Output: storyboy_onion_v<VERSION>.zip  (in the project root)
#
# Requires: build/storyboy_onion32, build/fetch_cover32, build/extract_cover32
#           build/libs32/, build/libs32_a30/
#           cross-compile/onion/launch.sh, config.json
#           resources/ directory

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

VERSION="${1:-dev}"
# Strip a leading 'v' so a git tag like "v0.1.2" yields storyboy_onion_v0.1.2.zip
VERSION="${VERSION#v}"
OUT_ZIP="$REPO_ROOT/storyboy_onion_v${VERSION}.zip"
STAGE="$REPO_ROOT/build/onion-stage"
APP="$STAGE/App/StoryBoy"

echo "=== Packaging StoryBoy OnionOS ${VERSION} ==="

rm -rf "$STAGE"
mkdir -p "$APP/bin32"
mkdir -p "$APP/lib32"
mkdir -p "$APP/lib32_a30"
mkdir -p "$APP/resources"

# Binaries
cp "$REPO_ROOT/build/storyboy_onion32"  "$APP/bin32/storyboy"
cp "$REPO_ROOT/build/fetch_cover32"     "$APP/bin32/fetch_cover"
cp "$REPO_ROOT/build/extract_cover32"   "$APP/bin32/extract_cover"
chmod +x "$APP/bin32/storyboy" "$APP/bin32/fetch_cover" "$APP/bin32/extract_cover"

# Shared libraries
# lib32/     — unpatched SDL2 (for reference / Mini Flip V4 if ever supported)
# lib32_a30/ — VERNEED-patched SDL2 for glibc 2.23 (OnionOS); launch.sh prepends this
if [ -d "$REPO_ROOT/build/libs32" ]; then
    cp "$REPO_ROOT/build/libs32/"*.so* "$APP/lib32/" 2>/dev/null || true
fi
if [ -d "$REPO_ROOT/build/libs32_a30" ]; then
    cp "$REPO_ROOT/build/libs32_a30/"*.so* "$APP/lib32_a30/" 2>/dev/null || true
fi

# Launch script and config
cp "$SCRIPT_DIR/launch.sh"   "$APP/launch.sh"
cp "$SCRIPT_DIR/config.json" "$APP/config.json"
chmod +x "$APP/launch.sh"

# Icon — OnionOS launcher expects a small icon; use the 74x74 variant
cp "$REPO_ROOT/resources/icon_onion.png" "$APP/icon.png"

# Resources
cp -r "$REPO_ROOT/resources/." "$APP/resources/"

# Create zip — contents: App/StoryBoy/...
cd "$STAGE"
rm -f "$OUT_ZIP"
zip -r "$OUT_ZIP" App/
echo "=== Created: $OUT_ZIP ==="
ls -lh "$OUT_ZIP"
