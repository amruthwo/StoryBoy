CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
          $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf) \
          $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample libavfilter)
LDFLAGS = $(shell pkg-config --libs sdl2 SDL2_image SDL2_ttf) \
          $(shell pkg-config --libs libavformat libavcodec libavutil libswresample libavfilter) \
          -lm

SRC_DIR = src
SRCS    = $(SRC_DIR)/main.c \
          $(SRC_DIR)/platform.c \
          $(SRC_DIR)/filebrowser.c \
          $(SRC_DIR)/browser.c \
          $(SRC_DIR)/history.c \
          $(SRC_DIR)/hintbar.c \
          $(SRC_DIR)/overlay.c \
          $(SRC_DIR)/theme.c \
          $(SRC_DIR)/decoder.c \
          $(SRC_DIR)/audio.c \
          $(SRC_DIR)/player.c \
          $(SRC_DIR)/resume.c \
          $(SRC_DIR)/statusbar.c \
          $(SRC_DIR)/cover.c
TARGET  = storyboy

# -------------------------------------------------------------------------
# Miyoo A30 cross-compile settings
# Intended to run INSIDE the Docker container built from Dockerfile.storyboy.
# pkg-config env vars are set by the Dockerfile (PKG_CONFIG_PATH etc.).
# -------------------------------------------------------------------------
A30_CC      = arm-linux-gnueabihf-gcc
A30_CFLAGS  = -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L \
              -DSB_A30 \
              -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
              $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf \
                  libavformat libavcodec libavutil libswresample libavfilter)
# SDL2 linked DYNAMICALLY — libSDL2-2.0.so.0 ships in lib32/.
# On A30 our ALSA-backed SDL2 is loaded; on MiyooMini launch.sh prepends
# /customer/lib so SpruceOS's MI_AO SDL2 is loaded instead.
# SDL2_image, SDL2_ttf, FFmpeg, libavfilter remain fully static.
# storyboy32 NEEDED at runtime: libc, libm, libpthread, libdl, libSDL2-2.0.so.0
A30_LDFLAGS = -Wl,--start-group \
              $(shell pkg-config --static --libs sdl2 SDL2_image SDL2_ttf \
                  libavformat libavcodec libavutil libswresample libavfilter) \
              -Wl,--end-group \
              -Wl,--as-needed \
              -lm -lpthread -ldl -static-libgcc -static-libstdc++

A30_SRCS    = $(SRCS) $(SRC_DIR)/a30_screen.c $(SRC_DIR)/glibc_compat.c
A30_TARGET  = storyboy32

# OnionOS build — same toolchain/flags as A30 but adds -DSB_ONION which
# lifts the embedded-cover extraction restriction (swap available on OnionOS).
ONION_CFLAGS  = $(A30_CFLAGS) -DSB_ONION
ONION_SRCS    = $(A30_SRCS)
ONION_TARGET  = storyboy_onion32

SB_A30_IMAGE  ?= storyboy-a30
SB_A30_DEPLOY ?= spruce@192.168.1.62
SB_A30_PATH   ?= /mnt/SDCARD/App/StoryBoy

# -------------------------------------------------------------------------
# Trimui Brick / Miyoo Flip / Smart Pro cross-compile settings
# Intended to run INSIDE the Docker container built from Dockerfile.storyboy.
# No custom screen layer — plain SDL_Renderer on all 64-bit devices.
# -------------------------------------------------------------------------
BRICK_CC      = aarch64-linux-gnu-gcc
BRICK_CFLAGS  = -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L \
                -DSB_TRIMUI_BRICK \
                -march=armv8-a \
                $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf \
                    libavformat libavcodec libavutil libswresample libavfilter)
# Static linking — storyboy64 NEEDED: only libc, libm, libpthread, libdl, libasound.so.2
BRICK_LDFLAGS = -Wl,--start-group \
                $(shell pkg-config --static --libs sdl2 SDL2_image SDL2_ttf \
                    libavformat libavcodec libavutil libswresample libavfilter) \
                -Wl,--end-group \
                -Wl,--as-needed \
                -lm -lpthread -ldl -static-libgcc -static-libstdc++

BRICK_SRCS    = $(SRCS) $(SRC_DIR)/brick_screen.c
BRICK_TARGET  = storyboy64

SB_BRICK_IMAGE  ?= storyboy-brick
SB_BRICK_DEPLOY ?= spruce@192.168.1.45
SB_BRICK_PATH   ?= /mnt/SDCARD/App/StoryBoy

.PHONY: all clean test miyoo-a30-build onion-build miyoo-a30-docker miyoo-a30-package miyoo-a30-deploy \
        trimui-brick-build trimui-brick-docker trimui-brick-package trimui-brick-deploy \
        fetch-cover-a30-build fetch-cover-brick-build \
        extract-cover-a30-build extract-cover-brick-build \
        universal-package nextui-package

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Build with local test media paths (/tmp/storyboy_test/) instead of /mnt/SDCARD/
test: $(SRCS)
	$(CC) $(CFLAGS) -DSB_TEST_ROOTS -o $(TARGET) $^ $(LDFLAGS)

# ---- A30 targets ---------------------------------------------------------

miyoo-a30-build: $(A30_SRCS)
	$(A30_CC) $(A30_CFLAGS) -o $(A30_TARGET) $^ $(A30_LDFLAGS)
	@echo "Built: $(A30_TARGET)"

onion-build: $(ONION_SRCS)
	$(A30_CC) $(ONION_CFLAGS) -o $(ONION_TARGET) $^ $(A30_LDFLAGS)
	@echo "Built: $(ONION_TARGET)"

miyoo-a30-docker:
	docker build -f cross-compile/miyoo-a30/Dockerfile.storyboy \
	             -t $(SB_A30_IMAGE) .
	mkdir -p build
	env -u USER docker run --rm -v $(CURDIR):/storyboy $(SB_A30_IMAGE) \
	           sh cross-compile/miyoo-a30/build_inside_docker.sh

miyoo-a30-package: VERSION ?= test
miyoo-a30-package:
	sh cross-compile/miyoo-a30/package_storyboy_a30.sh $(VERSION)

miyoo-a30-deploy: build/storyboy32 build/extract_cover32
	ssh $(SB_A30_DEPLOY) "mkdir -p $(SB_A30_PATH)/lib32 $(SB_A30_PATH)/lib32_a30 $(SB_A30_PATH)/resources/fonts"
	scp build/storyboy32 $(SB_A30_DEPLOY):$(SB_A30_PATH)/bin32/storyboy
	scp build/extract_cover32 $(SB_A30_DEPLOY):$(SB_A30_PATH)/bin32/extract_cover
	scp -r build/libs32/. $(SB_A30_DEPLOY):$(SB_A30_PATH)/lib32/
	scp -r build/libs32_a30/. $(SB_A30_DEPLOY):$(SB_A30_PATH)/lib32_a30/
	scp cross-compile/universal/launch.sh \
	    cross-compile/universal/config.json \
	    $(SB_A30_DEPLOY):$(SB_A30_PATH)/
	scp resources/fonts/DejaVuSans.ttf \
	    $(SB_A30_DEPLOY):$(SB_A30_PATH)/resources/fonts/
	scp resources/default_cover.png \
	    resources/default_cover.svg \
	    resources/default_folder.svg \
	    resources/icon.png \
	    $(SB_A30_DEPLOY):$(SB_A30_PATH)/resources/
	ssh $(SB_A30_DEPLOY) "chmod +x $(SB_A30_PATH)/launch.sh $(SB_A30_PATH)/bin32/storyboy"
	@echo "Deployed to $(SB_A30_DEPLOY):$(SB_A30_PATH)"

# ---- Trimui Brick targets ------------------------------------------------

trimui-brick-build: $(BRICK_SRCS)
	$(BRICK_CC) $(BRICK_CFLAGS) -o $(BRICK_TARGET) $^ $(BRICK_LDFLAGS)
	@echo "Built: $(BRICK_TARGET)"

trimui-brick-docker:
	docker build -f cross-compile/trimui-brick/Dockerfile.storyboy \
	             -t $(SB_BRICK_IMAGE) .
	mkdir -p build
	env -u USER docker run --rm -v $(CURDIR):/storyboy $(SB_BRICK_IMAGE) \
	           sh cross-compile/trimui-brick/build_inside_docker.sh

trimui-brick-package: VERSION ?= test
trimui-brick-package:
	sh cross-compile/trimui-brick/package_storyboy_brick.sh $(VERSION)

trimui-brick-deploy: build/storyboy64
	ssh $(SB_BRICK_DEPLOY) "mkdir -p $(SB_BRICK_PATH)/bin64 $(SB_BRICK_PATH)/lib64 $(SB_BRICK_PATH)/resources/fonts"
	scp build/storyboy64 $(SB_BRICK_DEPLOY):$(SB_BRICK_PATH)/bin64/storyboy
	scp build/fetch_cover64 $(SB_BRICK_DEPLOY):$(SB_BRICK_PATH)/bin64/fetch_cover
	scp -r build/libs64/. $(SB_BRICK_DEPLOY):$(SB_BRICK_PATH)/lib64/
	scp cross-compile/universal/launch.sh \
	    cross-compile/universal/config.json \
	    $(SB_BRICK_DEPLOY):$(SB_BRICK_PATH)/
	scp resources/fonts/DejaVuSans.ttf \
	    $(SB_BRICK_DEPLOY):$(SB_BRICK_PATH)/resources/fonts/
	scp resources/default_cover.png \
	    resources/default_cover.svg \
	    resources/default_folder.svg \
	    resources/icon.png \
	    $(SB_BRICK_DEPLOY):$(SB_BRICK_PATH)/resources/
	ssh $(SB_BRICK_DEPLOY) "chmod +x $(SB_BRICK_PATH)/launch.sh $(SB_BRICK_PATH)/bin64/storyboy"
	@echo "Deployed to $(SB_BRICK_DEPLOY):$(SB_BRICK_PATH)"

# ---- fetch_cover targets (run inside Docker) -----------------------------

fetch-cover-a30-build: src/fetch_cover.c src/glibc_compat.c
	$(A30_CC) -Wall -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -DSB_A30 \
	    -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
	    $$(pkg-config --cflags libcurl) \
	    -o build/fetch_cover32 $< src/glibc_compat.c \
	    $$(pkg-config --static --libs libcurl) \
	    -lz -lm -static-libgcc
	@echo "Built: build/fetch_cover32"

fetch-cover-brick-build: src/fetch_cover.c
	$(BRICK_CC) -Wall -std=c11 -O2 -D_POSIX_C_SOURCE=200809L \
	    -march=armv8-a \
	    $$(pkg-config --cflags libcurl) \
	    -o build/fetch_cover64 $< \
	    $$(pkg-config --static --libs libcurl) \
	    -lz -lm -static-libgcc
	@echo "Built: build/fetch_cover64"

extract-cover-a30-build: src/extract_cover.c src/glibc_compat.c
	$(A30_CC) -Wall -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -DSB_A30 \
	    -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
	    $$(pkg-config --cflags libavformat libavutil) \
	    -o build/extract_cover32 src/extract_cover.c src/glibc_compat.c \
	    $$(pkg-config --static --libs libavformat libavutil) \
	    -lm -static-libgcc
	@echo "Built: build/extract_cover32"

extract-cover-brick-build: src/extract_cover.c
	$(BRICK_CC) -Wall -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 \
	    -march=armv8-a \
	    $$(pkg-config --cflags libavformat libavutil) \
	    -o build/extract_cover64 src/extract_cover.c \
	    $$(pkg-config --static --libs libavformat libavutil) \
	    -lm -static-libgcc
	@echo "Built: build/extract_cover64"

# ---- Universal package ---------------------------------------------------

universal-package: VERSION ?= test
universal-package:
	sh cross-compile/universal/package_storyboy_universal.sh $(VERSION)

nextui-package: VERSION ?= test
nextui-package:
	sh cross-compile/nextui/package_storyboy_nextui.sh $(VERSION)

clean:
	rm -f $(TARGET) $(A30_TARGET) $(BRICK_TARGET) $(ONION_TARGET)
	rm -rf build/storyboy32 build/libs32 build/libs32_a30
	rm -rf build/storyboy64 build/libs64
	rm -f build/fetch_cover32 build/fetch_cover64
	rm -f build/extract_cover32 build/extract_cover64
