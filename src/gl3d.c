/*
 * gl3d.c - the scene's objects on the GPU.
 *
 * The original projects each vertex as
 *     sx = cx - focal_x * zoom * Xc / (Zc + near)
 *     sy = cy - focal_y * Yc / (Zc + near)
 * with Xc, Yc, Zc the view coordinates (0x449e70), and draws the faces
 * back to front. Here the same projection is one matrix per object,
 * with w = Zc + near, and a depth buffer orders the faces. Faces are
 * clipped at the near plane, which the original does not do. The GPU
 * work goes through rnd.h.
 */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "gl3d.h"
#include "prof.h"
#include "rnd.h"
#include "soft3d/soft3d.h"

#if defined(IGN_GLES) && defined(__ANDROID__) && defined(IGN_GLES_DYN_ARENA) && \
    IGN_GLES_DYN_ARENA == 1
#define IGN_DYN_ARENA 1
#else
#define IGN_DYN_ARENA 0
#endif

#if defined(IGN_GLES) && defined(__ANDROID__) && defined(IGN_GLES_VISIBILITY_CULL) && \
    IGN_GLES_VISIBILITY_CULL == 1
#define IGN_VISIBILITY_CULL 1
#else
#define IGN_VISIBILITY_CULL 0
#endif

enum {
    MAX_MESHES = 1024,
    MAX_TEXTURES = 64,
    FLOATS = 8          /* x, y, z, u, v, page, colour, mode */
};

/* How a face's pixels come out. */
enum {
    MODE_TEXTURE = 0,   /* the texel */
    MODE_KEYED = 1,     /* the texel, colour 0 left out */
    MODE_FLAT = 2,      /* the face's colour */
    MODE_GRADIENT = 3   /* a colour from u, across a line */
};

struct mesh_entry {
    const s32 *mesh;
    int first;
    int count;
    int deferred;               /* has faces through a table */
#if IGN_VISIBILITY_CULL
    float lo[3], hi[3];         /* uploaded positions, for the draw cull */
    int bounds_valid;
#endif
};

struct texture_entry {
    const u8 *pixels;
    int pages;
    unsigned tex;
};

static int s_ready;
/* Render targets: 0 is read back over an 8 bit screen, 1 is the view
 * kept on the GPU. */
struct target {
    unsigned target, color;     /* RG8: the index, and 1 where drawn */
    int w, h;
};

static struct target s_targets[2];
static int s_w, s_h;            /* the target's size */
static double s_vw, s_vh;       /* the original's pixels it spans */
static double s_ox, s_oy;       /* where the original's (0, 0) is in them */
static struct mesh_entry s_meshes[MAX_MESHES];
static int s_mesh_count;
static int s_vbo_used, s_vbo_size;
static struct texture_entry s_textures[MAX_TEXTURES];
static int s_texture_count;
static u8 *s_readback;
static size_t s_readback_size;

/* Faces drawn after the rest, each frame anew: points and sprites
 * (0x44b5e0, 0x44d0f0, 0x44d230), whose meshes change as the effects
 * move, and faces through a table (19, 23: the .SHD; 0x44cb20), which
 * read the pixels under them. Kept as clip space vertices. */
enum { DYN_FLOATS = 9, MAX_DYN = 4096 };    /* x, y, z, w, u, v, page, colour, mode */

struct dyn {
    float v[6][DYN_FLOATS];
    int count;                  /* 3 or 6 vertices */
    float depth;                /* w of the middle, for the order */
    unsigned tex;
    const u8 *table;            /* NULL: drawn as it is */
#if IGN_SHARED_PAGES_BUILD
    unsigned pool_tex;          /* 0: use tex */
    int pool_layer;
#endif
};

static struct dyn s_dyns[MAX_DYN];
static const struct dyn *s_order[MAX_DYN];   /* far to near */
static int s_dyn_count;
static const u8 *s_tables_shd, *s_tables_fixed;     /* 0x63b5f0, 0x63c5f4 */
static struct { const s32 *begin, *end; } s_dyn_ranges[8];    /* meshes always drawn anew */
static int s_dyn_range_count;
static unsigned s_dyn_dst;      /* RG8, a copy of the target */
static int s_dyn_dst_w, s_dyn_dst_h;

#if IGN_SHARED_PAGES_BUILD
struct page_source {
    const u8 *pixels;
    int pages;
    int base;                   /* -1: this source is not pooled */
};

static struct page_source s_page_sources[MAX_TEXTURES];
static int s_page_source_count, s_page_used, s_page_layers;
static unsigned s_page_tex;
static int s_page_tried;
#endif

static unsigned table_get(const u8 *table);
static unsigned texture_get(int slot);

/* The raster path. */
enum { MAX_TABLES = 16, RASTER_FLOATS = 8 };     /* x, y, u, v, page, colour, mode, - */

struct table_entry {
    const u8 *table;
    unsigned tex;
};

static unsigned s_raster_target, s_raster_color, s_raster_dst;
static int s_raster_w, s_raster_h;
static unsigned s_raster_pages;
static struct table_entry s_tables[MAX_TABLES];
static int s_table_count;
static float *s_raster_verts;
static int s_raster_cap;

/* Dirty cells: what was drawn since the copy a table reads was taken.
 * A draw through a table that covers none of them can use the copy as
 * it is. */
enum { CELL = 16 };

static u8 *s_cells;
static int s_cells_w, s_cells_h, s_cells_cap;

static void cells_reset(int w, int h)
{
    s_cells_w = (w + CELL - 1) / CELL;
    s_cells_h = (h + CELL - 1) / CELL;
    if (s_cells_w * s_cells_h > s_cells_cap) {
        free(s_cells);
        s_cells_cap = s_cells_w * s_cells_h;
        s_cells = malloc((size_t)s_cells_cap);
    }
    memset(s_cells, 0, (size_t)s_cells_w * s_cells_h);
}

/* Marks (mark) or tests the pixels x0..x1, y0..y1 (r: x0, y0, x1, y1,
 * the end excluded, within the cells' frame). */
static int cells_touch(const int r[4], int mark)
{
    int x, y, cx0 = r[0] / CELL, cy0 = r[1] / CELL;
    int cx1 = (r[2] - 1) / CELL, cy1 = (r[3] - 1) / CELL;

    cx0 = cx0 < 0 ? 0 : cx0;
    cy0 = cy0 < 0 ? 0 : cy0;
    cx1 = cx1 >= s_cells_w ? s_cells_w - 1 : cx1;
    cy1 = cy1 >= s_cells_h ? s_cells_h - 1 : cy1;
    for (y = cy0; y <= cy1; y++) {
        for (x = cx0; x <= cx1; x++) {
            if (mark) {
                s_cells[y * s_cells_w + x] = 1;
            } else if (s_cells[y * s_cells_w + x]) {
                return 1;
            }
        }
    }
    return 0;
}

/* Layers, where copies are dear (GXM: rnd_batch_copies). A face through
 * a table reads what is under it, so it must come after everything
 * earlier that it overlaps has been written out: one layer above them.
 * Any other face only has to be drawn after the earlier faces it
 * overlaps: in the highest of their layers, after them. Drawn layer by
 * layer, each in the original order, with a copy before each layer but
 * the first, the frame is the same as with a copy before every face
 * through a table; but there is a copy a layer, not one a face.
 *
 * Faces overlap when their triangles share area: those that only meet
 * at an edge, as a mesh's neighbours do, do not (the GPU gives an edge's
 * pixels to one of them). A grid of cells finds the faces to test.
 *
 * Each layer costs GXM a scene, and a scene's end costs about 0.25 ms:
 * a pile of smoke or sparks made 90 of them a frame. Faces go no higher
 * than MAX_LAYER; one through a table there reads the frame from before
 * that layer, missing the faces drawn under it in the same layer. */
enum { LCELL = 32, MAX_LAYER = 5 };

struct geo {
    float x[6], y[6];           /* one or two triangles */
    int tris;
    int whole;                  /* behind the eye: all of the frame */
    int r[4];                   /* pixels x0, y0, x1, y1 */
};

static struct geo *s_geo;
static int *s_layers, *s_seen;
static int s_layers_cap, s_stamp;
static int *s_head, s_head_cap, s_gw, s_gh;
static struct { int face, next; } *s_nodes;
static int s_nodes_n, s_nodes_cap;

static int *layers_for(int count)
{
    if (count > s_layers_cap) {
        free(s_layers);
        free(s_seen);
        free(s_geo);
        s_layers_cap = count;
        s_layers = malloc((size_t)count * sizeof(*s_layers));
        s_seen = calloc((size_t)count, sizeof(*s_seen));
        s_geo = malloc((size_t)count * sizeof(*s_geo));
        s_stamp = 0;
    }
    return s_layers;
}

static void grid_reset(int w, int h)
{
    s_gw = (w + LCELL - 1) / LCELL;
    s_gh = (h + LCELL - 1) / LCELL;
    if (s_gw * s_gh > s_head_cap) {
        free(s_head);
        s_head_cap = s_gw * s_gh;
        s_head = malloc((size_t)s_head_cap * sizeof(*s_head));
    }
    memset(s_head, 0xff, (size_t)s_gw * s_gh * sizeof(*s_head));
    s_nodes_n = 0;
}

/* Projections of triangle t (x, y) onto the axis normal to edge a..b. */
static void project(const float *x, const float *y, float nx, float ny, float *lo, float *hi)
{
    int k;

    *lo = *hi = x[0] * nx + y[0] * ny;
    for (k = 1; k < 3; k++) {
        float p = x[k] * nx + y[k] * ny;

        *lo = p < *lo ? p : *lo;
        *hi = p > *hi ? p : *hi;
    }
}

/* Whether two triangles share area: no edge of either separates them. */
static int tris_overlap(const float *ax, const float *ay, const float *bx, const float *by)
{
    const float *tx[2] = { ax, bx }, *ty[2] = { ay, by };
    int t, k;

    for (t = 0; t < 2; t++) {
        for (k = 0; k < 3; k++) {
            float nx = ty[t][(k + 1) % 3] - ty[t][k], ny = tx[t][k] - tx[t][(k + 1) % 3];
            float alo, ahi, blo, bhi, eps;

            if (nx == 0.0f && ny == 0.0f) {
                continue;
            }
            project(ax, ay, nx, ny, &alo, &ahi);
            project(bx, by, nx, ny, &blo, &bhi);
            eps = 1e-3f * (fabsf(nx) + fabsf(ny));
            if (ahi <= blo + eps || bhi <= alo + eps) {
                return 0;
            }
        }
    }
    return 1;
}

static int faces_overlap(const struct geo *a, const struct geo *b)
{
    int i, j;

    if (a->whole || b->whole) {
        return 1;
    }
    if (a->r[2] <= b->r[0] || b->r[2] <= a->r[0] || a->r[3] <= b->r[1] || b->r[3] <= a->r[1]) {
        return 0;
    }
    for (i = 0; i < a->tris; i++) {
        for (j = 0; j < b->tris; j++) {
            if (tris_overlap(a->x + 3 * i, a->y + 3 * i, b->x + 3 * j, b->y + 3 * j)) {
                return 1;
            }
        }
    }
    return 0;
}

/* Face i's layer (s_geo[i], through a table or not), from the earlier
 * faces it overlaps; then it is entered in the grid. */
static int layer_of(int i, int through_table)
{
    const struct geo *g = &s_geo[i];
    int x, y, best = 0;
    int cx0 = g->r[0] / LCELL, cy0 = g->r[1] / LCELL;
    int cx1 = (g->r[2] - 1) / LCELL, cy1 = (g->r[3] - 1) / LCELL;

    if (g->whole) {
        cx0 = cy0 = 0;
        cx1 = s_gw - 1;
        cy1 = s_gh - 1;
    }
    cx0 = cx0 < 0 ? 0 : cx0;
    cy0 = cy0 < 0 ? 0 : cy0;
    cx1 = cx1 >= s_gw ? s_gw - 1 : cx1;
    cy1 = cy1 >= s_gh ? s_gh - 1 : cy1;
    if (++s_stamp == 0) {
        memset(s_seen, 0, (size_t)s_layers_cap * sizeof(*s_seen));
        s_stamp = 1;
    }
    for (y = cy0; y <= cy1; y++) {
        for (x = cx0; x <= cx1; x++) {
            int n;

            for (n = s_head[y * s_gw + x]; n >= 0; n = s_nodes[n].next) {
                int j = s_nodes[n].face;

                /* Only a face above the best so far can raise it. */
                if (s_seen[j] == s_stamp || s_layers[j] <= best) {
                    continue;
                }
                s_seen[j] = s_stamp;
                if (faces_overlap(g, &s_geo[j])) {
                    best = s_layers[j] > best ? s_layers[j] : best;
                }
            }
        }
    }
#if defined(__vita__) || defined(IGN_GXM)
    /* GXM: each layer costs a scene, so keep the upstream cap. */
    s_layers[i] = through_table && best < MAX_LAYER ? best + 1 : best;
#else
    /* Android GLES: the layer copies are the fallback that preserves the
     * table faces' dependencies, so do not merge them above MAX_LAYER. */
    s_layers[i] = through_table ? best + 1 : best;
#endif
    for (y = cy0; y <= cy1; y++) {
        for (x = cx0; x <= cx1; x++) {
            if (s_nodes_n == s_nodes_cap) {
                s_nodes_cap = s_nodes_cap ? s_nodes_cap * 2 : 4096;
                s_nodes = realloc(s_nodes, (size_t)s_nodes_cap * sizeof(*s_nodes));
            }
            s_nodes[s_nodes_n].face = i;
            s_nodes[s_nodes_n].next = s_head[y * s_gw + x];
            s_head[y * s_gw + x] = s_nodes_n++;
        }
    }
    return s_layers[i];
}

static void init(void)
{
    s_vbo_size = 1 << 16;
    rnd_mesh_store(s_vbo_size);
    s_ready = 1;
}

/* The size of a face of each type in bytes, 0 for the types that end a
 * mesh (the dispatch table at 0x49c8e0). */
static int face_size(int type)
{
    switch (type) {
    case 1: case 2: return 12;
    case 7: case 8: return 36;
    case 11: case 12: return 16;
    case 13: return 24;
    case 15: case 16: case 20: return 20;
    case 17: case 18: case 19: case 21: case 22: case 23: return 44;
    default: return 0;
    }
}

static void put(float *out, const s32 *mesh, s32 vi, float u, float v, float page,
                float colour, float mode)
{
    const s32 *p = mesh + 2 + 3 * vi;

    out[0] = (float)p[0];
    out[1] = (float)p[1];
    out[2] = (float)p[2];
    out[3] = u;
    out[4] = v;
    out[5] = page;
    out[6] = colour;
    out[7] = mode;
}

static void warn_once(int type)
{
    static u8 warned[256];

    if (!warned[type & 0xff]) {
        warned[type & 0xff] = 1;
        fprintf(stderr, "gl3d: face type %d not drawn yet\n", type);
    }
}

#if IGN_VISIBILITY_CULL
/* GLSL does not promise the CPU's association of a mat4 * vec4, nor
 * forbid contraction into an FMA. This envelope assumes ordinary IEEE
 * binary32 with round-to-nearest, FMA allowed, and optionally FTZ; it
 * does not cover arbitrary-precision arithmetic. 2^-24 is float's unit
 * roundoff, and eight covers every product and addition of any
 * association with room for a fused multiply-add. The bound is absolute,
 * from the sum of the term magnitudes, so it also survives cancellation.
 * Eight FLT_MIN cover a flushed product or addition result at each of
 * the four products and three additions. */
#define CLIP_DOT_UNIT 5.9604644775390625e-8
#define CLIP_DOT_ROUNDINGS 8.0
#define CLIP_DOT_FTZ_ALLOWANCE (8.0 * (double)FLT_MIN)

/* A flushed subnormal input may lose far more than its own magnitude
 * after multiplication by a large position, so it is outside the bound.
 * An s32 upload never produces a subnormal position, but direct bounds
 * are checked as well. */
static int clip_value_uncertain(float v)
{
    return !isfinite(v) || (v != 0.0f && fabsf(v) < FLT_MIN);
}

static int clip_inputs_uncertain(const struct mesh_entry *e, const float m[16])
{
    int i;

    for (i = 0; i < 16; i++) {
        if (clip_value_uncertain(m[i])) {
            return 1;
        }
    }
    for (i = 0; i < 3; i++) {
        if (clip_value_uncertain(e->lo[i]) || clip_value_uncertain(e->hi[i])) {
            return 1;
        }
    }
    return 0;
}

/* One row of m * (px, py, pz, 1) as the exact double sum, with a bound
 * on how far any float evaluation of that row can differ from it. */
static double clip_dot(const float m[16], int row, float px, float py, float pz,
                       float *error)
{
    double p0 = (double)m[row] * px;
    double p1 = (double)m[4 + row] * py;
    double p2 = (double)m[8 + row] * pz;
    double p3 = (double)m[12 + row];
    double sum = fabs(p0) + fabs(p1) + fabs(p2) + fabs(p3);

    /* Any float product or association can overflow once the absolute
     * terms sum above FLT_MAX, even when the relative error is finite.
     * A sum below FLT_MAX is not enough either: rounded products or a
     * different association can land above it, so the error must fit as
     * headroom too. */
    {
        double allowance = CLIP_DOT_ROUNDINGS * CLIP_DOT_UNIT * sum
                         + CLIP_DOT_FTZ_ALLOWANCE;

        *error = sum > FLT_MAX || sum + allowance > FLT_MAX
               ? INFINITY : (float)allowance;
    }
    return p0 + p1 + p2 + p3;
}

/* The enclosing box of the positions actually in the mesh's FLOATS8
 * upload. A mesh whose build produced no triangles has no usable box. */
static void mesh_bounds(const float *verts, int n, struct mesh_entry *e)
{
    int i, k;

    e->bounds_valid = n > 0;
    if (!e->bounds_valid) {
        return;
    }
    for (k = 0; k < 3; k++) {
        e->lo[k] = verts[k];
        e->hi[k] = verts[k];
    }
    for (i = 1; i < n; i++) {
        for (k = 0; k < 3; k++) {
            float p = verts[(size_t)i * FLOATS + k];

            e->lo[k] = p < e->lo[k] ? p : e->lo[k];
            e->hi[k] = p > e->hi[k] ? p : e->hi[k];
        }
    }
}

/* Whether any clip side has every corner of the uploaded box outside
 * it. Near-plane and behind-eye cases stay: only the four side planes
 * can reject a box without relying on the depth convention. */
static int mesh_visible(const struct mesh_entry *e, const float m[16])
{
    double corner[8][3];
    float error[8][3];
    int i, side, x, y, z;

    if (!e->bounds_valid || clip_inputs_uncertain(e, m)) {
        return 1;
    }
    i = 0;
    for (x = 0; x < 2; x++) {
        for (y = 0; y < 2; y++) {
            for (z = 0; z < 2; z++) {
                float px = x ? e->hi[0] : e->lo[0];
                float py = y ? e->hi[1] : e->lo[1];
                float pz = z ? e->hi[2] : e->lo[2];

                corner[i][0] = clip_dot(m, 0, px, py, pz, &error[i][0]);
                corner[i][1] = clip_dot(m, 1, px, py, pz, &error[i][1]);
                corner[i][2] = clip_dot(m, 3, px, py, pz, &error[i][2]);
                if (!isfinite(error[i][0]) || !isfinite(error[i][1])
                    || !isfinite(error[i][2])
                    || corner[i][2] <= (double)error[i][2]) {
                    return 1;
                }
                i++;
            }
        }
    }
    for (side = 0; side < 4; side++) {
        int outside = 1;

        for (i = 0; i < 8; i++) {
            double k = side == 0 ? corner[i][0] : side == 1 ? -corner[i][0]
                    : side == 2 ? corner[i][1] : -corner[i][1];
            double margin = (double)(side < 2 ? error[i][0] : error[i][1])
                          + (double)error[i][2];

            if (!(k > corner[i][2] + margin)) {
                outside = 0;
                break;
            }
        }
        if (outside) {
            return 0;
        }
    }
    return 1;
}
#endif

/* A mesh's faces as triangles into `verts`; returns their vertices. */
static int mesh_build(const s32 *mesh, float *verts, int *deferred_out)
{
    const u8 *f;
    int i, n = 0, deferred = 0;

    f = (const u8 *)(mesh + 2 + 3 * mesh[0]);
    for (i = 0; i < mesh[1]; i++) {
        const s32 *w = (const s32 *)f;
        int type = (s8)f[0];
        int size = face_size(type);
        float *o = verts + n * FLOATS;

        if (size == 0) {
            break;
        }
        if (type == 19 || type == 23 || type == 1 || type == 2 || type == 7 || type == 8
            || type == 13 || type == 20) {
            deferred = 1;
        } else if (size == 44 && type != 19 && type != 23) {
            float page = (float)((u32)w[10] >> 16);
            float mode = type == 18 || type == 22 ? MODE_KEYED : MODE_TEXTURE;
            int k;

            for (k = 0; k < 3; k++) {
                put(o + k * FLOATS, mesh, w[1 + k], w[4 + 2 * k] / 65536.0f,
                    w[5 + 2 * k] / 65536.0f, page, 0, mode);
            }
            n += 3;
        } else if (type == 15 || type == 16) {
            int k;

            for (k = 0; k < 3; k++) {
                put(o + k * FLOATS, mesh, w[1 + k], 0, 0, 0, (float)(w[4] & 0xff), MODE_FLAT);
            }
            n += 3;
        } else {
            warn_once(type);
        }
        f += size;
    }
    *deferred_out = deferred;
    return n;
}

/* Turns a mesh's faces into triangles, cached by the mesh's address. */
static struct mesh_entry *mesh_get(const s32 *mesh)
{
    struct mesh_entry *e;
    float *verts;
    int i, n, deferred;

    for (i = 0; i < s_mesh_count; i++) {
        if (s_meshes[i].mesh == mesh) {
            return &s_meshes[i];
        }
    }
    if (s_mesh_count == MAX_MESHES) {
        return NULL;
    }
    verts = malloc((size_t)mesh[1] * 3 * FLOATS * sizeof(float));
    n = mesh_build(mesh, verts, &deferred);
    if (s_vbo_used + n > s_vbo_size) {
        free(verts);
        return NULL;
    }
#if IGN_VISIBILITY_CULL
    mesh_bounds(verts, n, &s_meshes[s_mesh_count]);
#endif
    rnd_mesh_write(s_vbo_used, n, verts);
    free(verts);
    e = &s_meshes[s_mesh_count++];
    e->mesh = mesh;
    e->first = s_vbo_used;
    e->count = n;
    e->deferred = deferred;
    s_vbo_used += n;
    return e;
}

/* A cached mesh's faces changed (the animated textures move their
 * coordinates): its triangles are built again in place. */
void gl3d_mesh_changed(const s32 *mesh)
{
    float *verts;
    int i, n, deferred;

    for (i = 0; i < s_mesh_count; i++) {
        if (s_meshes[i].mesh == mesh) {
            break;
        }
    }
    if (i == s_mesh_count) {
        return;
    }
    verts = malloc((size_t)mesh[1] * 3 * FLOATS * sizeof(float));
    n = mesh_build(mesh, verts, &deferred);
    if (n == s_meshes[i].count) {
        rnd_mesh_write(s_meshes[i].first, n, verts);
#if IGN_VISIBILITY_CULL
        mesh_bounds(verts, n, &s_meshes[i]);
#endif
    }
    free(verts);
}

/* A texture slot's pages. */
static unsigned texture_get(int slot)
{
    const u8 *pixels;
    int pages, i;
    struct texture_entry *e;

    if (slot < 0 || slot >= SCENE_TEXTURES || g_scene_textures[slot] == NULL) {
        return 0;
    }
    pixels = g_scene_textures[slot];
    pages = g_scene_texture_pages[slot];
    for (i = 0; i < s_texture_count; i++) {
        if (s_textures[i].pixels == pixels && s_textures[i].pages == pages) {
            return s_textures[i].tex;
        }
    }
    if (s_texture_count == MAX_TEXTURES || pages <= 0) {
        return 0;
    }
    e = &s_textures[s_texture_count++];
    e->pixels = pixels;
    e->pages = pages;
    e->tex = rnd_pages(pixels, pages);
    return e->tex;
}

#if IGN_SHARED_PAGES_BUILD

/* A face may move to the shared array only when every vertex reads a
 * whole layer of its own source array. */
static int page_exact(const struct dyn *d, int pages)
{
    int k;

    if (pages <= 0) {
        return 0;
    }
    for (k = 0; k < d->count; k++) {
        float page = d->v[k][6];

        if (!(page >= 0.0f && page < (float)pages) || floorf(page) != page) {
            return 0;
        }
    }
    return 1;
}

static unsigned page_texture(void)
{
    if (!s_page_tried) {
        s_page_tried = 1;
        s_page_tex = rnd_page_pool_create(&s_page_layers);
    }
    return s_page_tex;
}

/* The layer where an admitted source starts, uploading it once. A source
 * that cannot be pooled stays out of the array, leaving its faces on
 * their original one. */
static int page_source(const u8 *pixels, int pages, int *base)
{
    struct page_source *e;
    int i;

    if (pixels == NULL || pages <= 0) {
        return 0;
    }
    for (i = 0; i < s_page_source_count; i++) {
        if (s_page_sources[i].pixels == pixels && s_page_sources[i].pages == pages) {
            *base = s_page_sources[i].base;
            return s_page_sources[i].base >= 0;
        }
    }
    if (page_texture() == 0 || s_page_source_count == MAX_TEXTURES) {
        return 0;
    }
    e = &s_page_sources[s_page_source_count++];
    e->pixels = pixels;
    e->pages = pages;
    e->base = -1;
    if (pages > s_page_layers - s_page_used) {
        return 0;
    }
    e->base = s_page_used;
    if (!rnd_page_pool_upload(s_page_tex, e->base, pixels, pages)) {
        e->base = -1;
        return 0;
    }
    s_page_used += pages;
    *base = e->base;
    return 1;
}

static void pages_reset(void)
{
    if (s_page_tex != 0) {
        rnd_page_pool_release(s_page_tex);
    }
    s_page_tex = 0;
    s_page_layers = s_page_used = s_page_source_count = 0;
    s_page_tried = 0;
}

static void pool_face(struct dyn *d, int slot, int all)
{
    int base;

    if (!all || !rnd_dyn_framebuffer_fetch()) {
        return;
    }
    if (d->tex == 0 || !page_exact(d, g_scene_texture_pages[slot])) {
        return;
    }
    if (page_source(g_scene_textures[slot], g_scene_texture_pages[slot], &base)) {
        d->pool_tex = s_page_tex;
        d->pool_layer = base;
    }
}

#endif

void gl3d_flush(void)
{
    int i;

    if (!s_ready) {
        return;
    }
#if IGN_SHARED_PAGES_BUILD
    pages_reset();
#endif
    for (i = 0; i < s_texture_count; i++) {
        rnd_texture_free(s_textures[i].tex);
    }
    s_texture_count = 0;
    for (i = 0; i < s_table_count; i++) {
        rnd_texture_free(s_tables[i].tex);
    }
    s_table_count = 0;
    if (s_raster_pages != 0) {
        rnd_texture_free(s_raster_pages);
        s_raster_pages = 0;
    }
    s_mesh_count = 0;
    s_vbo_used = 0;
}

static void target_begin(struct target *t, int w, int h)
{
    static const float zero[4] = { 0, 0, 0, 0 };

    if (!s_ready) {
        init();
    }
    if (w != t->w || h != t->h) {
        if (t->target != 0) {
            rnd_target_free(t->target);
            rnd_texture_free(t->color);
        }
        t->color = rnd_texture(RND_RG8, w, h, NULL, 0);
        t->target = rnd_target(t->color, 1);
        t->w = w;
        t->h = h;
    }
    rnd_bind(t->target);
    s_w = w;
    s_h = h;
    s_ox = s_oy = 0;
    rnd_clear(zero, 1);
}

void gl3d_begin(int w, int h)
{
    target_begin(&s_targets[0], w, h);
    s_vw = w;
    s_vh = h;
    if (s_readback_size < (size_t)w * h * 2) {
        free(s_readback);
        s_readback_size = (size_t)w * h * 2;
        s_readback = malloc(s_readback_size);
    }
}

void gl3d_view_begin(int w, int h, double vw, double vh)
{
    target_begin(&s_targets[1], w, h);
    s_vw = vw;
    s_vh = vh;
}

/* Fills rows y0..y1 of the view (in its virtual rows), across its whole
 * width, with a palette index, as the original fills the back buffer
 * before drawing into it. */
void gl3d_view_fill(double y0, double y1, int index)
{
    int r0 = (int)(y0 * s_h / s_vh + 0.5), r1 = (int)(y1 * s_h / s_vh + 0.5);

    float c[4] = { 0, 1.0f, 0, 0 };

    if (r1 <= r0) {
        return;
    }
    c[0] = (float)(index & 0xff) / 255.0f;
    rnd_scissor(1, 0, r0, s_w, r1 - r0);
    rnd_clear(c, 0);
    rnd_scissor(0, 0, 0, 0, 0);
}

/* Binds the view again, keeping what is drawn (a second view in the
 * same frame). */
void gl3d_view_resume(void)
{
    struct target *t = &s_targets[1];

    rnd_bind(t->target);
    s_w = t->w;
    s_h = t->h;
    s_ox = s_oy = 0;
}

/* Limits drawing to the view's columns x0..x1 (its virtual columns);
 * x0 >= x1 lifts the limit. */
void gl3d_view_columns(double x0, double x1)
{
    int c0, c1;

    if (x0 >= x1) {
        rnd_scissor(0, 0, 0, 0, 0);
        return;
    }
    c0 = (int)(x0 * s_w / s_vw + 0.5);
    c1 = (int)(x1 * s_w / s_vw + 0.5);
    c0 = c0 < 0 ? 0 : c0;
    c1 = c1 > s_w ? s_w : c1;
    rnd_scissor(1, c0, 0, c1 > c0 ? c1 - c0 : 0, s_h);
}

/* The next draw binds its own target. */
void gl3d_view_end(void)
{
}

unsigned gl3d_view_texture(void)
{
    return s_targets[1].color;
}

/* Mesh coordinates to clip space for one object. Rows 0 and 1 map to
 * pixels from the frame's top left (GL's row 0 is the screen's first
 * row, so the read back needs no flip); w is Zc + near. */
static void object_matrix(const struct scene *sc, const struct scene_obj *o, double ox,
                          double oy, float out[16])
{
    double view[3][3], rot[3][3], m[3][3], t[3], clip[4][4];
    double n = sc->near_ofs, near_w = 1.0, far_w = 200000.0;
    double ax = -2.0 * sc->focal_x * sc->zoom / s_vw, bx = 2.0 * (ox + s_ox + sc->center_x) / s_vw - 1.0;
    double ay = -2.0 * sc->focal_y / s_vh, by = 2.0 * (oy + s_oy + sc->center_y) / s_vh - 1.0;
    double az = (far_w + near_w) / (far_w - near_w), bz = -2.0 * far_w * near_w / (far_w - near_w);
    double d[3];
    int i, j, k;

    scene_view_rows(sc, view);
    if (o->ax == 0 && o->ay == 0 && o->az == 0) {
        memset(rot, 0, sizeof(rot));
        rot[0][0] = 1;
        rot[1][1] = -1;
        rot[2][2] = 1;
    } else {
        scene_obj_rows(o, rot);
    }
    d[0] = o->x - sc->x;
    d[1] = o->y - sc->y;
    d[2] = o->z - sc->z;
    for (i = 0; i < 3; i++) {
        t[i] = 0;
        for (j = 0; j < 3; j++) {
            m[i][j] = 0;
            for (k = 0; k < 3; k++) {
                m[i][j] += view[i][k] * rot[k][j];
            }
            t[i] += view[i][j] * d[j];
        }
    }
    /* view (m v + t) -> clip */
    for (j = 0; j < 3; j++) {
        clip[0][j] = ax * m[0][j] + bx * m[2][j];
        clip[1][j] = ay * m[1][j] + by * m[2][j];
        clip[2][j] = az * m[2][j];
        clip[3][j] = m[2][j];
    }
    clip[0][3] = ax * t[0] + bx * (t[2] + n);
    clip[1][3] = ay * t[1] + by * (t[2] + n);
    clip[2][3] = az * (t[2] + n) + bz;
    clip[3][3] = t[2] + n;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            out[j * 4 + i] = (float)clip[i][j];
        }
    }
}

void gl3d_set_tables(const u8 *shd, const u8 *fixed)
{
    s_tables_shd = shd;
    s_tables_fixed = fixed;
}

void gl3d_set_dynamic(const s32 *begin, const s32 *end)
{
    int i;

    for (i = 0; i < s_dyn_range_count; i++) {
        if (s_dyn_ranges[i].begin == begin) {
            s_dyn_ranges[i].end = end;
            return;
        }
    }
    if (s_dyn_range_count < 8) {
        s_dyn_ranges[s_dyn_range_count].begin = begin;
        s_dyn_ranges[s_dyn_range_count].end = end;
        s_dyn_range_count++;
    }
}

static int is_dynamic(const s32 *mesh)
{
    int i;

    for (i = 0; i < s_dyn_range_count; i++) {
        if (mesh >= s_dyn_ranges[i].begin && mesh < s_dyn_ranges[i].end) {
            return 1;
        }
    }
    return 0;
}

static void clip_of(const float m[16], const s32 *v, float c[4])
{
    int k;

    for (k = 0; k < 4; k++) {
        c[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k];
    }
}

static void dyn_vertex(float *o, const float c[4], float u, float v, float page, float colour,
                       float mode)
{
    o[0] = c[0];
    o[1] = c[1];
    o[2] = c[2];
    o[3] = c[3];
    o[4] = u;
    o[5] = v;
    o[6] = page;
    o[7] = colour;
    o[8] = mode;
}

/* A screen box around clip point c, half hx, hy in NDC, kept at c's
 * depth: two triangles. */
static void dyn_box(struct dyn *d, const float c[4], float hx, float hy, const float uv[4],
                    float page, float colour, float mode)
{
    float x0 = c[0] / c[3] - hx, x1 = c[0] / c[3] + hx, y0 = c[1] / c[3] - hy, y1 = c[1] / c[3] + hy;
    float z = c[2] / c[3];
    float p[4][4] = { { x0, y0, z, 1 }, { x1, y0, z, 1 }, { x0, y1, z, 1 }, { x1, y1, z, 1 } };

    dyn_vertex(d->v[0], p[0], uv[0], uv[1], page, colour, mode);
    dyn_vertex(d->v[1], p[1], uv[2], uv[1], page, colour, mode);
    dyn_vertex(d->v[2], p[2], uv[0], uv[3], page, colour, mode);
    dyn_vertex(d->v[3], p[1], uv[2], uv[1], page, colour, mode);
    dyn_vertex(d->v[4], p[3], uv[2], uv[3], page, colour, mode);
    dyn_vertex(d->v[5], p[2], uv[0], uv[3], page, colour, mode);
    d->count = 6;
}

/* An object's faces for the deferred pass: all of a dynamic mesh's, or
 * a static mesh's table faces. Points and sprites are placed as the
 * rasteriser places them on the screen (8.8 sprite sizes of 0x44d0f0
 * and 0x44d230 through 0x455648's box). */
static void collect(const struct scene *sc, const struct scene_obj *o, const float m[16],
                    int all)
{
    const s32 *mesh = o->mesh;
    const u8 *f = (const u8 *)(mesh + 2 + 3 * mesh[0]);
    unsigned tex = 0;
    int i, have_tex = 0;

    for (i = 0; i < mesh[1] && s_dyn_count < MAX_DYN; i++) {
        const s32 *w = (const s32 *)f;
        int type = (s8)f[0], size = face_size(type);
        struct dyn *d = &s_dyns[s_dyn_count];
        float c[3][4];
        int k;

        if (size == 0) {
            break;
        }
        f += size;
        if (!all && type != 19 && type != 23 && type != 1 && type != 2 && type != 7 && type != 8
            && type != 13 && type != 20) {
            continue;
        }
        if (!have_tex) {
            tex = texture_get(o->tex);
            have_tex = 1;
        }
        d->tex = tex;
        d->table = NULL;
#if IGN_SHARED_PAGES_BUILD
        d->pool_tex = 0;
        d->pool_layer = 0;
#endif
        if (type == 1 || type == 2) {
            float h = type == 1 ? 0.5f : 1.0f, uv[4] = { 0, 0, 0, 0 };

            clip_of(m, mesh + 2 + 3 * w[1], c[0]);
            /* The original leaves out what is within 200 of the eye. */
            if (c[0][3] <= 200.0f) {
                continue;
            }
            dyn_box(d, c[0], h * 2.0f / (float)s_vw, h * 2.0f / (float)s_vh, uv, 0,
                    (float)(w[2] & 0xff), MODE_FLAT);
            d->depth = c[0][3];
        } else if (type == 7 || type == 8) {
            double hot_x = (double)(w[4] + w[2]) / 2 - w[2], hot_y = (double)(w[5] + w[3]) / 2 - w[3];
            float uv[4] = { w[2] / 65536.0f, w[3] / 65536.0f, w[4] / 65536.0f, w[5] / 65536.0f };
            s32 sx, sy;

            clip_of(m, mesh + 2 + 3 * w[1], c[0]);
            if (c[0][3] <= 200.0f || tex == 0) {
                continue;
            }
            if (type == 7) {
                double k4 = 4.0 / (s32)c[0][3];

                sx = (s32)(w[7] * k4);
                sy = (s32)(w[8] * k4);
                d->table = s_tables_shd;
            } else {
                double px, py;

                sx = (s32)(0.003952569328248501 * sc->focal_y * w[7]);
                sy = (s32)(sc->focal_y * 0.004820206304829848 * w[8]);
                d->table = s_tables_fixed;
                /* Type 8 sits on a whole pixel. */
                px = floor((c[0][0] / c[0][3] + 1.0) * s_vw / 2);
                py = floor((c[0][1] / c[0][3] + 1.0) * s_vh / 2);
                c[0][0] = (float)((px * 2 / s_vw - 1.0) * c[0][3]);
                c[0][1] = (float)((py * 2 / s_vh - 1.0) * c[0][3]);
            }
            /* hot (8.8 texels) * scale >> 16 is 8.8 pixels. */
            dyn_box(d, c[0], (float)(hot_x * sx / 16777216.0 * 2.0 / s_vw),
                    (float)(hot_y * sy / 16777216.0 * 2.0 / s_vh), uv, (float)((u32)w[6] >> 16), 0,
                    MODE_KEYED);
            d->depth = c[0][3];
        } else if (type == 13) {
            /* A line a pixel wide (0x44d470), its colour running from w[3]
             * to w[4] (8.8), through the .SHD when w[5] is set (the
             * original keeps the table's address there). */
            float a[3], b[3], dx, dy, len, nx, ny;
            float p[4][4];

            clip_of(m, mesh + 2 + 3 * w[1], c[0]);
            clip_of(m, mesh + 2 + 3 * w[2], c[1]);
            if (c[0][3] + c[1][3] <= 400.0f || c[0][3] <= 1.0f || c[1][3] <= 1.0f) {
                continue;
            }
            for (k = 0; k < 3; k++) {
                a[k] = c[0][k] / c[0][3];
                b[k] = c[1][k] / c[1][3];
            }
            dx = (b[0] - a[0]) * (float)s_vw;
            dy = (b[1] - a[1]) * (float)s_vh;
            len = sqrtf(dx * dx + dy * dy);
            if (len < 1e-6f) {
                nx = 1.0f / (float)s_vw;
                ny = 0;
            } else {
                nx = -dy / len / (float)s_vw;
                ny = dx / len / (float)s_vh;
            }
            p[0][0] = a[0] - nx; p[0][1] = a[1] - ny; p[0][2] = a[2]; p[0][3] = 1;
            p[1][0] = a[0] + nx; p[1][1] = a[1] + ny; p[1][2] = a[2]; p[1][3] = 1;
            p[2][0] = b[0] - nx; p[2][1] = b[1] - ny; p[2][2] = b[2]; p[2][3] = 1;
            p[3][0] = b[0] + nx; p[3][1] = b[1] + ny; p[3][2] = b[2]; p[3][3] = 1;
            dyn_vertex(d->v[0], p[0], w[3] / 256.0f, 0, 0, 0, MODE_GRADIENT);
            dyn_vertex(d->v[1], p[1], w[3] / 256.0f, 0, 0, 0, MODE_GRADIENT);
            dyn_vertex(d->v[2], p[2], w[4] / 256.0f, 0, 0, 0, MODE_GRADIENT);
            dyn_vertex(d->v[3], p[1], w[3] / 256.0f, 0, 0, 0, MODE_GRADIENT);
            dyn_vertex(d->v[4], p[3], w[4] / 256.0f, 0, 0, 0, MODE_GRADIENT);
            dyn_vertex(d->v[5], p[2], w[4] / 256.0f, 0, 0, 0, MODE_GRADIENT);
            d->count = 6;
            d->depth = (c[0][3] + c[1][3]) * 0.5f;
            d->table = w[5] != 0 ? s_tables_shd : NULL;
        } else if (size == 44 || type == 15 || type == 16 || type == 20) {
            int flat = type == 15 || type == 16 || type == 20 || type == 21;
            float page = flat ? 0 : (float)((u32)w[10] >> 16);
            float mode = flat ? MODE_FLAT : (type == 18 || type == 22) ? MODE_KEYED : MODE_TEXTURE;
            float colour = flat ? (float)(w[4] & 0xff) : 0;

            for (k = 0; k < 3; k++) {
                clip_of(m, mesh + 2 + 3 * w[1 + k], c[k]);
            }
            if (c[0][3] <= 1.0f && c[1][3] <= 1.0f && c[2][3] <= 1.0f) {
                continue;
            }
            /* Type 20 (0x44cf00): a flat face through the .SHD, left out
             * near the eye (its three depths up to 600). */
            if (type == 20 && c[0][3] + c[1][3] + c[2][3] <= 600.0f) {
                continue;
            }
            if (!flat && tex == 0) {
                continue;
            }
            for (k = 0; k < 3; k++) {
                dyn_vertex(d->v[k], c[k], flat ? 0 : w[4 + 2 * k] / 65536.0f,
                           flat ? 0 : w[5 + 2 * k] / 65536.0f, page, colour, mode);
            }
            d->count = 3;
            d->depth = (c[0][3] + c[1][3] + c[2][3]) / 3.0f;
            if (type == 19 || type == 23 || type == 20) {
                d->table = s_tables_shd;
            }
        } else {
            warn_once(type);
            continue;
        }
#if IGN_SHARED_PAGES_BUILD
        pool_face(d, o->tex, all);
#endif
        s_dyn_count++;
    }
}

static int dyn_far_first(const void *a, const void *b)
{
    const struct dyn *p = *(const struct dyn *const *)a, *q = *(const struct dyn *const *)b;

    /* Equal depths keep the order the faces came in. */
    if (p->depth != q->depth) {
        return p->depth < q->depth ? 1 : -1;
    }
    return p < q ? -1 : p > q ? 1 : 0;
}

#if IGN_VISIBILITY_CULL
/* A face is rejected only when every vertex is strictly outside one of
 * the four side planes. Behind the eye, touching or crossing a plane,
 * and non-finite clip coordinates keep it. */
static int dyn_visible(const struct dyn *d)
{
    int side, k;

    for (side = 0; side < 4; side++) {
        int outside = 1;

        for (k = 0; k < d->count; k++) {
            float x = d->v[k][0], y = d->v[k][1], w = d->v[k][3];
            float value, margin;

            if (!isfinite(x) || !isfinite(y) || !isfinite(w) || w <= 0.0f) {
                outside = 0;
                break;
            }
            value = side == 0 ? x : side == 1 ? -x : side == 2 ? y : -y;
            margin = 1e-4f * (fabsf(value) + fabsf(w) + 1.0f);
            if (!(value > w + margin)) {
                outside = 0;
                break;
            }
        }
        if (outside) {
            return 0;
        }
    }
    return 1;
}
#endif

/* The pixels a face covers, from its clip space vertices. */
static int dyn_rect(const struct dyn *d, int r[4])
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int k;

    for (k = 0; k < d->count; k++) {
        float w = d->v[k][3], x, y;

        if (w <= 0.0f) {
            /* Behind the eye: take the whole frame. */
            x0 = y0 = -1;
            x1 = y1 = 1;
            break;
        }
        x = d->v[k][0] / w;
        y = d->v[k][1] / w;
        x0 = x < x0 ? x : x0;
        x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0;
        y1 = y > y1 ? y : y1;
    }
    r[0] = (int)floor((x0 + 1.0f) * 0.5f * s_w) - 1;
    r[1] = (int)floor((y0 + 1.0f) * 0.5f * s_h) - 1;
    r[2] = (int)ceil((x1 + 1.0f) * 0.5f * s_w) + 1;
    r[3] = (int)ceil((y1 + 1.0f) * 0.5f * s_h) + 1;
    r[0] = r[0] < 0 ? 0 : r[0];
    r[1] = r[1] < 0 ? 0 : r[1];
    r[2] = r[2] > s_w ? s_w : r[2];
    r[3] = r[3] > s_h ? s_h : r[3];
    return r[2] > r[0] && r[3] > r[1];
}

/* Faces gathered for one draw: their vertices, one after the other. */
static float *s_batch;
static int s_batch_cap;

static unsigned batch_texture(const struct dyn *d)
{
#if IGN_SHARED_PAGES_BUILD
    if (d->pool_tex != 0) {
        return d->pool_tex;
    }
#endif
    return d->tex;
}

static int same_batch(const struct dyn *a, const struct dyn *b)
{
    return a->table == b->table && batch_texture(a) == batch_texture(b);
}

static int batch_add(int n, const struct dyn *d)
{
#if IGN_SHARED_PAGES_BUILD
    int i;
#endif

    if (n + d->count > s_batch_cap) {
        s_batch_cap = (n + d->count) * 2;
        s_batch = realloc(s_batch, (size_t)s_batch_cap * DYN_FLOATS * sizeof(float));
    }
    memcpy(s_batch + (size_t)n * DYN_FLOATS, d->v, (size_t)d->count * DYN_FLOATS * sizeof(float));
#if IGN_SHARED_PAGES_BUILD
    if (d->pool_tex != 0) {
        for (i = 0; i < d->count; i++) {
            s_batch[(size_t)(n + i) * DYN_FLOATS + 6] += (float)d->pool_layer;
        }
    }
#endif
    return n + d->count;
}

/* n vertices of faces that share first's texture and table. */
static void draw_batch(const struct dyn *first, int n)
{
    int use_table = first->table != NULL;
    float u = (float)use_table;

    if (use_table) {
        rnd_texture_unit(2, s_dyn_dst);
        rnd_texture_unit(1, table_get(first->table));
    }
    rnd_depth(RND_DEPTH_LEQUAL, !use_table);
    rnd_offset(use_table);
    rnd_texture_unit(0, batch_texture(first));
    rnd_uniform(RND_U_USE_TABLE, &u);
    rnd_stream(s_batch, n);
    rnd_draw(RND_TRIANGLES, 0, n);
}

#if IGN_DYN_ARENA
/* A run's place in the pass's one vertex block. */
struct dyn_run {
    const struct dyn *first;
    int at;
    int count;
};

static struct dyn_run s_runs[MAX_DYN];

static int batch_reserve(int need)
{
    if (need > s_batch_cap) {
        int cap = need * 2;
        float *p = realloc(s_batch, (size_t)cap * DYN_FLOATS * sizeof(float));

        if (p == NULL) {
            return 0;
        }
        s_batch = p;
        s_batch_cap = cap;
    }
    return 1;
}

/* `at` is where the pass's runs so far end; n counts this run. -1 when
 * the staging block cannot grow; the pass then falls back. */
static int batch_add_pass(int at, int n, const struct dyn *d)
{
    if (!batch_reserve(at + n + d->count)) {
        return -1;
    }
    return batch_add(at + n, d) - at;
}

/* A run from the pass's already uploaded block. */
static void draw_batch_at(const struct dyn *first, int at, int n)
{
    int use_table = first->table != NULL;
    float u = (float)use_table;

    if (use_table) {
        rnd_texture_unit(2, s_dyn_dst);
        rnd_texture_unit(1, table_get(first->table));
    }
    rnd_depth(RND_DEPTH_LEQUAL, !use_table);
    rnd_offset(use_table);
    rnd_texture_unit(0, batch_texture(first));
    rnd_uniform(RND_U_USE_TABLE, &u);
    rnd_draw(RND_TRIANGLES, at, n);
}

/* Staging allocation failed: each face streams as before, so none is
 * dropped and none reads another face's vertices. */
static void draw_one(const struct dyn *d)
{
    int use_table = d->table != NULL;
    float u = (float)use_table;

    if (use_table) {
        rnd_texture_unit(2, s_dyn_dst);
        rnd_texture_unit(1, table_get(d->table));
    }
    rnd_depth(RND_DEPTH_LEQUAL, !use_table);
    rnd_offset(use_table);
    rnd_texture_unit(0, d->tex);
    rnd_uniform(RND_U_USE_TABLE, &u);
    rnd_stream(&d->v[0][0], d->count);
    rnd_draw(RND_TRIANGLES, 0, d->count);
}
#endif

/* Back to front. With framebuffer fetch the table reads the current
 * target, so faces of one material in that order share a draw and no
 * copy is made. Otherwise each reads a copy of what is under it; where
 * copies are dear, one copy of the whole frame serves the faces after
 * it until one of them covers what was drawn since. */
static void draw_dyns(void)
{
    int i, copied = 0, batch = rnd_batch_copies();

    if (s_dyn_count == 0) {
        return;
    }
#if IGN_VISIBILITY_CULL
    /* Admission has already happened: the same first MAX_DYN faces are
     * considered. Compact in place so equal depths keep their order. */
    {
        int kept = 0;

        for (i = 0; i < s_dyn_count; i++) {
            if (dyn_visible(&s_dyns[i])) {
                if (kept != i) {
                    s_dyns[kept] = s_dyns[i];
                }
                kept++;
            }
        }
        s_dyn_count = kept;
    }
    if (s_dyn_count == 0) {
        return;
    }
#endif
    /* The faces' order, sorted rather than the faces themselves, which
     * are big. */
    prof_begin(PROF_SORT);
    for (i = 0; i < s_dyn_count; i++) {
        s_order[i] = &s_dyns[i];
    }
    qsort(s_order, (size_t)s_dyn_count, sizeof(s_order[0]), dyn_far_first);
    prof_end(PROF_SORT);
    rnd_use(RND_DYN);
    rnd_cull(RND_CULL_OFF);
    if (rnd_dyn_framebuffer_fetch()) {
#if IGN_DYN_ARENA
        int total = 0, runs = 0, failed = 0;

        for (i = 0; i < s_dyn_count;) {
            int j, n = 0;

            for (j = i; j < s_dyn_count; j++) {
                const struct dyn *d = s_order[j];

                if (!same_batch(d, s_order[i])) {
                    break;
                }
                n = batch_add_pass(total, n, d);
                if (n < 0) {
                    failed = 1;
                    break;
                }
            }
            if (failed) {
                break;
            }
            s_runs[runs].first = s_order[i];
            s_runs[runs].at = total;
            s_runs[runs].count = n;
            runs++;
            total += n;
            i = j;
        }
        if (!failed) {
            int r;

            rnd_stream_pass(s_batch, total);
            for (r = 0; r < runs; r++) {
                draw_batch_at(s_runs[r].first, s_runs[r].at, s_runs[r].count);
            }
        } else {
            for (i = 0; i < s_dyn_count; i++) {
                draw_one(s_order[i]);
            }
        }
#else
        for (i = 0; i < s_dyn_count;) {
            int j, n = 0;

            for (j = i; j < s_dyn_count; j++) {
                const struct dyn *d = s_order[j];

                if (!same_batch(d, s_order[i])) {
                    break;
                }
                n = batch_add(n, d);
            }
            draw_batch(s_order[i], n);
            i = j;
        }
#endif
        rnd_offset(0);
        rnd_depth(RND_DEPTH_OFF, 1);
        s_dyn_count = 0;
        return;
    }
    if (s_dyn_dst == 0 || s_dyn_dst_w != s_w || s_dyn_dst_h != s_h) {
        if (s_dyn_dst != 0) {
            rnd_texture_free(s_dyn_dst);
        }
        s_dyn_dst = rnd_texture(RND_RG8, s_w, s_h, NULL, 0);
        s_dyn_dst_w = s_w;
        s_dyn_dst_h = s_h;
    }
    cells_reset(s_w, s_h);
    if (batch) {
        int *layer = layers_for(s_dyn_count), top = 0, l;

        prof_begin(PROF_LAYERS);
        grid_reset(s_w, s_h);
        for (i = 0; i < s_dyn_count; i++) {
            const struct dyn *d = s_order[i];
            struct geo *g = &s_geo[i];
            int k;

            if (!dyn_rect(d, g->r)) {
                /* Off the frame: through a table it is not drawn. */
                layer[i] = d->table != NULL ? -1 : 0;
                continue;
            }
            g->whole = 0;
            g->tris = d->count / 3;
            for (k = 0; k < d->count; k++) {
                float w = d->v[k][3];

                if (w <= 0.0f) {
                    g->whole = 1;
                    break;
                }
                g->x[k] = (d->v[k][0] / w + 1.0f) * 0.5f * s_w;
                g->y[k] = (d->v[k][1] / w + 1.0f) * 0.5f * s_h;
            }
            layer_of(i, d->table != NULL);
            top = layer[i] > top ? layer[i] : top;
        }
        prof_end(PROF_LAYERS);
        for (l = 0; l <= top; l++) {
            if (l > 0) {
                rnd_copy(s_dyn_dst, 0, 0, s_w, s_h);
            }
            /* The layer's faces in order, those in a row that share a
             * texture and a table in one draw. */
            for (i = 0; i < s_dyn_count; i++) {
                int j, n;

                if (layer[i] != l) {
                    continue;
                }
                n = 0;
                for (j = i; j < s_dyn_count; j++) {
                    const struct dyn *d = s_order[j];

                    if (layer[j] != l) {
                        continue;
                    }
                    if (!same_batch(d, s_order[i])) {
                        break;
                    }
                    n = batch_add(n, d);
                    layer[j] = -2;
                }
                draw_batch(s_order[i], n);
            }
        }
        rnd_offset(0);
        rnd_depth(RND_DEPTH_OFF, 1);
        s_dyn_count = 0;
        return;
    }
    for (i = 0; i < s_dyn_count; i++) {
        const struct dyn *d = s_order[i];
        int use_table = d->table != NULL, r[4], on;
        float u = (float)use_table;

        on = dyn_rect(d, r);
        if (use_table) {
            if (!on) {
                continue;
            }
            if (!batch) {
                rnd_copy(s_dyn_dst, r[0], r[1], r[2] - r[0], r[3] - r[1]);
            } else if (!copied || cells_touch(r, 0)) {
                rnd_copy(s_dyn_dst, 0, 0, s_w, s_h);
                cells_reset(s_w, s_h);
                copied = 1;
            }
            rnd_texture_unit(2, s_dyn_dst);
            rnd_texture_unit(1, table_get(d->table));
        }
        /* Through a table nothing writes the depth, as the faces show what
         * is behind them; and they are pulled a little toward the eye, as
         * the shadows and skids lie on the ground they shade. */
        rnd_depth(RND_DEPTH_LEQUAL, !use_table);
        rnd_offset(use_table);
        rnd_texture_unit(0, d->tex);
        rnd_uniform(RND_U_USE_TABLE, &u);
        rnd_stream(&d->v[0][0], d->count);
        rnd_draw(RND_TRIANGLES, 0, d->count);
        if (on) {
            cells_touch(r, 1);
        }
    }
    rnd_offset(0);
    rnd_depth(RND_DEPTH_OFF, 1);
    s_dyn_count = 0;
}

void gl3d_draw(const struct scene *sc, struct scene_obj *const *objs, int count, double ox,
               double oy)
{
    prof_begin(PROF_MESHES);
    int i;

    rnd_use(RND_MESH);
    rnd_depth(RND_DEPTH_LESS, 1);
    /* The front faces are clockwise when the view is not mirrored. */
    rnd_cull(sc->zoom > 0 ? RND_CULL_CCW : RND_CULL_CW);
    for (i = 0; i < count; i++) {
        const struct scene_obj *o = objs[i];
        struct mesh_entry *e;
        float m[16];

        if (is_dynamic(o->mesh)) {
            object_matrix(sc, o, ox, oy, m);
            collect(sc, o, m, 1);
            continue;
        }
        e = mesh_get(o->mesh);
        if (e == NULL) {
            continue;
        }
        object_matrix(sc, o, ox, oy, m);
        if (e->deferred) {
            collect(sc, o, m, 0);
        }
        if (e->count == 0) {
            continue;
        }
        rnd_use(RND_MESH);
        rnd_texture_unit(0, texture_get(o->tex));
#if IGN_VISIBILITY_CULL
        /* texture_get has already run, so the cache fills exactly as the
         * unculled path would; only this face's own draw is skipped. */
        if (!mesh_visible(e, m)) {
            continue;
        }
#endif
        rnd_uniform(RND_U_MATRIX, m);
        rnd_draw_mesh(e->first, e->count);
    }
    rnd_cull(RND_CULL_OFF);
    rnd_depth(RND_DEPTH_OFF, 1);
    prof_end(PROF_MESHES);
    prof_begin(PROF_DYNS);
    draw_dyns();
    prof_end(PROF_DYNS);
}

void gl3d_end(u8 *screen, int pitch)
{
    int x, y;

    rnd_read(0, 0, s_w, s_h, RND_RG8, s_readback, s_w * 2);
    for (y = 0; y < s_h; y++) {
        const u8 *src = s_readback + (size_t)y * s_w * 2;
        u8 *dst = screen + (size_t)y * pitch;

        for (x = 0; x < s_w; x++) {
            if (src[2 * x + 1] != 0) {
                dst[x] = src[2 * x];
            }
        }
    }
}

/* Raster commands: pixels in, the index out (RND_RASTER). */

static unsigned table_get(const u8 *table)
{
    struct table_entry *e;
    int i;

    for (i = 0; i < s_table_count; i++) {
        if (s_tables[i].table == table) {
            return s_tables[i].tex;
        }
    }
    if (s_table_count == MAX_TABLES) {
        return s_tables[0].tex;
    }
    e = &s_tables[s_table_count++];
    e->table = table;
    e->tex = rnd_texture(RND_R8, 256, 256, table, 256);
    return e->tex;
}

/* The pages, for --soft3d, which draws the commands itself. */
static const u8 *s_raster_pixels;

void gl3d_raster_pages(const u8 *pixels, int count)
{
    s_raster_pixels = pixels;
    if (g_soft3d) {
        return;
    }
    if (s_raster_pages != 0) {
        rnd_texture_free(s_raster_pages);
    }
    s_raster_pages = rnd_pages(pixels, count);
}

static int cmd_mode(int type)
{
    switch (type) {
    case RASTER_TEXTURE: return 0;
    case RASTER_FLAT: return 1;
    case RASTER_TEXTURE_TABLE: return 2;
    default: return 3;
    }
}

/* A command's pixels: x0, y0, x1, y1, the end excluded, a pixel more
 * all round. */
static int cmd_opaque(const struct raster_cmd *c)
{
    return c->type == RASTER_TEXTURE || c->type == RASTER_FLAT;
}

static void cmd_rect(const struct raster_cmd *c, int r[4])
{
    float x0 = c->x[0], x1 = c->x[0], y0 = c->y[0], y1 = c->y[0];
    int k;

    for (k = 1; k < 3; k++) {
        x0 = c->x[k] < x0 ? c->x[k] : x0;
        x1 = c->x[k] > x1 ? c->x[k] : x1;
        y0 = c->y[k] < y0 ? c->y[k] : y0;
        y1 = c->y[k] > y1 ? c->y[k] : y1;
    }
    r[0] = (int)floorf(x0) - 1;
    r[1] = (int)floorf(y0) - 1;
    r[2] = (int)ceilf(x1) + 1;
    r[3] = (int)ceilf(y1) + 1;
}

/* A flat triangle into memory, as the GPU path covers it: a pixel is
 * filled when its bottom right corner (x + 1, y + 1) is inside. */
static void flat_fill(u8 *target, int w, int h, int pitch, const struct raster_cmd *c)
{
    double x0 = c->x[0], y0 = c->y[0], x1 = c->x[1], y1 = c->y[1], x2 = c->x[2], y2 = c->y[2];
    double area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    int lx = (int)floor(fmin(x0, fmin(x1, x2))) - 1, hx = (int)ceil(fmax(x0, fmax(x1, x2)));
    int ly = (int)floor(fmin(y0, fmin(y1, y2))) - 1, hy = (int)ceil(fmax(y0, fmax(y1, y2)));
    int x, y;

    if (area == 0.0) {
        return;
    }
    lx = lx < 0 ? 0 : lx;
    ly = ly < 0 ? 0 : ly;
    hx = hx > w ? w : hx;
    hy = hy > h ? h : hy;
    for (y = ly; y < hy; y++) {
        double py = y + 1.0;

        for (x = lx; x < hx; x++) {
            double px = x + 1.0;
            double e0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
            double e1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
            double e2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);

            if (area > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                target[y * pitch + x] = c->colour;
            }
        }
    }
}

void gl3d_raster(u8 *target, int w, int h, int pitch, const struct raster_cmd *cmds, int count)
{
    int i, k, run, pw, ph, copied = 0, batch = rnd_batch_copies();
    int gpu;
    float size[2], off[2] = { 0, 0 };

    if (g_soft3d) {
        soft3d_raster(target, w, h, pitch, cmds, count, s_raster_pixels);
        return;
    }
    gpu = canvas_is_gpu(target);

    /* Only flat triangles into memory (the race HUD's needle and bar):
     * filled here, as a trip through the GPU waits for it twice. */
    if (!gpu) {
        for (i = 0; i < count && cmds[i].type == RASTER_FLAT; i++) {
        }
        if (i == count) {
            for (i = 0; i < count; i++) {
                flat_fill(target, w, h, pitch, &cmds[i]);
            }
            return;
        }
    }

    if (!gpu && (w != s_raster_w || h != s_raster_h)) {
        if (s_raster_color != 0) {
            rnd_target_free(s_raster_target);
            rnd_texture_free(s_raster_color);
            rnd_texture_free(s_raster_dst);
        }
        s_raster_color = rnd_texture(RND_R8, w, h, NULL, 0);
        s_raster_dst = rnd_texture(RND_R8, w, h, NULL, 0);
        s_raster_target = rnd_target(s_raster_color, 0);
        s_raster_w = w;
        s_raster_h = h;
    }
    if (count * 3 > s_raster_cap) {
        s_raster_cap = count * 3;
        free(s_raster_verts);
        s_raster_verts = malloc((size_t)s_raster_cap * RASTER_FLOATS * sizeof(float));
    }
    for (i = 0; i < count; i++) {
        float *o = s_raster_verts + i * 3 * RASTER_FLOATS;

        for (k = 0; k < 3; k++, o += RASTER_FLOATS) {
            o[0] = cmds[i].x[k];
            o[1] = cmds[i].y[k];
            o[2] = cmds[i].u[k];
            o[3] = cmds[i].v[k];
            o[4] = (float)cmds[i].page;
            o[5] = cmds[i].colour;
            o[6] = (float)cmd_mode(cmds[i].type);
            o[7] = 0;
        }
    }

    if (gpu) {
        /* Straight into the surface, at its resolution. A surface with
         * NATIVE's margins spans more than w x h. */
        canvas_bind(target, &pw, &ph, 0);
        canvas_extent(target, &off[0], &off[1]);
    } else {
        rnd_bind(s_raster_target);
        rnd_texture_set(s_raster_color, w, h, target, pitch);
    }
    rnd_use(RND_RASTER);
    rnd_stream(s_raster_verts, count * 3);
    size[0] = w + 2 * off[0];
    size[1] = h + 2 * off[1];
    rnd_uniform(RND_U_SIZE, size);
    rnd_uniform(RND_U_OFF, off);
    rnd_texture_unit(0, s_raster_pages);
    rnd_depth(RND_DEPTH_OFF, 1);
    rnd_cull(RND_CULL_OFF);
    cells_reset(w, h);

    if (batch) {
        /* The cells take in NATIVE's margins, which the faces reach. */
        int *layer = layers_for(count), top = 0, l;
        int mx = (int)ceilf(off[0]), my = (int)ceilf(off[1]);

        grid_reset(w + 2 * mx, h + 2 * my);
        for (i = 0; i < count; i++) {
            struct geo *g = &s_geo[i];

            cmd_rect(&cmds[i], g->r);
            g->r[0] += mx;
            g->r[1] += my;
            g->r[2] += mx;
            g->r[3] += my;
            g->whole = 0;
            g->tris = 1;
            for (k = 0; k < 3; k++) {
                g->x[k] = cmds[i].x[k];
                g->y[k] = cmds[i].y[k];
            }
            layer_of(i, !cmd_opaque(&cmds[i]));
            top = layer[i] > top ? layer[i] : top;
        }
        for (l = 0; l <= top; l++) {
            if (l > 0) {
                unsigned copy;

                if (gpu) {
                    copy = canvas_snapshot(target);
                } else {
                    rnd_copy(s_raster_dst, 0, 0, w, h);
                    copy = s_raster_dst;
                }
                rnd_texture_unit(2, copy);
            }
            for (i = 0; i < count; i += run) {
                run = 1;
                if (layer[i] != l) {
                    continue;
                }
                if (cmd_opaque(&cmds[i])) {
                    while (i + run < count && layer[i + run] == l && cmd_opaque(&cmds[i + run])) {
                        run++;
                    }
                } else {
                    rnd_texture_unit(1, table_get(cmds[i].table));
                }
                rnd_draw(RND_TRIANGLES, i * 3, run * 3);
            }
        }
        if (!gpu) {
            rnd_read(0, 0, w, h, RND_R8, target, pitch);
        }
        return;
    }

    /* Opaque commands go in runs; one through a table reads what is
     * under it, from a copy of the frame taken after whatever it
     * covers was drawn. */
    for (i = 0; i < count; i += run) {
        int r[4];

        if (cmds[i].type == RASTER_TEXTURE || cmds[i].type == RASTER_FLAT) {
            for (run = 1; i + run < count && (cmds[i + run].type == RASTER_TEXTURE
                                              || cmds[i + run].type == RASTER_FLAT); run++) {
            }
        } else {
            run = 1;
            cmd_rect(&cmds[i], r);
            if (!batch) {
                /* What the command covers, or the whole frame. */
                if (gpu) {
                    const struct raster_cmd *c = &cmds[i];
                    float x0 = c->x[0], x1 = c->x[0], y0 = c->y[0], y1 = c->y[0];

                    for (k = 1; k < 3; k++) {
                        x0 = c->x[k] < x0 ? c->x[k] : x0;
                        x1 = c->x[k] > x1 ? c->x[k] : x1;
                        y0 = c->y[k] < y0 ? c->y[k] : y0;
                        y1 = c->y[k] > y1 ? c->y[k] : y1;
                    }
                    rnd_texture_unit(2, canvas_snapshot_rect(target, x0, y0, x1, y1));
                } else {
                    rnd_copy(s_raster_dst, 0, 0, w, h);
                    rnd_texture_unit(2, s_raster_dst);
                }
            } else if (!copied || cells_touch(r, 0)) {
                unsigned copy;

                if (gpu) {
                    copy = canvas_snapshot(target);
                } else {
                    rnd_copy(s_raster_dst, 0, 0, w, h);
                    copy = s_raster_dst;
                }
                cells_reset(w, h);
                copied = 1;
                rnd_texture_unit(2, copy);
            }
            rnd_texture_unit(1, table_get(cmds[i].table));
        }
        rnd_draw(RND_TRIANGLES, i * 3, run * 3);
        for (k = i; k < i + run; k++) {
            cmd_rect(&cmds[k], r);
            cells_touch(r, 1);
        }
    }

    if (!gpu) {
        rnd_read(0, 0, w, h, RND_R8, target, pitch);
    }
}

int gl3d_surface_begin(u8 *key, int lw, int lh)
{
    int pw, ph;

    if (!s_ready) {
        init();
    }
    if (canvas_bind(key, &pw, &ph, 1) == 0) {
        return 0;
    }
    {
        float mx, my;

        canvas_extent(key, &mx, &my);
        s_ox = mx;
        s_oy = my;
        s_vw = lw + 2.0 * mx;
        s_vh = lh + 2.0 * my;
    }
    s_w = pw;
    s_h = ph;
    return 1;
}

/* The next draw binds its own target. */
void gl3d_surface_end(void)
{
}
