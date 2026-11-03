/*
 * video.h - the Lisa video layer, on the GPU (gfx.h).
 *
 * The original goes through a function table at 0x50eb6c, filled for
 * DirectDraw: create the window (0x456bc0), set a mode (0x456be0), set
 * the palette, flip. A mode here only sizes the 8 bit screen surface;
 * the window keeps its own size and gfx centres the surface in it.
 */

#ifndef IGN_VIDEO_H
#define IGN_VIDEO_H

#include "types.h"

/* The mode game code asks for before calling video_set_mode. */
struct video_mode {
    s32 width;      /* 0x4ba6e0 */
    s32 height;     /* 0x4ba6e4 */
    s32 bpp;        /* 0x4ba6e8 */
    s32 flag;       /* 0x4ba6ec, 1 in every call seen */
};

extern struct video_mode g_video_mode;  /* 0x4ba6e0 */

/* The screen surface, `width` x `height` at 8 bits. */
extern u8 *g_video_screen;
extern u8 g_video_palette[768];

/* The race's 3D view's resolution as a share of the drawable's: 1, or
 * less where the GPU needs the room (the Vita's, at 0.75). */
extern float g_view_scale;

/* Fills the driver tables for `driver` 0, DirectDraw. Returns 1, or 2
 * for any other driver. */
int video_select_driver(int driver);

int video_create_window(void);
void video_shutdown(void);
int video_set_mode(void);

/* Shows the screen surface with the current palette. */
void video_present(void);

/* 0x456b70: copies `w` x `h` pixels at (x, y) of `src` to (dx, dy) of
 * the screen surface. The original also names the DirectDraw surface;
 * there is one here. */
int video_blit(const u8 *src, int pitch, int x, int y, int w, int h, int dx, int dy);

/* 0x456c50: restores DirectDraw surfaces lost to another program. The
 * OpenGL surface is never lost. */
int video_restore(void);

/* 0x456bb0: clears the screen surface to colour 0. */
int video_clear(void);

/* 0x456c90: shows the screen surface. */
int video_flip(void);

/* 0x456c40: 256 RGB entries, 8 bits a channel. */
int video_set_palette(const u8 *rgb);

/* Writes the screen surface, through its palette, to `path` as an
 * uncompressed 24 bit TGA when `ms` has passed, for scripted runs. */
void video_schedule_shot(u32 ms, const char *path);

/* The aspect the surface is shown at. 320 x 200 was shown on a 4:3
 * monitor, so it keeps 4:3; other modes have square pixels. */
float video_aspect(void);
/* Shows `layer` (gl3d_view_texture) under the screen; where the screen
 * equals `snapshot` the layer shows through. 0 for none. */
void video_set_layer(unsigned layer, const u8 *snapshot);
/* An RGBA layer (premultiplied) shown over the 3D layer, under the
 * screen's changes; 0 for none. video_set_layer clears it. */
void video_set_overlay(unsigned overlay);
/* A screenshot of the next frame, to shotNN.tga (the Vita's L and R). */
void video_request_shot(void);
void video_drawable_size(int *w, int *h);

#endif /* IGN_VIDEO_H */
