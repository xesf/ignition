# Ignition - reconstructed engine
#
# The engine core is plain C99; each host is one native file behind
# src/platform/platform.h, and every host but the Vita's draws with OpenGL
# through src/rnd_gl.c:
#
#   make                 the native build: macOS (src/platform/macos.m,
#                        Cocoa) or Linux (src/platform/linux.c, X11 + GLX)
#   make linux           Linux, also the Steam Deck build: build-linux/
#   make windows         Windows x86_64 (src/platform/windows.c, Win32 + WGL),
#                        cross-built with MinGW-w64: build-windows/
#   make web             WebAssembly and WebGL 2 with Emscripten
#                        (src/platform/web.c): build-web/dist/
#   make android         Android APK (src/platform/android.c, NDK, no Gradle)
#   make ios / ios-sim   iOS and iPadOS (src/platform/ios.m, Xcode)
#   make vita            PlayStation Vita (src/platform/vita.c,
#                        src/rnd_gxm.c, docs/vita.md)
#
# No third-party libraries: only each system's own APIs, and Khronos'
# OpenGL headers in src/third_party/. VERSION holds this port's
# version; `make VERSION=v1.2.3` overrides it (CI passes the release
# tag). docs/platforms.md covers each platform.

.DEFAULT_GOAL := all

TARGET := ignition
SRCDIR := src
BUILD  := build
CD_EXTRACT ?= cd_extract

VERSION ?= $(shell cat VERSION)
IGN_VERSION := $(patsubst v%,%,$(VERSION))

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
HOST ?= macos
else
HOST ?= linux
endif

CORE_SRC := $(SRCDIR)/app.c $(SRCDIR)/game.c $(SRCDIR)/stubs.c $(SRCDIR)/net.c $(SRCDIR)/canvas.c $(SRCDIR)/scene.c $(SRCDIR)/gl3d.c $(SRCDIR)/menu/car.c $(SRCDIR)/menu/bg.c $(SRCDIR)/race/load.c $(SRCDIR)/race/track.c $(SRCDIR)/race/world.c $(SRCDIR)/race/cars.c $(SRCDIR)/phys.c $(SRCDIR)/race/phys.c $(SRCDIR)/anim_tex.c $(SRCDIR)/race/ai.c $(SRCDIR)/srf.c $(SRCDIR)/race/contact.c $(SRCDIR)/race/fx.c $(SRCDIR)/race/respawn.c $(SRCDIR)/race/camera.c $(SRCDIR)/race/view.c $(SRCDIR)/race/misc.c $(SRCDIR)/race/hud.c $(SRCDIR)/race/results.c $(SRCDIR)/race/sound.c $(SRCDIR)/race/control.c $(SRCDIR)/race/init.c $(SRCDIR)/race/race.c $(SRCDIR)/race/frame_stubs.c $(SRCDIR)/race/input.c $(SRCDIR)/race/data.c \
            $(SRCDIR)/lisa/lisa.c $(SRCDIR)/lisa/log.c $(SRCDIR)/lisa/mem.c $(SRCDIR)/lisa/file.c \
            $(SRCDIR)/lisa/exit.c $(SRCDIR)/lisa/gfx.c $(SRCDIR)/lisa/texture.c $(SRCDIR)/lisa/sprite.c \
            $(SRCDIR)/lisa/font.c $(SRCDIR)/startup.c $(SRCDIR)/input.c \
            $(SRCDIR)/menu/engine.c $(SRCDIR)/menu/data.c $(SRCDIR)/settings.c $(SRCDIR)/frontend/frontend.c $(SRCDIR)/frontend/state.c \
            $(SRCDIR)/menu/panels.c $(SRCDIR)/menu/pages.c $(SRCDIR)/audio.c $(SRCDIR)/cd.c $(SRCDIR)/sound.c $(SRCDIR)/cdp.c \
            $(SRCDIR)/video.c $(SRCDIR)/gfx.c $(SRCDIR)/rnd_gl.c $(SRCDIR)/vfs.c $(SRCDIR)/main.c \
            $(SRCDIR)/cdimage.c $(SRCDIR)/prof.c

WARN := -Wall -Wextra -Wno-unused-parameter

ifeq ($(HOST),macos)
# MACOS_ARCH="-arch arm64 -arch x86_64" makes a universal binary (CI).
MAC_SRC  := $(SRCDIR)/platform/macos.m
HOST_C   := $(SRCDIR)/platform/common.c

CC        := clang
CFLAGS    := -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE $(WARN) -g -O1 -I$(SRCDIR) $(MACOS_ARCH)
OBJCFLAGS := -fobjc-arc -DGL_SILENCE_DEPRECATION -Wno-deprecated-declarations $(WARN) -g -O1 -I$(SRCDIR) $(MACOS_ARCH)
LDFLAGS   := -framework Cocoa -framework OpenGL -framework AudioUnit -framework AudioToolbox $(MACOS_ARCH)
TOOL_CFLAGS := $(CFLAGS)
else
# Linux, Windows and the web. -fsigned-char: the game is x86 code, where
# char is signed (arm64 Linux). GCC also warns where snprintf may
# truncate, which the game means.
MAC_SRC  :=
OPT      ?= -O2
TOOL_CFLAGS := -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -fsigned-char $(WARN) $(OPT) -I$(SRCDIR)
ifneq ($(shell $(CC) --version 2>/dev/null | grep -c "Free Software Foundation"),0)
TOOL_CFLAGS += -Wno-format-truncation -Wno-format-overflow
endif
CFLAGS   := $(TOOL_CFLAGS) -g -I$(SRCDIR)/third_party

ifeq ($(HOST),linux)
HOST_C   := $(SRCDIR)/platform/linux.c $(SRCDIR)/platform/common.c
LDFLAGS  := -lX11 -lGL -lasound -lpthread -lm
endif
ifeq ($(HOST),windows)
HOST_C   := $(SRCDIR)/platform/windows.c $(SRCDIR)/platform/common.c
LDFLAGS  := -mwindows -static -lopengl32 -lgdi32 -lwinmm -luser32
endif
ifeq ($(HOST),web)
HOST_C   := $(SRCDIR)/platform/web.c $(SRCDIR)/platform/common.c $(SRCDIR)/platform/touch.c
CFLAGS   += -DIGN_GLES
LDFLAGS  := $(WEB_LDFLAGS)
endif
endif

HOST_OBJ := $(patsubst $(SRCDIR)/%.m,$(BUILD)/%.o,$(MAC_SRC)) $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(HOST_C))
CORE_OBJ := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(CORE_SRC))

# The version only reaches the files that print it.
$(BUILD)/main.o $(BUILD)/startup.o: CFLAGS += -DIGN_VERSION='"$(IGN_VERSION)"'
$(BUILD)/main.o $(BUILD)/startup.o: VERSION

.PHONY: all clean extract test run vita vita-deploy vita-clean linux linux-clean windows \
        windows-clean web web-serve web-clean android android-install android-clean \
        ios-configure ios ios-sim ios-clean

all: $(TARGET) $(CD_EXTRACT)

$(TARGET): $(CORE_OBJ) $(HOST_OBJ)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD)/%.o: $(SRCDIR)/%.c $(wildcard $(SRCDIR)/*.h $(SRCDIR)/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SRCDIR)/%.m $(wildcard $(SRCDIR)/*.h $(SRCDIR)/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(OBJCFLAGS) -c $< -o $@

$(CD_EXTRACT): tools/cd_extract.c $(SRCDIR)/cdimage.c $(SRCDIR)/cdimage.h
	$(CC) $(TOOL_CFLAGS) tools/cd_extract.c $(SRCDIR)/cdimage.c -o $@

# The engine without main.c, for tests.
TEST_OBJ := $(filter-out $(BUILD)/main.o,$(CORE_OBJ)) $(HOST_OBJ)

test_lisa: tests/test_lisa.c $(TEST_OBJ)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_menu: tests/test_menu.c $(TEST_OBJ)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_settings: tests/test_settings.c $(TEST_OBJ)
	$(CC) $(CFLAGS) -Itests $^ $(LDFLAGS) -o $@

test_sound: tests/test_sound.c $(TEST_OBJ)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test: test_lisa test_menu test_settings test_sound
	./test_lisa $(BUILD)
	./test_menu
	./test_settings
	./test_sound

# Extracts each disc image in data/ into data/<image name>/disc/.
extract: cd_extract
	@for cue in data/*/*.cue; do \
		./cd_extract "$$cue" "$$(dirname "$$cue")/disc" || exit 1; \
	done

run: $(TARGET)
	./$(TARGET)

# ---------------------------------------------------------------- other hosts
#
# Linux, Windows and the web run this Makefile again with their HOST and
# build folder; Android and iOS have their own build files under
# platforms/. docs/platforms.md has the details.

NPROC ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Linux, and the Steam Deck: build-linux/ignition. Needs the X11, OpenGL
# and ALSA development files (libx11-dev libgl-dev libasound2-dev).
linux:
	$(MAKE) -j$(NPROC) HOST=linux BUILD=build-linux/obj TARGET=build-linux/ignition \
		CD_EXTRACT=build-linux/cd_extract all

linux-clean:
	rm -rf build-linux

# Windows x86_64: build-windows/ignition.exe, cross-compiled with
# MinGW-w64 (x86_64-w64-mingw32-gcc). MINGW= uses a native gcc (MSYS2).
MINGW ?= x86_64-w64-mingw32-

windows:
	$(MAKE) -j$(NPROC) HOST=windows CC=$(MINGW)gcc BUILD=build-windows/obj \
		TARGET=build-windows/ignition.exe CD_EXTRACT=build-windows/cd_extract.exe all

windows-clean:
	rm -rf build-windows

# The web: build-web/dist/, a static page (index.html, ignition.js,
# ignition.wasm). Needs Emscripten (emcc on PATH). WebGL 2 and Web Audio
# through Emscripten's own APIs; Asyncify lets the game's loops wait for
# the browser.
WEB_DIST := build-web/dist
WEB_LDFLAGS := -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sASYNCIFY \
               -sASYNCIFY_STACK_SIZE=262144 -sSTACK_SIZE=2MB -sINITIAL_MEMORY=256MB \
               -sALLOW_MEMORY_GROWTH=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sENVIRONMENT=web \
               -sEXPORTED_RUNTIME_METHODS=callMain,FS -sEXPORTED_FUNCTIONS=_main,_web_audio_fill \
               -lidbfs.js

web:
	@command -v emcc >/dev/null || { echo "Emscripten not found: install emsdk and source emsdk_env.sh."; exit 1; }
	@mkdir -p $(WEB_DIST)
	$(MAKE) -j$(NPROC) HOST=web CC=emcc BUILD=build-web/obj TARGET=$(WEB_DIST)/ignition.js \
		WEB_LDFLAGS="$(WEB_LDFLAGS)" $(WEB_DIST)/ignition.js
	cp platforms/web/index.html $(WEB_DIST)/index.html

web-serve: web
	python3 -m http.server -d $(WEB_DIST) 8080

web-clean:
	rm -rf build-web

# Android: build-android/ignition.apk (armeabi-v7a, arm64-v8a and x86_64). Needs
# ANDROID_HOME with an NDK, build-tools and a platform, and a JDK; see
# platforms/android/README.md.
android:
	sh platforms/android/build.sh $(VERSION)

android-install: android
	adb install -r build-android/ignition.apk

android-clean:
	rm -rf build-android

# iOS and iPadOS: an Xcode project from platforms/ios/CMakeLists.txt.
# `make ios` builds an unsigned build-ios/ignition.ipa for sideloading,
# `make ios-sim` a build for the simulator.
IOS_BUILD := build-ios

ios-configure:
	@command -v xcodebuild >/dev/null || { echo "Xcode not found."; exit 1; }
	cmake -S platforms/ios -B $(IOS_BUILD) -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
		-DIGN_VERSION=$(IGN_VERSION)

ios-sim: ios-configure
	cmake --build $(IOS_BUILD) --config Release -- -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO

ios: ios-configure
	cmake --build $(IOS_BUILD) --config Release -- -sdk iphoneos CODE_SIGNING_ALLOWED=NO
	rm -rf $(IOS_BUILD)/Payload $(IOS_BUILD)/ignition.ipa
	mkdir -p $(IOS_BUILD)/Payload
	cp -R $(IOS_BUILD)/Release-iphoneos/Ignition.app $(IOS_BUILD)/Payload/
	cd $(IOS_BUILD) && zip -qr ignition.ipa Payload
	@echo "ignition: $(IOS_BUILD)/ignition.ipa (unsigned)"

ios-clean:
	rm -rf $(IOS_BUILD)

# PlayStation Vita: build-vita/ignition.vpk. Needs VitaSDK ($VITASDK, or
# ~/vitasdk), nothing else. The renderer is src/rnd_gxm.c.
VITASDK ?= $(HOME)/vitasdk
VBUILD  := build-vita
JOBS    ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

vita:
	@test -f $(VBUILD)/CMakeCache.txt || VITASDK=$(VITASDK) cmake -S platforms/vita -B $(VBUILD) >/dev/null
	@VITASDK=$(VITASDK) cmake --build $(VBUILD) -j $(JOBS)
	@echo "ignition: $(VBUILD)/ignition.vpk"

# To the console (tools/vita_deploy.sh): the shaders it compiled come
# back into platforms/vita/shaders/ first, so the package carries them.
vita-deploy:
	@sh tools/vita_deploy.sh pull
	@$(MAKE) --no-print-directory vita
	@sh tools/vita_deploy.sh push

vita-clean:
	rm -rf $(VBUILD)

clean:
	rm -rf $(BUILD) $(TARGET) cd_extract test_lisa test_menu test_settings test_sound *.dSYM
