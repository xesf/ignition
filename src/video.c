/*
 * video.c - the Lisa video layer, on the GPU (gfx.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "canvas.h"
#include "gfx.h"
#include "platform/platform.h"
#include "video.h"
#include "prof.h"
#include "lisa/sprite.h"
#include "lisa/turned.h"
#include "stubs.h"

struct video_mode g_video_mode;     /* 0x4ba6e0 */
u8 *g_video_screen;
u8 g_video_palette[768];
float g_view_scale = 1.0f;

static int s_gfx_ready;
static const u8 *s_present;     /* a canvas surface to show, or NULL */
static const u8 *s_blit_src;    /* the last whole screen video_blit was given, or NULL */
static const u8 *screen_pixels(void)
{
    return s_blit_src != NULL ? s_blit_src : g_video_screen;
}

static unsigned s_layer;
static const u8 *s_layer_snapshot;
static int s_screen_w, s_screen_h;

static int gfx_register_generic(void);

/* 0x456af0
 * Driver 0 fills the generic table at 0x50eba0 (0x456e60) and the
 * DirectDraw table at 0x50eb68 (0x45b690). This file stands in for both;
 * there is nothing to fill. */
int video_select_driver(int driver)
{
    if (driver != 0) {
        return 2;
    }
    gfx_register_generic();
    return 1;
}

/* 0x456e60
 * Fills the generic table; what is left to do is set up the
 * rasteriser's buffers and the sprite records. */
static int gfx_register_generic(void)
{
    gfx_init_45d840();
    gfx_init_spans();
    sprite_init();
    return 1;
}

/* 0x456bc0, through the table entries 0x50eb6c and 0x50eba0.
 * The window opens wide; the original's full screen mode is a full
 * screen window. */
int video_create_window(void)
{
    if (!plat_window_open("Ignition", 1280, 720, g_app_fullscreen)) {
        return 0;
    }
    if (!s_gfx_ready) {
        if (!gfx_init() || !canvas_init()) {
            return 0;
        }
        s_gfx_ready = 1;
    }
    return 1;
}

/* 0x456bf0. The GL context outlives mode switches; see
 * plat_destroy_window. */
void video_shutdown(void)
{
}

/* 0x456be0, through 0x50eb70 */
int video_set_mode(void)
{
    u8 *screen;

    if (g_video_mode.bpp != 8 || g_video_mode.width <= 0 || g_video_mode.height <= 0) {
        return 0;
    }
    screen = calloc((size_t)g_video_mode.width, (size_t)g_video_mode.height);
    if (screen == NULL) {
        return 0;
    }
    free(g_video_screen);
    g_video_screen = screen;
    s_blit_src = NULL;
    s_screen_w = g_video_mode.width;
    s_screen_h = g_video_mode.height;
    return 1;
}

float video_aspect(void)
{
    if (s_screen_w == 320 && s_screen_h == 200) {
        return 4.0f / 3.0f;
    }
    return (float)s_screen_w / (float)s_screen_h;
}

/* 0x456b70, through 0x50eb80 (ddraw_blit, 0x45c1d0) */
int video_blit(const u8 *src, int pitch, int x, int y, int w, int h, int dx, int dy)
{
    int row;

    if (canvas_is_gpu(src)) {
        /* A GPU surface is shown as it is. */
        s_present = src;
        return 1;
    }
    s_present = NULL;
    if (g_video_screen == NULL) {
        return 0;
    }
    if (dx < 0 || dy < 0 || dx + w > s_screen_w || dy + h > s_screen_h) {
        return 0;
    }
    /* A whole screen: shown from where it is, without a copy (it stays
     * as it is until the next frame draws), as g_video_screen. */
    if (x == 0 && y == 0 && dx == 0 && dy == 0 && w == s_screen_w && h == s_screen_h
        && pitch == w) {
        s_blit_src = src;
        return 1;
    }
    s_blit_src = NULL;
    for (row = 0; row < h; row++) {
        memcpy(g_video_screen + (dy + row) * s_screen_w + dx,
               src + (y + row) * pitch + x, (size_t)w);
    }
    return 1;
}

/* 0x456c50, through 0x50eb9c (ddraw_restore, 0x45c730) */
int video_restore(void)
{
    return 1;
}

/* 0x456bb0, through 0x50eb84 (0x45c3d0) */
int video_clear(void)
{
    if (g_video_screen == NULL) {
        return 0;
    }
    s_blit_src = NULL;
    memset(g_video_screen, 0, (size_t)s_screen_w * (size_t)s_screen_h);
    return 1;
}

/* 0x456c90, through 0x50eb88 (ddraw_flip, 0x45c4b0) */
int video_flip(void)
{
    video_present();
    return 1;
}

/* 0x456c40, through 0x50eb98 (ddraw_set_palette, 0x45c6d0) */
int video_set_palette(const u8 *rgb)
{
    memcpy(g_video_palette, rgb, sizeof(g_video_palette));
    return 1;
}

/* Scheduled screenshots, taken in the order given. */
enum { SHOTS = 64 };
static struct {
    u32 ms;
    char path[1024];
} s_shots[SHOTS];
static int s_shot_count, s_shot_next;
static u32 s_shot_ms;
static char s_shot_path[1024];

void video_schedule_shot(u32 ms, const char *path)
{
    if (s_shot_count < SHOTS) {
        s_shots[s_shot_count].ms = ms;
        snprintf(s_shots[s_shot_count].path, sizeof(s_shots[0].path), "%s", path);
        s_shot_count++;
    }
    if (s_shot_path[0] == '\0' && s_shot_next < s_shot_count) {
        s_shot_ms = s_shots[s_shot_next].ms;
        snprintf(s_shot_path, sizeof(s_shot_path), "%s", s_shots[s_shot_next].path);
        s_shot_next++;
    }
}

/* With a 3D layer the shot is the drawable, at its size. */
static void write_drawable_shot(int w, int h)
{
    FILE *fp = fopen(s_shot_path, "wb");
    u8 header[18] = { 0, 0, 2 };
    u8 *rgb = malloc((size_t)w * h * 3);
    int x, y;

    if (fp == NULL || rgb == NULL) {
        if (fp != NULL) {
            fclose(fp);
        }
        free(rgb);
        return;
    }
    gfx_read_rgb(w, h, rgb);
    header[12] = (u8)w;
    header[13] = (u8)(w >> 8);
    header[14] = (u8)h;
    header[15] = (u8)(h >> 8);
    header[16] = 24;
    header[17] = 0x20;      /* top to bottom; GL reads bottom up */
    fwrite(header, 1, sizeof(header), fp);
    for (y = 0; y < h; y++) {
        u8 *row = rgb + (size_t)(h - 1 - y) * w * 3;

        for (x = 0; x < w; x++) {
            u8 t = row[3 * x];

            row[3 * x] = row[3 * x + 2];
            row[3 * x + 2] = t;
        }
        fwrite(row, 1, (size_t)w * 3, fp);
    }
    fclose(fp);
    free(rgb);
}

static void write_shot(void)
{
    FILE *fp = fopen(s_shot_path, "wb");
    u8 header[18] = { 0, 0, 2 };
    int x, y;

    if (fp == NULL) {
        perror(s_shot_path);
        return;
    }
    header[12] = (u8)s_screen_w;
    header[13] = (u8)(s_screen_w >> 8);
    header[14] = (u8)s_screen_h;
    header[15] = (u8)(s_screen_h >> 8);
    header[16] = 24;
    header[17] = 0x20;      /* top to bottom */
    fwrite(header, 1, sizeof(header), fp);
    for (y = 0; y < s_screen_h; y++) {
        for (x = 0; x < s_screen_w; x++) {
            const u8 *c = &g_video_palette[3 * screen_pixels()[y * s_screen_w + x]];
            u8 bgr[3] = { c[2], c[1], c[0] };

            fwrite(bgr, 1, 3, fp);
        }
    }
    fclose(fp);
}

static int s_shot_requested;

void video_request_shot(void)
{
    s_shot_requested = 1;
}

void video_present(void)
{
    int w, h;

    if (!s_gfx_ready || g_video_screen == NULL) {
        return;
    }
    if (s_shot_requested && s_shot_path[0] == '\0') {
        static int number;

        s_shot_requested = 0;
#ifdef __vita__
        snprintf(s_shot_path, sizeof(s_shot_path), "ux0:data/ignition/shot%02d.tga", number++);
#else
        snprintf(s_shot_path, sizeof(s_shot_path), "shot%02d.tga", number++);
#endif
        s_shot_ms = 0;
    }
    prof_begin(PROF_P_GFX);
    prof_begin(PROF_G_SYNC);
    canvas_sync_drawable();
    plat_drawable_size(&w, &h);
    prof_end(PROF_G_SYNC);
    prof_begin(PROF_G_BEGIN);
    gfx_begin_frame(w, h);
    prof_end(PROF_G_BEGIN);
    prof_begin(PROF_G_LAYERS);
    if (s_present != NULL) {
        gfx_draw_index_texture(canvas_texture(s_present), g_video_palette,
                               canvas_aspect(s_present, video_aspect()));
    } else if (s_layer != 0) {
        gfx_draw_layers(s_layer, screen_pixels(), s_layer_snapshot, s_screen_w, s_screen_h,
                        s_screen_w, g_video_palette, video_aspect());
    } else {
        gfx_draw_screen(screen_pixels(), s_screen_w, s_screen_h, s_screen_w,
                        g_video_palette, video_aspect());
    }
    prof_end(PROF_G_LAYERS);
    if (s_shot_path[0] != '\0' && plat_time_ms() >= s_shot_ms) {
        if (s_layer != 0 || s_present != NULL) {
            write_drawable_shot(w, h);
        } else {
            write_shot();
        }
        s_shot_path[0] = '\0';
        if (s_shot_next < s_shot_count) {
            s_shot_ms = s_shots[s_shot_next].ms;
            snprintf(s_shot_path, sizeof(s_shot_path), "%s", s_shots[s_shot_next].path);
            s_shot_next++;
        }
    }
    prof_end(PROF_P_GFX);
    plat_present();
}

void video_set_overlay(unsigned overlay)
{
    gfx_set_overlay(overlay);
}

void video_set_layer(unsigned layer, const u8 *snapshot)
{
    gfx_set_overlay(0);
    s_layer = layer;
    s_layer_snapshot = snapshot;
}

void video_drawable_size(int *w, int *h)
{
    plat_drawable_size(w, h);
}
