#!/bin/sh
# Runs INSIDE the Docker container.
# Compiles storyboy64 and collects all .so dependencies into /storyboy/build/libs64/
set -e

AARCH64_LIB=/usr/lib/aarch64-linux-gnu
BUILD=/storyboy/build

mkdir -p "$BUILD/libs64"

echo "=== Compiling storyboy64 ==="
make -C /storyboy trimui-brick-build
cp /storyboy/storyboy64 "$BUILD/storyboy64"

echo "=== Compiling fetch_cover64 ==="
aarch64-linux-gnu-gcc -Wall -std=c11 -O2 -D_POSIX_C_SOURCE=200809L \
    -march=armv8-a \
    $(pkg-config --cflags libcurl) \
    -o "$BUILD/fetch_cover64" /storyboy/src/fetch_cover.c \
    $(pkg-config --static --libs libcurl) \
    -lz -lm -static-libgcc
echo "Built: fetch_cover64"

echo "=== Collecting shared library dependencies ==="
# Skip libs the Brick already provides natively.
SKIP="libc.so libm.so libpthread.so libdl.so librt.so libgcc_s.so ld-linux"

collect_libs() {
    BINARY="$1"
    aarch64-linux-gnu-objdump -p "$BINARY" 2>/dev/null \
        | awk '/NEEDED/{print $2}'
}

copy_lib() {
    SONAME="$1"
    for skip in $SKIP; do
        case "$SONAME" in *$skip*) return ;; esac
    done
    [ -f "$BUILD/libs64/$SONAME" ] && return
    REAL=$(readlink -f "$AARCH64_LIB/$SONAME" 2>/dev/null)
    if [ -z "$REAL" ] || [ ! -f "$REAL" ]; then
        BASE=$(echo "$SONAME" | sed 's/\.so\..*/\.so/')
        REAL=$(readlink -f "$AARCH64_LIB/$BASE" 2>/dev/null)
    fi
    if [ -z "$REAL" ] || [ ! -f "$REAL" ]; then
        echo "  WARNING: $SONAME not found in $AARCH64_LIB"
        return
    fi
    echo "  $SONAME -> $(basename $REAL)"
    cp "$REAL" "$BUILD/libs64/$SONAME"
    for dep in $(collect_libs "$BUILD/libs64/$SONAME"); do
        copy_lib "$dep"
    done
}

for soname in $(collect_libs "$BUILD/storyboy64"); do
    copy_lib "$soname"
done

echo "=== libs64 contents ==="
ls -lh "$BUILD/libs64/"
echo "=== Done ==="
