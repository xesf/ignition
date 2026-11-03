/*
 * gfx.c - the screen: colour indices through the palette (rnd.h).
 */

#include "gfx.h"
#include "rnd.h"
#include "prof.h"

static unsigned s_index_tex;    /* R8, the surface */
static unsigned s_palette_tex;  /* 256 x 1 RGB8 */
static unsigned s_snap_tex;     /* R8, the screen as the 3D left it */
static int s_draw_w, s_draw_h;
static const u8 *s_snap_src;    /* what s_snap_tex holds */
static int s_snap_w, s_snap_h;
static unsigned s_overlay;      /* RGBA8, over the 3D */

void gfx_set_overlay(unsigned overlay)
{
    s_overlay = overlay;
}

int gfx_init(void)
{
    if (!rnd_init()) {
        return 0;
    }
    s_index_tex = rnd_texture(RND_R8, 1, 1, NULL, 0);
    s_palette_tex = rnd_texture(RND_RGB8, 256, 1, NULL, 0);
    s_snap_tex = rnd_texture(RND_R8, 1, 1, NULL, 0);
    s_snap_src = NULL;
    return 1;
}

void gfx_shutdown(void)
{
    rnd_texture_free(s_index_tex);
    rnd_texture_free(s_palette_tex);
    rnd_texture_free(s_snap_tex);
    rnd_shutdown();
}

void gfx_begin_frame(int width, int height)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    s_draw_w = width;
    s_draw_h = height;
    rnd_bind_screen(width, height);
    rnd_clear(black, 1);
}

/* The rectangle a screen of `aspect` takes: the drawable's height,
 * pillarboxed, or letterboxed when the window is narrower. */
static void screen_rect(float aspect, float rect[4])
{
    float sw, sh = 1.0f;

    sw = aspect * (float)s_draw_h / (float)s_draw_w;
    if (sw > 1.0f) {
        sh = 1.0f / sw;
        sw = 1.0f;
    }
    rect[0] = -sw;
    rect[1] = -sh;
    rect[2] = sw;
    rect[3] = sh;
}

static void set_palette(const u8 *palette)
{
    rnd_texture_set(s_palette_tex, 256, 1, palette, 256);
}

void gfx_draw_screen(const u8 *pixels, int w, int h, int pitch,
                     const u8 *palette, float aspect)
{
    float rect[4];

    rnd_texture_set(s_index_tex, w, h, pixels, pitch);
    set_palette(palette);
    screen_rect(aspect, rect);
    rnd_use(RND_PRESENT);
    rnd_uniform(RND_U_RECT, rect);
    rnd_texture_unit(0, s_index_tex);
    rnd_texture_unit(1, s_palette_tex);
    rnd_depth(RND_DEPTH_OFF, 1);
    rnd_draw(RND_STRIP, 0, 4);
}

void gfx_draw_layers(unsigned layer, const u8 *pixels, const u8 *snapshot, int w, int h,
                     int pitch, const u8 *palette, float aspect)
{
    static const float whole[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    float rect[4];

    prof_begin(PROF_H_PAL);
    set_palette(palette);
    prof_end(PROF_H_PAL);
    prof_begin(PROF_H_3D);
    rnd_depth(RND_DEPTH_OFF, 1);

    /* The 3D, the whole drawable. */
    rnd_use(RND_PRESENT);
    rnd_uniform(RND_U_RECT, whole);
    rnd_texture_unit(0, layer);
    rnd_texture_unit(1, s_palette_tex);
    rnd_draw(RND_STRIP, 0, 4);

    /* The 3D effects, blended over it. */
    if (s_overlay != 0) {
        rnd_use(RND_LAYER);
        rnd_uniform(RND_U_RECT, whole);
        rnd_texture_unit(0, s_overlay);
        rnd_blend(RND_BLEND_PREMUL);
        rnd_draw(RND_STRIP, 0, 4);
        rnd_blend(RND_BLEND_OFF);
    }

    prof_end(PROF_H_3D);
    prof_begin(PROF_H_TEX);
    /* What the 2D drawing changed, at the screen's place. */
    rnd_texture_set(s_index_tex, w, h, pixels, pitch);
    prof_end(PROF_H_TEX);
    prof_begin(PROF_H_SNAP);
    /* The snapshot is the same buffer, never written, in a race: it is
     * uploaded once (video_set_layer). */
    if (snapshot != s_snap_src || w != s_snap_w || h != s_snap_h) {
        rnd_texture_set(s_snap_tex, w, h, snapshot, pitch);
        s_snap_src = snapshot;
        s_snap_w = w;
        s_snap_h = h;
    }
    prof_end(PROF_H_SNAP);
    prof_begin(PROF_H_OVER);
    screen_rect(aspect, rect);
    rnd_use(RND_OVER);
    rnd_uniform(RND_U_RECT, rect);
    rnd_texture_unit(0, s_index_tex);
    rnd_texture_unit(1, s_palette_tex);
    rnd_texture_unit(2, s_snap_tex);
    rnd_draw(RND_STRIP, 0, 4);
    prof_end(PROF_H_OVER);
}

void gfx_read_rgb(int w, int h, u8 *rgb)
{
    rnd_read(0, 0, w, h, RND_RGB8, rgb, w * 3);
}

void gfx_draw_index_texture(unsigned tex, const u8 *palette, float aspect)
{
    float rect[4];

    set_palette(palette);
    screen_rect(aspect, rect);
    rnd_depth(RND_DEPTH_OFF, 1);
    rnd_scissor(0, 0, 0, 0, 0);
    rnd_use(RND_PRESENT);
    rnd_uniform(RND_U_RECT, rect);
    rnd_texture_unit(0, tex);
    rnd_texture_unit(1, s_palette_tex);
    rnd_draw(RND_STRIP, 0, 4);
}
