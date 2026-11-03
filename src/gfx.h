/*
 * gfx.h - the screen.
 *
 * Replaces the original's software rasteriser and DirectDraw. The 2D
 * screens (menus, HUD overlays) are still drawn by the game into an 8 bit
 * surface at their original size; gfx_draw_screen shows that surface,
 * through its palette, centred at its original aspect ratio in the window.
 * 3D scenes are drawn on the GPU directly (gl3d.c). All GPU work goes
 * through rnd.h: OpenGL on macOS, GXM on the Vita.
 *
 * Must not include platform.h. Expects the host's window to be open.
 */

#ifndef IGN_GFX_H
#define IGN_GFX_H

#include "types.h"

int gfx_init(void);
void gfx_shutdown(void);

/* Starts a frame on a `width` x `height` pixel drawable: clears it to
 * black and sets the viewport. */
void gfx_begin_frame(int width, int height);

/* Draws an 8 bit surface of `w` x `h` pixels with a 256 entry RGB palette
 * (8 bits a channel), scaled to fill the drawable's height at a display
 * aspect of `aspect` (width / height) and centred. Index 0 is drawn like
 * any other colour. */
void gfx_draw_screen(const u8 *pixels, int w, int h, int pitch,
                     const u8 *palette, float aspect);

/* Draws `layer`, a texture of colour indices (rnd.h) (row 0 at the top),
 * over the whole drawable, then the surface over it as gfx_draw_screen
 * places it, but only its pixels that differ from `snapshot`. */
/* An RGBA layer gfx_draw_layers puts over the 3D; 0 for none. */
void gfx_set_overlay(unsigned overlay);
void gfx_draw_layers(unsigned layer, const u8 *pixels, const u8 *snapshot, int w, int h,
                     int pitch, const u8 *palette, float aspect);

/* Draws a texture of colour indices (rnd.h) (row 0 at the top) as
 * gfx_draw_screen places a surface. */
void gfx_draw_index_texture(unsigned tex, const u8 *palette, float aspect);

/* The drawable's pixels as drawn so far, bottom row first. */
void gfx_read_rgb(int w, int h, u8 *rgb);

#endif /* IGN_GFX_H */
