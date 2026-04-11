#!/bin/sh
# Host-side packaging script.
# Assembles the SpruceOS app package from:
#   build/storyboy32       — compiled binary (from build_inside_docker.sh)
#   build/libs32/     — collected .so files (from build_inside_docker.sh)
#   storyboy_base/         — SpruceOS infrastructure (launch.sh, config.json)
#   resources/        — fonts, default cover
#
# Usage: ./package_gvu_a30.sh [version]
set -e

VERSION=${1:-test}
SCRIPT_DIR=$(dirname "$0")
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BASE_DIR="$SCRIPT_DIR/storyboy_base"
BUILD="$REPO_ROOT/build"

if [ ! -f "$BUILD/storyboy32" ]; then
    echo "ERROR: $BUILD/storyboy32 not found. Run 'make miyoo-a30-docker' first."
    exit 1
fi

STAGE=$(mktemp -d)
trap "rm -rf $STAGE" EXIT

APP="$STAGE/spruce_gvu_pkg/App/StoryBoy"
mkdir -p "$APP/libs32"
mkdir -p "$APP/resources/fonts"

# SpruceOS infrastructure
cp -v "$BASE_DIR/launch.sh"   "$APP/"
cp -v "$BASE_DIR/config.json" "$APP/"
chmod +x "$APP/launch.sh"

# Icon — use a placeholder if none exists yet
if [ -f "$BASE_DIR/icon.png" ]; then
    cp -v "$BASE_DIR/icon.png" "$APP/"
fi

# Binary
cp -v "$BUILD/storyboy32" "$APP/"
chmod +x "$APP/storyboy32"

# Bundled .so files — libz.so.1 is committed in storyboy_base/ so packaging works
# without a fresh Docker build. build/libs32/ is used if present (may have
# additional libs from a fresh build_inside_docker.sh run).
cp -v "$BASE_DIR/libz.so.1" "$APP/libs32/"
if [ -d "$BUILD/libs32" ]; then
    cp -v "$BUILD/libs32/"* "$APP/libs32/" 2>/dev/null || true
fi

# Resources
cp -v "$REPO_ROOT/resources/fonts/DejaVuSans.ttf"  "$APP/resources/fonts/"
cp -v "$REPO_ROOT/resources/default_cover.png"      "$APP/resources/"
cp -v "$REPO_ROOT/resources/default_cover.svg"      "$APP/resources/"
cp -v "$REPO_ROOT/resources/app_icon.svg"           "$APP/resources/"
cp -v "$REPO_ROOT/resources/scrape_covers.sh"           "$APP/resources/"
cp -v "$REPO_ROOT/resources/clear_covers.sh"            "$APP/resources/"
cp -v "$REPO_ROOT/resources/fetch_subtitles.py"         "$APP/resources/"
cp -v "$REPO_ROOT/resources/clear_subtitle_pref.sh"     "$APP/resources/"
chmod +x "$APP/resources/scrape_covers.sh" \
         "$APP/resources/clear_covers.sh" \
         "$APP/resources/clear_subtitle_pref.sh"

# storyboy.conf — written fresh each package so it never lives in the repo.
# API keys are read from .<name>_key files in the repo root (gitignored).
# Users who build from source without key files get reduced functionality.
printf 'theme = SPRUCE\n' > "$APP/storyboy.conf"
TMDB_KEY_FILE="$REPO_ROOT/.books_api_key"
if [ -f "$TMDB_KEY_FILE" ]; then
    KEY=$(tr -d '[:space:]' < "$TMDB_KEY_FILE")
    if [ -n "$KEY" ]; then
        printf 'books_api_key = %s\n' "$KEY" >> "$APP/storyboy.conf"
        echo "TMDB key: included from .books_api_key"
    fi
else
    echo "TMDB key: not found (.books_api_key missing) — TVMaze-only package"
fi
SUBDL_KEY_FILE="$REPO_ROOT/.openlib_api_key"
if [ -f "$SUBDL_KEY_FILE" ]; then
    KEY=$(tr -d '[:space:]' < "$SUBDL_KEY_FILE")
    if [ -n "$KEY" ]; then
        printf 'openlib_api_key = %s\n' "$KEY" >> "$APP/storyboy.conf"
        echo "SubDL key: included from .openlib_api_key"
    fi
else
    echo "SubDL key: not found (.openlib_api_key missing) — Podnapisi-only subtitle search"
fi

OUTFILE="$REPO_ROOT/build/storyboy_spruce_a30_v${VERSION}.zip"
(cd "$STAGE" && zip -r "$OUTFILE" spruce_gvu_pkg)
echo ""
echo "Package: $OUTFILE"
echo "Contents:"
unzip -l "$OUTFILE" | tail -n +4 | head -40
