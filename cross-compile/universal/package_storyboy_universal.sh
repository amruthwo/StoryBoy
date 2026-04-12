#!/bin/sh
# package_storyboy_universal.sh — assemble a universal StoryBoy SpruceOS zip
#
# Usage:  sh cross-compile/universal/package_storyboy_universal.sh [VERSION]
# Output: storyboy_spruce_universal_v<VERSION>.zip  (in the project root)
#
# Requires: build/storyboy32, build/storyboy64, build/fetch_cover32, build/fetch_cover64,
#           build/extract_cover32, build/extract_cover64
#           build/libs32/, build/libs32_a30/, build/libs64/
#           cross-compile/universal/launch.sh, config.json
#           resources/ directory

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

VERSION="${1:-dev}"
# Strip a leading 'v' from the version so we don't double it when the
# caller passes a git tag like "v0.2.2" (result: storyboy_spruce_universal_v0.2.2.zip)
VERSION="${VERSION#v}"
OUT_ZIP="$REPO_ROOT/storyboy_spruce_universal_v${VERSION}.zip"
STAGE="$REPO_ROOT/build/universal-stage"
APP="$STAGE/App/StoryBoy"

echo "=== Packaging StoryBoy universal ${VERSION} ==="

# Clean and recreate staging area
# Zip will contain App/StoryBoy/... so users can extract directly to SD card root
rm -rf "$STAGE"
mkdir -p "$APP/bin32"
mkdir -p "$APP/bin64"
mkdir -p "$APP/lib32"
mkdir -p "$APP/lib32_a30"
mkdir -p "$APP/lib64"
mkdir -p "$APP/resources"

# Binaries
cp "$REPO_ROOT/build/storyboy32"       "$APP/bin32/storyboy"
cp "$REPO_ROOT/build/storyboy64"       "$APP/bin64/storyboy"
cp "$REPO_ROOT/build/fetch_cover32"    "$APP/bin32/fetch_cover"
cp "$REPO_ROOT/build/fetch_cover64"    "$APP/bin64/fetch_cover"
cp "$REPO_ROOT/build/extract_cover32"  "$APP/bin32/extract_cover"
cp "$REPO_ROOT/build/extract_cover64"  "$APP/bin64/extract_cover"
chmod +x "$APP/bin32/storyboy" "$APP/bin64/storyboy" \
         "$APP/bin32/fetch_cover" "$APP/bin64/fetch_cover" \
         "$APP/bin32/extract_cover" "$APP/bin64/extract_cover"

# Shared libraries
# lib32/     — unpatched SDL2 for Mini Flip/V4 (glibc 2.28+)
# lib32_a30/ — VERNEED-patched SDL2 for A30 (glibc 2.23); launch.sh prepends this for PLATFORM=A30
# lib64/     — aarch64 libs for Brick/Flip/Smart Pro
if [ -d "$REPO_ROOT/build/libs32" ]; then
    cp "$REPO_ROOT/build/libs32/"*.so* "$APP/lib32/" 2>/dev/null || true
fi
if [ -d "$REPO_ROOT/build/libs32_a30" ]; then
    cp "$REPO_ROOT/build/libs32_a30/"*.so* "$APP/lib32_a30/" 2>/dev/null || true
fi
if [ -d "$REPO_ROOT/build/libs64" ]; then
    cp "$REPO_ROOT/build/libs64/"*.so* "$APP/lib64/" 2>/dev/null || true
fi

# Launch script and config
cp "$SCRIPT_DIR/launch.sh"   "$APP/launch.sh"
cp "$SCRIPT_DIR/config.json" "$APP/config.json"
chmod +x "$APP/launch.sh"

# Icon
cp "$REPO_ROOT/resources/icon.png" "$APP/"

# Resources
cp -r "$REPO_ROOT/resources/." "$APP/resources/"

# Create zip — contents: App/StoryBoy/...
cd "$STAGE"
rm -f "$OUT_ZIP"
zip -r "$OUT_ZIP" App/
echo "=== Created: $OUT_ZIP ==="
ls -lh "$OUT_ZIP"
