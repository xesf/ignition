# Standalone headless renderer tests; also included by the main Makefile.
# Uses only GNU Make, a POSIX shell and a native C compiler.
.DEFAULT_GOAL := test-renderer
RENDER_BUILD ?= build/renderer-tests
RENDER_EXE :=
RENDER_HOST := $(shell uname -s)
RENDER_CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Wno-unused-parameter \
    -D_POSIX_C_SOURCE=200809L -Itests/fake_gles -Isrc -Isrc/soft3d \
    -ffunction-sections -fdata-sections $(RENDER_EXTRA_CFLAGS)
RENDER_LDFLAGS := -Wl,--gc-sections $(RENDER_EXTRA_LDFLAGS)
ifeq ($(RENDER_HOST),Darwin)
RENDER_LDFLAGS := -Wl,-dead_strip $(RENDER_EXTRA_LDFLAGS)
endif
ifeq ($(OS),Windows_NT)
RENDER_EXE := .exe
RENDER_CFLAGS += -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-jump-tables
ifeq ($(origin CC),default)
CC := gcc
endif
endif

RENDER_ANDROID := -DIGN_GLES=1 -D__ANDROID__=1
RENDER_FETCH := $(RENDER_ANDROID) -DIGN_GLES_FRAMEBUFFER_FETCH=1
RENDER_TEST_NAMES :=
RENDER_DEPS := $(wildcard src/*.h src/soft3d/*.h tests/test_*.c \
    tests/fake_gles/GLES3/*.h tests/fake_gles/OpenGLES/ES3/*.h) \
    src/gl3d.c src/rnd_gl.c src/prof.c

$(RENDER_BUILD):
	mkdir -p "$@"

# The source list deliberately excludes TEST_OBJ: some fixtures include their
# production translation unit to exercise otherwise-static real entry points.
define renderer_case
RENDER_TEST_NAMES += $(1)
$(RENDER_BUILD)/$(1)$(RENDER_EXE): $(2) $(RENDER_DEPS) | $(RENDER_BUILD)
	$$(CC) $$(RENDER_CFLAGS) $(3) $(2) $$(RENDER_LDFLAGS) -lm -o $$@
endef

$(eval $(call renderer_case,fetch-default,tests/test_dyn_fetch_default.c,))
$(eval $(call renderer_case,arena-on,tests/test_perf_stream_arena.c,$(RENDER_ANDROID) -DIGN_GLES_DYN_ARENA=1))
$(eval $(call renderer_case,arena-off,tests/test_perf_stream_arena.c,$(RENDER_ANDROID) -DIGN_GLES_DYN_ARENA=0))
$(eval $(call renderer_case,backend-fetch,tests/test_rnd_dyn_fetch.c src/rnd_gl.c,$(RENDER_FETCH) -DIGN_GLES_DYN_ARENA=1 -DTEST_EXPECT_FETCH_BUILD=1))
$(eval $(call renderer_case,backend-fallback,tests/test_rnd_dyn_fetch.c src/rnd_gl.c,$(RENDER_ANDROID) -DIGN_GLES_FRAMEBUFFER_FETCH=0 -DTEST_EXPECT_FETCH_BUILD=0))
$(eval $(call renderer_case,batch-android,tests/test_batch_copies.c src/rnd_gl.c,$(RENDER_ANDROID) -DTEST_EXPECT_BATCH_COPIES=1))
$(eval $(call renderer_case,batch-off,tests/test_batch_copies.c src/rnd_gl.c,$(RENDER_ANDROID) -DIGN_GLES_BATCH_COPIES=0 -DTEST_EXPECT_BATCH_COPIES=0))
$(eval $(call renderer_case,batch-other,tests/test_batch_copies.c src/rnd_gl.c,-DIGN_GLES=1 -DTEST_EXPECT_BATCH_COPIES=0))
$(eval $(call renderer_case,profiler,tests/test_prof_counts.c,-DIGN_PROF=1))
$(eval $(call renderer_case,profiler-off,tests/test_prof_disabled.c,))

$(eval $(call renderer_case,visibility-on,tests/test_perf_visibility.c,$(RENDER_ANDROID) -DIGN_GLES_VISIBILITY_CULL=1))
$(eval $(call renderer_case,visibility-off,tests/test_perf_visibility.c,$(RENDER_ANDROID) -DIGN_GLES_VISIBILITY_CULL=0))
$(eval $(call renderer_case,stream-visibility,tests/test_perf_stream_visibility.c src/prof.c,$(RENDER_ANDROID) -DIGN_GLES_DYN_ARENA=1 -DIGN_GLES_VISIBILITY_CULL=1 -DIGN_PROF=1))
$(eval $(call renderer_case,state-on,tests/test_perf_state_cache.c src/rnd_gl.c src/prof.c,$(RENDER_FETCH) -DIGN_GLES_STATE_CACHE=1 -DTEST_EXPECT_STATE_CACHE=1 -DIGN_PROF=1))
$(eval $(call renderer_case,state-off,tests/test_perf_state_cache.c src/rnd_gl.c,$(RENDER_FETCH) -DIGN_GLES_STATE_CACHE=0 -DTEST_EXPECT_STATE_CACHE=0))
$(eval $(call renderer_case,state-stream,tests/test_perf_state_stream.c src/rnd_gl.c src/prof.c,$(RENDER_FETCH) -DIGN_GLES_DYN_ARENA=1 -DIGN_GLES_VISIBILITY_CULL=1 -DIGN_GLES_STATE_CACHE=1 -DTEST_EXPECT_STATE_CACHE=1 -DIGN_PROF=1))
$(eval $(call renderer_case,shared-gl3d,tests/test_gl3d_dyn_fetch.c,$(RENDER_FETCH) -DIGN_GLES_SHARED_PAGES=1))
$(eval $(call renderer_case,shared-stream,tests/test_perf_shared_stream.c src/prof.c,$(RENDER_FETCH) -DIGN_GLES_DYN_ARENA=1 -DIGN_GLES_VISIBILITY_CULL=1 -DIGN_GLES_SHARED_PAGES=1 -DIGN_PROF=1))
$(eval $(call renderer_case,shared-off,tests/test_perf_shared_stream.c,$(RENDER_FETCH) -DIGN_GLES_DYN_ARENA=1 -DIGN_GLES_VISIBILITY_CULL=1 -DIGN_GLES_SHARED_PAGES=0))
$(eval $(call renderer_case,pages-cache,tests/test_perf_pages_cache.c src/rnd_gl.c src/prof.c,$(RENDER_FETCH) -DIGN_GLES_DYN_ARENA=1 -DIGN_GLES_STATE_CACHE=1 -DIGN_GLES_SHARED_PAGES=1 -DTEST_EXPECT_FETCH_BUILD=1 -DIGN_PROF=1))
$(eval $(call renderer_case,pages-no-cache,tests/test_perf_pages_cache.c src/rnd_gl.c,$(RENDER_FETCH) -DIGN_GLES_DYN_ARENA=1 -DIGN_GLES_STATE_CACHE=0 -DIGN_GLES_SHARED_PAGES=1 -DTEST_EXPECT_FETCH_BUILD=1))
$(eval $(call renderer_case,pages-nonfetch,tests/test_rnd_dyn_fetch.c src/rnd_gl.c,$(RENDER_ANDROID) -DIGN_GLES_DYN_ARENA=1 -DIGN_GLES_STATE_CACHE=1 -DIGN_GLES_SHARED_PAGES=1 -DTEST_EXPECT_FETCH_BUILD=0))
$(eval $(call renderer_case,state-nonfetch,tests/test_perf_state_cache.c src/rnd_gl.c,$(RENDER_ANDROID) -DIGN_GLES_STATE_CACHE=1 -DTEST_EXPECT_STATE_CACHE=1))
$(eval $(call renderer_case,layers-other,tests/test_gl3d_dyn_fetch.c,-DIGN_GXM=1))
RENDER_RUN_TARGETS := $(addprefix renderer-run-,$(RENDER_TEST_NAMES))
.PHONY: test-renderer $(RENDER_RUN_TARGETS)
test-renderer: $(RENDER_RUN_TARGETS)
$(RENDER_RUN_TARGETS): renderer-run-%: $(RENDER_BUILD)/%$(RENDER_EXE)
	./$<
