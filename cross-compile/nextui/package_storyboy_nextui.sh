#!/bin/sh
# package_storyboy_nextui.sh — assemble a StoryBoy NextUI pak zip
#
# Usage:  sh cross-compile/nextui/package_storyboy_nextui.sh [VERSION]
# Output: storyboy_nextui_v<VERSION>.zip  (in the project root)
#
# Requires: build/storyboy64, build/extract_cover64
#           build/libs64/
#           cross-compile/nextui/launch.sh, pak.json
#           cross-compile/universal/config.json
#           resources/ directory

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

VERSION="${1:-dev}"
VERSION="${VERSION#v}"
OUT_ZIP="$REPO_ROOT/storyboy_nextui_v${VERSION}.zip"
STAGE="$REPO_ROOT/build/nextui-stage"
PAK="$STAGE/Tools/tg5040/StoryBoy.pak"

echo "=== Packaging StoryBoy NextUI ${VERSION} ==="

rm -rf "$STAGE"
mkdir -p "$PAK/bin64"
mkdir -p "$PAK/lib64"
mkdir -p "$PAK/resources"

# Main binary
cp "$REPO_ROOT/build/storyboy64"       "$PAK/bin64/storyboy"
chmod +x "$PAK/bin64/storyboy"

# Cover extractor (background process on first library scan)
cp "$REPO_ROOT/build/extract_cover64"  "$PAK/bin64/extract_cover"
# Cover fetcher (Open Library HTTP fetch, triggered by Y button)
cp "$REPO_ROOT/build/fetch_cover64"    "$PAK/bin64/fetch_cover"
chmod +x "$PAK/bin64/extract_cover" "$PAK/bin64/fetch_cover"

# Libraries
if [ -d "$REPO_ROOT/build/libs64" ]; then
    cp "$REPO_ROOT/build/libs64/"*.so* "$PAK/lib64/" 2>/dev/null || true
fi

# NextUI-specific launch script, pak metadata, and default config
cp "$SCRIPT_DIR/launch.sh"      "$PAK/launch.sh"
cp "$SCRIPT_DIR/pak.json"       "$PAK/pak.json"
cp "$SCRIPT_DIR/storyboy.conf"  "$PAK/storyboy.conf"
chmod +x "$PAK/launch.sh"

# Config (shared with SpruceOS universal build)
cp "$REPO_ROOT/cross-compile/universal/config.json" "$PAK/config.json"

# Icon and resources
cp "$REPO_ROOT/resources/icon.png" "$PAK/"
cp -r "$REPO_ROOT/resources/." "$PAK/resources/"

# Create zip — contents: Tools/tg5040/StoryBoy.pak/...
cd "$STAGE"
rm -f "$OUT_ZIP"
zip -r "$OUT_ZIP" Tools/
echo "=== Created: $OUT_ZIP ==="
ls -lh "$OUT_ZIP"
