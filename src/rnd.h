/*
 * rnd.h - the GPU operations the renderer is made of, one backend for
 * each host:
 *
 *   rnd_gl.c    OpenGL 3.2 core, shaders in GLSL 150 (macOS)
 *   rnd_gxm.c   GXM, shaders in Cg (PlayStation Vita)
 *
 * canvas.c, gl3d.c and gfx.c draw only through these. Everything is
 * colour indices until the present: render targets hold an index in R
 * (and in G, for the 3D targets, whether a face covers the pixel), and
 * gfx.c shows them through a 256 entry palette.
 *
 * Coordinates are those of OpenGL's framebuffer objects: a target's row
 * 0 is its first row in memory, and clip space y = -1 is row 0. The
 * screen (target 0) is the other way up, as a window is.
 *
 * Textures and targets are unsigned handles, 0 for none. Textures are
 * nearest and clamped, row 0 first. A target draws into a texture
 * (R8 or RG8), with a depth buffer or not.
 */

#ifndef IGN_RND_H
#define IGN_RND_H

#include "types.h"

/* Texture formats. RGB8 is the palette's: 3 bytes a texel. */
enum { RND_R8, RND_RG8, RND_RGB8 };

/* The programs. Each has its vertex layout (floats a vertex) and its
 * textures by unit:
 *
 *   CANVAS   x, y, u, v               0 source, 1 table, 2 table rows,
 *                                     3 copy of the target
 *   PRESENT  none (a quad)            0 indices, 1 palette
 *   OVER     none (a quad)            0 indices, 1 palette, 2 snapshot
 *   MESH     x, y, z, u, v, page,     0 pages
 *            colour, mode
 *   DYN      x, y, z, w, u, v, page,  0 pages, 1 table, 2 copy
 *            colour, mode
 *   RASTER   x, y, u, v, page,        0 pages, 1 table, 2 copy
 *            colour, mode, -
 *
 * The shaders and what the uniforms mean are in the backends. */
enum { RND_CANVAS, RND_PRESENT, RND_OVER, RND_MESH, RND_DYN, RND_RASTER, RND_PROGRAMS };

/* Uniforms, by the program that has them: CANVAS LSIZE, OFF (vec2),
 * MODE (0 or 1); PRESENT and OVER RECT (vec4); MESH MATRIX (mat4,
 * column major); DYN USE_TABLE (0 or 1); RASTER SIZE, OFF (vec2). */
enum { RND_U_LSIZE, RND_U_OFF, RND_U_MODE, RND_U_RECT, RND_U_MATRIX, RND_U_USE_TABLE,
       RND_U_SIZE, RND_UNIFORMS };

enum { RND_DEPTH_OFF, RND_DEPTH_LESS, RND_DEPTH_LEQUAL };
/* Which faces are dropped: clockwise or counter-clockwise as the target's
 * rows are numbered (OpenGL's window coordinates). */
enum { RND_CULL_OFF, RND_CULL_CW, RND_CULL_CCW };
enum { RND_TRIANGLES, RND_STRIP };

/* Needs the host's window (and, on macOS, its current GL context). */
int rnd_init(void);
void rnd_shutdown(void);

/* 1 only while the Android GLES DYN shader can read the bound colour
 * target directly and the experimental framebuffer-fetch build is
 * active. Other backends need no implementation. */
#if defined(IGN_GLES)
int rnd_dyn_framebuffer_fetch(void);
#else
static inline int rnd_dyn_framebuffer_fetch(void)
{
    return 0;
}
#endif

#if defined(IGN_GLES) && defined(__ANDROID__) && defined(IGN_GLES_SHARED_PAGES) && \
    IGN_GLES_SHARED_PAGES == 1
#define IGN_SHARED_PAGES_BUILD 1
#else
#define IGN_SHARED_PAGES_BUILD 0
#endif

#if IGN_SHARED_PAGES_BUILD
/* A shared R8 256x256 page array for the Android GLES DYN fetch path.
 * create returns its texture and its usable layer count, or 0. */
enum { RND_PAGE_POOL_MAX_LAYERS = 256 };
unsigned rnd_page_pool_create(int *layers);
int rnd_page_pool_upload(unsigned tex, int base_layer, const u8 *pixels, int pages);
void rnd_page_pool_release(unsigned tex);
#endif

/* A texture of w x h, from `pixels` (rows `pitch` texels apart; 0 for
 * w) or empty (zeros). */
unsigned rnd_texture(int format, int w, int h, const void *pixels, int pitch);
/* New contents for the whole texture, which may change its size. */
void rnd_texture_set(unsigned tex, int w, int h, const void *pixels, int pitch);
void rnd_texture_free(unsigned tex);
/* `count` R8 pages of 256 x 256, one after the other in `pixels`,
 * repeating; freed with rnd_texture_free. */
unsigned rnd_pages(const u8 *pixels, int count);

/* A target drawing into `color` (R8 or RG8), with a depth buffer if
 * `depth`. Freeing it leaves the texture. */
unsigned rnd_target(unsigned color, int depth);
void rnd_target_free(unsigned target);
/* Draws into `target` from now on, the viewport all of it. */
void rnd_bind(unsigned target);
/* Draws into the screen, `w` x `h` pixels. */
void rnd_bind_screen(int w, int h);
/* Clears the bound target's colour to `rgba` (unless NULL) and its
 * depth to 1 (if `depth`), within the scissor. Depth is only ever
 * cleared whole. */
void rnd_clear(const float *rgba, int depth);
/* Reads a rectangle of the bound target into `out`, rows `pitch` bytes
 * apart; R8 and RG8 from targets, RGB8 from the screen. */
void rnd_read(int x, int y, int w, int h, int format, void *out, int pitch);
/* Copies a rectangle of the bound target into the same place of `tex`,
 * a texture of its size and format. */
void rnd_copy(unsigned tex, int x, int y, int w, int h);
/* Copies all of target `src` (sw x sh) over all of `dst` (dw x dh),
 * nearest. */
void rnd_blit(unsigned src, unsigned dst, int sw, int sh, int dw, int dh);

/* The scissor, in the bound target's pixels. */
void rnd_scissor(int on, int x, int y, int w, int h);
void rnd_depth(int func, int write);
void rnd_cull(int mode);
/* Pulls faces a little toward the eye (glPolygonOffset(-1, -4)). */
void rnd_offset(int on);

void rnd_use(int program);
/* A uniform of the program in use; integers as floats. */
void rnd_uniform(int id, const float *v);
void rnd_texture_unit(int unit, unsigned tex);
/* Vertices for the program in use, which rnd_draw draws from. */
void rnd_stream(const float *verts, int count);
/* One whole pass's vertices: orphan the program's stream buffer, keeping
 * its capacity, and upload them once. Runs draw with rnd_draw(first,count).
 * Only the opt-in GLES DYN arena path calls this. */
void rnd_stream_pass(const float *verts, int count);
void rnd_draw(int prim, int first, int count);

/* MESH's vertex store: room for `count` vertices (emptied), and
 * vertices written into it. */
void rnd_mesh_store(int count);
void rnd_mesh_write(int first, int count, const float *verts);
/* Draws triangles from the store with MESH. */
void rnd_draw_mesh(int first, int count);

/* Ends the frame: the screen is shown (the Vita; on macOS the host
 * presents). */
void rnd_present(void);

/* 1 when a copy is dear (GXM: it ends the scene and starts two), so
 * the faces that read a copy share one where they can. */
int rnd_batch_copies(void);

/* The texture's size. */
void rnd_texture_size(unsigned tex, int *w, int *h);

#endif /* IGN_RND_H */
