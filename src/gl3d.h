/*
 * gl3d.h - draws the scene's objects on the GPU (rnd.h), in place of the
 * original's software pipeline and rasteriser (0x4466d0, 0x44f0e9).
 *
 * The meshes' faces become triangles with 8 bit colour indices; the
 * frame is drawn into an index buffer and read back over an 8 bit
 * screen, so it mixes with the 2D drawing as the original's did.
 */

#ifndef IGN_GL3D_H
#define IGN_GL3D_H

#include "scene.h"
#include "types.h"

/* Starts a frame of w x h pixels, empty. */
void gl3d_begin(int w, int h);
/* Draws objects as seen from `sc`, the original's view window placed
 * at (ox, oy) on the frame. */
void gl3d_draw(const struct scene *sc, struct scene_obj *const *objs, int count, double ox,
               double oy);
/* Starts the view: a frame of w x h pixels kept on the GPU that spans
 * vw x vh of the original's pixels, as video_set_layer shows it. */
void gl3d_view_begin(int w, int h, double vw, double vh);
/* The tables faces go through: the .SHD (sprites of type 7, faces 19
 * and 23) and type 8 sprites'. */
void gl3d_set_tables(const u8 *shd, const u8 *fixed);
/* Meshes from begin to end change each frame and are never cached;
 * up to eight such runs, each named by its begin. */
void gl3d_set_dynamic(const s32 *begin, const s32 *end);
/* Meshes from begin to end are effects, drawn by the particle pass
 * when EFFECTS is 3D (fx3d.h); up to four runs. */
void gl3d_set_particles(const s32 *begin, const s32 *end);
/* The particle layer of the view (RGBA8, premultiplied), 0 if none. */
unsigned gl3d_particle_texture(void);
void gl3d_view_fill(double y0, double y1, int index);
void gl3d_mesh_changed(const s32 *mesh);
void gl3d_view_resume(void);
void gl3d_view_columns(double x0, double x1);
void gl3d_view_end(void);
/* The view's colour indices: a texture (rnd.h), R, row 0 at the top. */
unsigned gl3d_view_texture(void);
/* Draws into a canvas surface of logical size lw x lh, over what it
 * holds, with a cleared depth buffer. Returns 0 if `key` is not one. */
int gl3d_surface_begin(u8 *key, int lw, int lh);
void gl3d_surface_end(void);
/* Copies what was drawn over `screen` (w x h at `pitch`). */
void gl3d_end(u8 *screen, int pitch);
/* A screen space command, as the original's rasteriser takes them
 * (0x44f0e9), in pixels and texels. A sprite (type 7) is given as two
 * type 18 triangles. */
enum {
    RASTER_FLAT = 15,           /* colour */
    RASTER_TEXTURE = 17,        /* page[v][u] */
    RASTER_TEXTURE_TABLE = 18,  /* table[page[v][u]][dst] */
    RASTER_FLAT_TABLE = 19      /* table[colour][dst] */
};

struct raster_cmd {
    int type;
    int page;                   /* in the page set */
    const u8 *table;            /* 256 x 256, [a << 8 | dst] */
    u8 colour;
    float x[3], y[3];           /* pixels */
    float u[3], v[3];           /* texels */
};

/* The pages raster commands index: `count` pages of 256 x 256 from
 * `pixels`. Kept until gl3d_flush. */
void gl3d_raster_pages(const u8 *pixels, int count);
/* Draws commands in order over an 8 bit target of w x h. */
void gl3d_raster(u8 *target, int w, int h, int pitch, const struct raster_cmd *cmds, int count);
/* Forgets the meshes and textures it holds, before their memory goes. */
void gl3d_flush(void);

#endif /* IGN_GL3D_H */
