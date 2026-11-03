/*
 * rnd_gxm.c - rnd.h on GXM, the PlayStation Vita's GPU.
 *
 * Shaders. The programs are rnd_gl.c's, in Cg. GXM runs only compiled
 * programs (.gxp): each is looked for in app0:shaders/ (packaged from
 * platforms/vita/shaders/), then in ux0:data/ignition/shaders/, and
 * otherwise compiled on the device by the system's compiler,
 * libshacccg.suprx (ur0:data/, where Vita homebrew keeps it), and
 * saved to ux0:data/ignition/shaders/. A file's name carries a hash of
 * its source, so a changed shader is compiled again. Cg has no
 * texelFetch: textures are sampled at texel centres (all are nearest),
 * with their sizes as uniforms. An index is written as (i + 0.25) /
 * 255, which GXM's pack to 8 bits makes i whether it rounds or
 * truncates.
 *
 * Targets. Each draw goes into the scene of the bound target; drawing
 * into another target ends the scene and begins one there. A target's
 * colour stays in its memory between scenes and its depth is stored
 * and loaded again, so a target picks up where it was. A copy (rnd_copy,
 * rnd_blit) is a scene of its own on the destination, drawing a quad
 * that reads the source. Targets are drawn the way up their memory is
 * (row 0 first, as OpenGL's framebuffer objects); the screen the other
 * way, as a window. GXM has no clear: colour is cleared with a quad,
 * depth by beginning a scene that does not load it. The scissor is a
 * test in the fragment programs, as GXM's region clip is by tiles.
 *
 * Memory. Two heaps carved from memblocks mapped for the GPU: CDRAM for
 * textures, targets and the mesh store, uncached main memory for
 * textures filled each frame and buffers. The GPU runs up to two
 * frames behind; what a frame's draws read is not written again until
 * that frame is done: vertices go to a per-frame arena, a texture
 * filled anew gets new memory, freed memory waits three frames, and
 * the mesh store has a copy per frame in flight. A texture the GPU
 * draws into is filled from the CPU only after waiting for the GPU.
 *
 * The screen: three display buffers in CDRAM, shown by the display
 * queue from rnd_present (plat_present on the Vita).
 */

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/shacccg.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rnd.h"
#include "prof.h"

#define SHADER_DIR "ux0:data/ignition/shaders"

enum {
    SCREEN_W = 960,
    SCREEN_H = 544,
    DISPLAY_BUFFERS = 3,
    FRAMES = 3,                 /* in flight: two queued and one built */
    ARENA_BYTES = 8 * 1024 * 1024,
    MAX_INDEX = 65535,          /* vertices a draw, with 16 bit indices */
    UNITS = 4,
    FLOATS_MAX = 16
};

/* The programs: rnd.h's and two of this file's. */
enum { P_CLEAR = RND_PROGRAMS, P_COPY, PROGRAMS };

/* ------------------------------------------------------------ shaders */

#define CLIP_TEST \
    "    if (frag.x < u_clip.x || frag.y < u_clip.y || frag.x >= u_clip.z\n" \
    "        || frag.y >= u_clip.w) {\n" \
    "        discard;\n" \
    "    }\n"

/* Texel t of page `page` of a slot laid out as tiles, grid.x to a row,
 * grid.yz one over the texture's size. (`row` is a keyword in the Vita's
 * Cg; the shaders do not name anything that.) */
#define PAGES_TEXEL \
    "float pages_texel(sampler2D pages, float3 grid, float page, float2 t) {\n" \
    "    float tile_y = floor((page + 0.5) / grid.x);\n" \
    "    float tile_x = page - tile_y * grid.x;\n" \
    "    float2 at = (float2(tile_x, tile_y) * 256.0 + t + 0.5) * grid.yz;\n" \
    "    return floor(tex2D(pages, at).r * 255.0 + 0.5);\n" \
    "}\n"

/* table[d][s]: row s, column d. */
#define TABLE_OF \
    "float table_of(sampler2D table, sampler2D dst, float2 dst_size, float4 frag, float s) {\n" \
    "    float d = floor(tex2D(dst, (floor(frag.xy) + 0.5) / dst_size).r * 255.0 + 0.5);\n" \
    "    return floor(tex2D(table, float2(d + 0.5, s + 0.5) / 256.0).r * 255.0 + 0.5);\n" \
    "}\n"

static const char s_canvas_v[] =
    "void main(float2 a_pos, float2 a_uv, uniform float2 u_lsize, uniform float2 u_off,\n"
    "          float2 out v_uv : TEXCOORD0, float4 out v_pos : POSITION) {\n"
    "    v_uv = a_uv;\n"
    "    v_pos = float4((a_pos + u_off) / u_lsize * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char s_canvas_f[] =
    TABLE_OF
    "float4 main(float2 v_uv : TEXCOORD0, float4 frag : WPOS,\n"
    "            uniform sampler2D u_src, uniform sampler2D u_table, uniform sampler2D u_rows,\n"
    "            uniform sampler2D u_dst, uniform float u_mode, uniform float2 u_src_size,\n"
    "            uniform float2 u_dst_size, uniform float4 u_clip) : COLOR {\n"
    CLIP_TEST
    "    float2 t = clamp(floor(v_uv), float2(0.0, 0.0), u_src_size - 1.0);\n"
    "    float s = floor(tex2D(u_src, (t + 0.5) / u_src_size).r * 255.0 + 0.5);\n"
    "    if (u_mode > 0.5) {\n"
    "        float2 kind = tex2D(u_rows, float2((s + 0.5) / 256.0, 0.5)).rg;\n"
    "        float cls = floor(kind.r * 255.0 + 0.5);\n"
    "        if (cls < 0.5) {\n"
    "            discard;\n"
    "        }\n"
    "        if (cls < 1.5) {\n"
    "            s = floor(kind.g * 255.0 + 0.5);\n"
    "        } else {\n"
    "            s = table_of(u_table, u_dst, u_dst_size, frag, s);\n"
    "        }\n"
    "    }\n"
    "    return float4((s + 0.25) / 255.0, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char s_present_v[] =
    "void main(float2 a_corner, uniform float4 u_rect,\n"
    "          float2 out v_uv : TEXCOORD0, float4 out v_pos : POSITION) {\n"
    "    v_uv = float2(a_corner.x, 1.0 - a_corner.y);\n"
    "    v_pos = float4(lerp(u_rect.xy, u_rect.zw, a_corner), 0.0, 1.0);\n"
    "}\n";

static const char s_present_f[] =
    "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D u_index,\n"
    "            uniform sampler2D u_palette) : COLOR {\n"
    "    float i = floor(tex2D(u_index, v_uv).r * 255.0 + 0.5);\n"
    "    float3 c = tex2D(u_palette, float2((i + 0.5) / 256.0, 0.5)).rgb;\n"
    "    return float4((floor(c * 255.0 + 0.5) + 0.25) / 255.0, 1.0);\n"
    "}\n";

static const char s_over_f[] =
    "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D u_index,\n"
    "            uniform sampler2D u_palette, uniform sampler2D u_snap) : COLOR {\n"
    "    float i = floor(tex2D(u_index, v_uv).r * 255.0 + 0.5);\n"
    "    float s = floor(tex2D(u_snap, v_uv).r * 255.0 + 0.5);\n"
    "    if (abs(i - s) < 0.5) {\n"
    "        discard;\n"
    "    }\n"
    "    float3 c = tex2D(u_palette, float2((i + 0.5) / 256.0, 0.5)).rgb;\n"
    "    return float4((floor(c * 255.0 + 0.5) + 0.25) / 255.0, 1.0);\n"
    "}\n";

/* MESH, DYN and RASTER: v_info is page, colour, mode, the same at a
 * face's three corners. */
static const char s_mesh_v[] =
    "void main(float3 a_pos, float2 a_uv, float3 a_info, uniform float4x4 u_matrix,\n"
    "          float2 out v_uv : TEXCOORD0, float3 out v_info : TEXCOORD1,\n"
    "          float4 out v_pos : POSITION) {\n"
    "    v_uv = a_uv;\n"
    "    v_info = a_info;\n"
    "    v_pos = mul(float4(a_pos, 1.0), u_matrix);\n"
    "}\n";

static const char s_mesh_f[] =
    PAGES_TEXEL
    "float4 main(float2 v_uv : TEXCOORD0, float3 v_info : TEXCOORD1, float4 frag : WPOS,\n"
    "            uniform sampler2D u_pages, uniform float3 u_grid,\n"
    "            uniform float4 u_clip) : COLOR {\n"
    CLIP_TEST
    "    float c = floor(v_info.y + 0.5);\n"
    "    if (v_info.z < 1.5) {\n"
    "        float2 t = min(floor(frac(v_uv) * 256.0), 255.0);\n"
    "        c = pages_texel(u_pages, u_grid, floor(v_info.x + 0.5), t);\n"
    "        if (v_info.z > 0.5 && c < 0.5) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n"
    "    return float4((c + 0.25) / 255.0, 1.0, 0.0, 1.0);\n"
    "}\n";

static const char s_dyn_v[] =
    "void main(float4 a_pos, float2 a_uv, float3 a_info,\n"
    "          float2 out v_uv : TEXCOORD0, float3 out v_info : TEXCOORD1,\n"
    "          float4 out v_pos : POSITION) {\n"
    "    v_uv = a_uv;\n"
    "    v_info = a_info;\n"
    "    v_pos = a_pos;\n"
    "}\n";

static const char s_dyn_f[] =
    PAGES_TEXEL
    TABLE_OF
    "float4 main(float2 v_uv : TEXCOORD0, float3 v_info : TEXCOORD1, float4 frag : WPOS,\n"
    "            uniform sampler2D u_pages, uniform float3 u_grid, uniform sampler2D u_table,\n"
    "            uniform sampler2D u_dst, uniform float2 u_dst_size, uniform float u_use_table,\n"
    "            uniform float4 u_clip) : COLOR {\n"
    CLIP_TEST
    "    float c = v_info.z > 2.5 ? floor(v_uv.x) : floor(v_info.y + 0.5);\n"
    "    if (v_info.z < 1.5) {\n"
    "        float2 t = min(floor(frac(v_uv) * 256.0), 255.0);\n"
    "        c = pages_texel(u_pages, u_grid, floor(v_info.x + 0.5), t);\n"
    "        if (v_info.z > 0.5 && c < 0.5) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n"
    "    if (u_use_table > 0.5) {\n"
    "        c = table_of(u_table, u_dst, u_dst_size, frag, c);\n"
    "    }\n"
    "    return float4((c + 0.25) / 255.0, 1.0, 0.0, 1.0);\n"
    "}\n";

/* PARTICLE: as DYN, but the colour index is a row of u_table (256 x 1
 * RGBA, premultiplied; fx_blend.h), blended by the fragment program. */
static const char s_particle_f[] =
    PAGES_TEXEL
    "float4 main(float2 v_uv : TEXCOORD0, float3 v_info : TEXCOORD1, float4 frag : WPOS,\n"
    "            uniform sampler2D u_pages, uniform float3 u_grid, uniform sampler2D u_table,\n"
    "            uniform float4 u_clip) : COLOR {\n"
    CLIP_TEST
    "    float c = v_info.z > 2.5 ? floor(v_uv.x) : floor(v_info.y + 0.5);\n"
    "    if (v_info.z < 1.5) {\n"
    "        float2 t = min(floor(frac(v_uv) * 256.0), 255.0);\n"
    "        c = pages_texel(u_pages, u_grid, floor(v_info.x + 0.5), t);\n"
    "        if (v_info.z > 0.5 && c < 0.5) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n"
    "    return tex2D(u_table, float2((c + 0.5) / 256.0, 0.5));\n"
    "}\n";

/* LAYER: an RGBA layer, blended over the screen. */
static const char s_layer_f[] =
    "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D u_layer) : COLOR {\n"
    "    return tex2D(u_layer, v_uv);\n"
    "}\n";

static const char s_raster_v[] =
    "void main(float2 a_pos, float2 a_uv, float3 a_info, uniform float2 u_size,\n"
    "          uniform float2 u_off, float2 out v_uv : TEXCOORD0,\n"
    "          float3 out v_info : TEXCOORD1, float4 out v_pos : POSITION) {\n"
    "    v_uv = a_uv;\n"
    "    v_info = a_info;\n"
    "    v_pos = float4((a_pos - 0.5 + u_off) / u_size * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char s_raster_f[] =
    PAGES_TEXEL
    TABLE_OF
    "float4 main(float2 v_uv : TEXCOORD0, float3 v_info : TEXCOORD1, float4 frag : WPOS,\n"
    "            uniform sampler2D u_pages, uniform float3 u_grid, uniform sampler2D u_table,\n"
    "            uniform sampler2D u_dst, uniform float2 u_dst_size,\n"
    "            uniform float4 u_clip) : COLOR {\n"
    CLIP_TEST
    "    float mode = v_info.z;\n"
    "    float a = floor(v_info.y + 0.5);\n"
    "    if (mode < 0.5 || (mode > 1.5 && mode < 2.5)) {\n"
    "        float2 t = floor(v_uv);\n"
    "        t = t - 256.0 * floor(t / 256.0);\n"
    "        a = pages_texel(u_pages, u_grid, floor(v_info.x + 0.5), t);\n"
    "    }\n"
    "    if (mode > 1.5) {\n"
    "        a = table_of(u_table, u_dst, u_dst_size, frag, a);\n"
    "    }\n"
    "    return float4((a + 0.25) / 255.0, 0.0, 0.0, 1.0);\n"
    "}\n";

/* CLEAR: the colour (exact bytes), within the scissor. */
static const char s_clear_v[] =
    "void main(float2 a_pos, float4 out v_pos : POSITION) {\n"
    "    v_pos = float4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char s_clear_f[] =
    "float4 main(float4 frag : WPOS, uniform float4 u_color, uniform float4 u_clip) : COLOR {\n"
    CLIP_TEST
    "    return (floor(u_color * 255.0 + 0.5) + 0.25) / 255.0;\n"
    "}\n";

/* COPY: the source's texel under v_uv, byte for byte. */
static const char s_copy_v[] =
    "void main(float2 a_pos, float2 a_uv, float2 out v_uv : TEXCOORD0,\n"
    "          float4 out v_pos : POSITION) {\n"
    "    v_uv = a_uv;\n"
    "    v_pos = float4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char s_copy_f[] =
    "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D u_src,\n"
    "            uniform float2 u_src_size) : COLOR {\n"
    "    float4 c = tex2D(u_src, (floor(v_uv) + 0.5) / u_src_size);\n"
    "    return (floor(c * 255.0 + 0.5) + 0.25) / 255.0;\n"
    "}\n";

/* Uniforms: rnd.h's, then those this file works out. */
enum { U_SRC_SIZE = RND_UNIFORMS, U_DST_SIZE, U_GRID, U_CLIP, U_COLOR, UNIFORMS };

static const char *const s_uniform_names[UNIFORMS] = {
    "u_lsize", "u_off", "u_mode", "u_rect", "u_matrix", "u_use_table", "u_size",
    "u_src_size", "u_dst_size", "u_grid", "u_clip", "u_color"
};

static const int s_uniform_floats[UNIFORMS] = { 2, 2, 1, 4, 16, 1, 2, 2, 2, 3, 4, 4 };

struct attr {
    const char *name;
    int floats;
};

static const struct {
    const char *name;
    const char *vs, *fs;
    struct attr attrs[3];
    int stride;                 /* floats a vertex */
    const char *samplers[UNITS];
} s_def[PROGRAMS] = {
    { "canvas", s_canvas_v, s_canvas_f, { { "a_pos", 2 }, { "a_uv", 2 } }, 4,
      { "u_src", "u_table", "u_rows", "u_dst" } },
    { "present", s_present_v, s_present_f, { { "a_corner", 2 } }, 2,
      { "u_index", "u_palette" } },
    { "over", s_present_v, s_over_f, { { "a_corner", 2 } }, 2,
      { "u_index", "u_palette", "u_snap" } },
    { "mesh", s_mesh_v, s_mesh_f, { { "a_pos", 3 }, { "a_uv", 2 }, { "a_info", 3 } }, 8,
      { "u_pages" } },
    { "dyn", s_dyn_v, s_dyn_f, { { "a_pos", 4 }, { "a_uv", 2 }, { "a_info", 3 } }, 9,
      { "u_pages", "u_table", "u_dst" } },
    { "raster", s_raster_v, s_raster_f, { { "a_pos", 2 }, { "a_uv", 2 }, { "a_info", 3 } }, 8,
      { "u_pages", "u_table", "u_dst" } },
    { "particle", s_dyn_v, s_particle_f, { { "a_pos", 4 }, { "a_uv", 2 }, { "a_info", 3 } }, 9,
      { "u_pages", "u_table" } },
    { "layer", s_present_v, s_layer_f, { { "a_corner", 2 } }, 2, { "u_layer" } },
    { "clear", s_clear_v, s_clear_f, { { "a_pos", 2 } }, 2, { NULL } },
    { "copy", s_copy_v, s_copy_f, { { "a_pos", 2 }, { "a_uv", 2 } }, 4, { "u_src" } },
};

/* ------------------------------------------------------------ memory */

struct range {
    u32 off, len;
};

struct heap {
    u8 *base;
    u32 size;
    struct range *free_r, *used_r;
    int nfree, nused, cfree, cused;
};

static struct heap s_vram;      /* CDRAM */
static struct heap s_ram;       /* uncached main memory */

static u32 align_up(u32 v, u32 a)
{
    return (v + a - 1) & ~(a - 1);
}

static void *map_block(SceKernelMemBlockType type, u32 size, const char *name)
{
    SceUID uid = sceKernelAllocMemBlock(name, type, size, NULL);
    void *p = NULL;

    if (uid < 0 || sceKernelGetMemBlockBase(uid, &p) < 0) {
        return NULL;
    }
    if (sceGxmMapMemory(p, size, SCE_GXM_MEMORY_ATTRIB_RW) < 0) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    return p;
}

static void push_range(struct range **a, int *n, int *cap, int at, u32 off, u32 len)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *a = realloc(*a, (size_t)*cap * sizeof(**a));
    }
    memmove(*a + at + 1, *a + at, (size_t)(*n - at) * sizeof(**a));
    (*a)[at].off = off;
    (*a)[at].len = len;
    (*n)++;
}

static void drop_range(struct range *a, int *n, int at)
{
    memmove(a + at, a + at + 1, (size_t)(*n - at - 1) * sizeof(*a));
    (*n)--;
}

static int heap_init(struct heap *h, SceKernelMemBlockType type, u32 size, const char *name)
{
    memset(h, 0, sizeof(*h));
    h->base = map_block(type, size, name);
    if (h->base == NULL) {
        return 0;
    }
    h->size = size;
    push_range(&h->free_r, &h->nfree, &h->cfree, 0, 0, size);
    return 1;
}

/* First fit; the free ranges are kept in address order. */
static void *heap_alloc(struct heap *h, u32 len, u32 align)
{
    int i;

    len = align_up(len, 16);
    for (i = 0; i < h->nfree; i++) {
        u32 off = h->free_r[i].off, flen = h->free_r[i].len;
        u32 start = align_up(off, align), pad = start - off;

        if (flen < pad + len) {
            continue;
        }
        drop_range(h->free_r, &h->nfree, i);
        if (flen > pad + len) {
            push_range(&h->free_r, &h->nfree, &h->cfree, i, start + len, flen - pad - len);
        }
        if (pad != 0) {
            push_range(&h->free_r, &h->nfree, &h->cfree, i, off, pad);
        }
        push_range(&h->used_r, &h->nused, &h->cused, h->nused, start, len);
        return h->base + start;
    }
    return NULL;
}

static int heap_free(struct heap *h, void *p)
{
    u32 off, len;
    int i, at;

    if (h->base == NULL || (u8 *)p < h->base || (u8 *)p >= h->base + h->size) {
        return 0;
    }
    off = (u32)((u8 *)p - h->base);
    for (i = h->nused - 1; i >= 0 && h->used_r[i].off != off; i--) {
    }
    if (i < 0) {
        return 1;
    }
    len = h->used_r[i].len;
    drop_range(h->used_r, &h->nused, i);
    for (at = 0; at < h->nfree && h->free_r[at].off < off; at++) {
    }
    if (at < h->nfree && h->free_r[at].off == off + len) {
        len += h->free_r[at].len;
        drop_range(h->free_r, &h->nfree, at);
    }
    if (at > 0 && h->free_r[at - 1].off + h->free_r[at - 1].len == off) {
        h->free_r[at - 1].len += len;
        return 1;
    }
    push_range(&h->free_r, &h->nfree, &h->cfree, at, off, len);
    return 1;
}

static void *gpu_alloc(u32 len, u32 align, int vram)
{
    void *p = vram ? heap_alloc(&s_vram, len, align) : NULL;

    if (p == NULL) {
        p = heap_alloc(&s_ram, len, align);
    }
    if (p == NULL && !vram) {
        p = heap_alloc(&s_vram, len, align);
    }
    if (p == NULL) {
        fprintf(stderr, "gxm: out of GPU memory (%u bytes)\n", (unsigned)len);
    }
    return p;
}

static void gpu_free_now(void *p)
{
    if (p != NULL && !heap_free(&s_vram, p)) {
        heap_free(&s_ram, p);
    }
}

/* Freed once the frames that may read it are done. */
static struct {
    void **p;
    int n, cap;
} s_grave[FRAMES];

static int s_slot;              /* this frame's of FRAMES */
static unsigned s_frame_no;     /* frames presented */
static SceUInt64 s_end_us;      /* time in sceGxmEndScene, for the log */

static void gpu_free(void *p)
{
    if (p == NULL) {
        return;
    }
    if (s_grave[s_slot].n == s_grave[s_slot].cap) {
        s_grave[s_slot].cap = s_grave[s_slot].cap ? s_grave[s_slot].cap * 2 : 64;
        s_grave[s_slot].p = realloc(s_grave[s_slot].p, (size_t)s_grave[s_slot].cap * sizeof(void *));
    }
    s_grave[s_slot].p[s_grave[s_slot].n++] = p;
}

static struct {
    u8 *base;
    u32 used;
} s_arena[FRAMES];

/* ------------------------------------------------------------ state */

struct texture {
    int live, format, w, h;
    u32 stride;                 /* bytes a row */
    u8 *data;
    SceGxmTexture t;
    int cols, rows;             /* pages: the tiles' layout */
    int drawn;                  /* the GPU draws into it */
    unsigned copy_target;       /* a target to copy into it */
    unsigned alias;             /* read as this texture instead (rnd_copy) */
    /* A texture filled once a frame keeps a buffer for each frame in
     * flight and a cached copy of each, and gets only its changed rows
     * (rnd_texture_set). */
    u8 *ring[FRAMES], *shadow[FRAMES];
    u8 *rowzero[FRAMES];        /* 1: the ring's row was all zero when last filled */
    int ring_w, ring_h, in_ring;
    unsigned set_frame;
};

struct target {
    int live;
    unsigned color;
    int depth;                  /* has a depth buffer of its own */
    void *depth_data;
    int depth_w, depth_h;
    int depth_fresh;            /* cleared: the next scene does not load it */
    unsigned depth_of;          /* draws with this target's depth buffer (rnd_target_shared) */
    SceGxmColorSurface surface;
    SceGxmDepthStencilSurface ds;
};

static struct texture *s_tex;
static unsigned s_ntex;
static struct target *s_tgt;
static unsigned s_ntgt;

static SceGxmContext *s_ctx;
static SceGxmShaderPatcher *s_patcher;
static void *s_display[DISPLAY_BUFFERS];
static SceGxmColorSurface s_display_surface[DISPLAY_BUFFERS];
static SceGxmSyncObject *s_display_sync[DISPLAY_BUFFERS];
static SceGxmDepthStencilSurface s_display_depth;
static int s_back, s_front;

/* Render targets by size. A render target holds the driver's memory for
 * SCENES_PER_RT scenes in flight; a scene past that waits for the GPU to
 * finish an earlier one. A frame has many scenes (each copy ends one),
 * so each size has a ring of RT_RING targets, taken in turn. */
enum { RT_RING = 8, SCENES_PER_RT = 8 };

static struct {
    int w, h, next;
    SceGxmRenderTarget *rt[RT_RING];
} s_rts[16];
static int s_rt_count;

/* A depth buffer for the targets that have none: never loaded or kept. */
static SceGxmDepthStencilSurface s_scratch;
static void *s_scratch_data;
static int s_scratch_w, s_scratch_h;

static int s_in_scene;
static unsigned s_scene;        /* the target of the scene, 0 the screen */
static unsigned s_bound;        /* 0 the screen */
static int s_screen_w = SCREEN_W, s_screen_h = SCREEN_H;

static volatile unsigned int *s_notes;
static unsigned s_serial, s_fence[FRAMES];

static struct {
    const SceGxmProgram *vgxp, *fgxp;
    SceGxmShaderPatcherId vid, fid;
    SceGxmVertexProgram *vp;
    SceGxmFragmentProgram *fp;
    const SceGxmProgramParameter *uv[UNIFORMS], *uf[UNIFORMS];
    int unit_index[UNITS];      /* the sampler's texture unit, or -1 */
    float u[UNIFORMS][FLOATS_MAX];
} s_prog[PROGRAMS];

/* What the context was last given, so that a draw sets only what
 * changed. A new scene starts it afresh. */
static struct {
    int valid;
    int depth_func, depth_write, cull, offset, screen;
    int prog;                   /* programs set, or -1 */
    int uprog;                  /* the uniforms' program, or -1 */
    float u[UNIFORMS][FLOATS_MAX];
    const struct texture *tex[UNITS];
    const u8 *data[UNITS];
} s_cur;

static int s_use;
static unsigned s_units[UNITS];
static const void *s_stream;    /* rnd_stream's vertices, in the arena */
static u32 s_stream_bytes;
static u16 *s_indices;
static float *s_corners;        /* PRESENT and OVER's quad */
static unsigned s_dummy;        /* bound where a texture is missing */

static struct {
    int on, x, y, w, h;
} s_scissor;
static int s_depth_func = RND_DEPTH_OFF, s_depth_write = 1, s_cull, s_offset;

/* The mesh store: the vertices, and a copy for each frame in flight
 * with the range it lacks. */
static float *s_mesh;
static void *s_mesh_gpu[FRAMES];
static int s_mesh_count, s_mesh_lo[FRAMES], s_mesh_hi[FRAMES];

static struct texture *tex_get(unsigned t)
{
    return t != 0 && t < s_ntex && s_tex[t].live ? &s_tex[t] : NULL;
}

/* The texture a draw reads for t: the target it stands for after
 * rnd_copy. */
static struct texture *read_as(struct texture *t)
{
    struct texture *a = t != NULL && t->alias != 0 ? tex_get(t->alias) : NULL;

    return a != NULL ? a : t;
}

static struct target *tgt_get(unsigned t)
{
    return t != 0 && t < s_ntgt && s_tgt[t].live ? &s_tgt[t] : NULL;
}

/* ------------------------------------------------------------ frames */

static void *arena_alloc(u32 len);

struct display_data {
    void *address;
};

static void display_callback(const void *data)
{
    const struct display_data *d = data;
    SceDisplayFrameBuf fb;

    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = d->address;
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
}

static SceGxmRenderTarget *render_target(int w, int h)
{
    SceGxmRenderTargetParams p;
    int i, k;

    for (i = 0; i < s_rt_count; i++) {
        if (s_rts[i].w == w && s_rts[i].h == h) {
            k = s_rts[i].next;
            s_rts[i].next = (k + 1) % RT_RING;
            return s_rts[i].rt[k];
        }
    }
    if (s_rt_count == (int)(sizeof(s_rts) / sizeof(s_rts[0]))) {
        fprintf(stderr, "gxm: too many target sizes\n");
        return NULL;
    }
    memset(&p, 0, sizeof(p));
    p.width = (u32)w;
    p.height = (u32)h;
    p.scenesPerFrame = SCENES_PER_RT;
    p.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    p.driverMemBlock = -1;
    for (k = 0; k < RT_RING; k++) {
        if (sceGxmCreateRenderTarget(&p, &s_rts[s_rt_count].rt[k]) < 0) {
            fprintf(stderr, "gxm: no render target of %d x %d\n", w, h);
            return NULL;
        }
    }
    s_rts[s_rt_count].w = w;
    s_rts[s_rt_count].h = h;
    s_rts[s_rt_count].next = 1 % RT_RING;
    return s_rts[s_rt_count++].rt[0];
}

static void *depth_alloc(int w, int h, int *aw, int *ah)
{
    *aw = (int)align_up((u32)w, SCE_GXM_TILE_SIZEX);
    *ah = (int)align_up((u32)h, SCE_GXM_TILE_SIZEY);
    return gpu_alloc((u32)(*aw * *ah * 4), 4096, 1);
}

static void depth_init(SceGxmDepthStencilSurface *ds, void *data, int aw, int keep)
{
    sceGxmDepthStencilSurfaceInit(ds, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                  SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, (u32)aw, data, NULL);
    sceGxmDepthStencilSurfaceSetBackgroundDepth(ds, 1.0f);
    sceGxmDepthStencilSurfaceSetForceStoreMode(ds, keep ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                        : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
    sceGxmDepthStencilSurfaceSetForceLoadMode(ds, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
}

static void end_scene(void)
{
    if (s_in_scene) {
        SceUInt64 t0 = sceKernelGetProcessTimeWide();

        sceGxmEndScene(s_ctx, NULL, NULL);
        s_end_us += sceKernelGetProcessTimeWide() - t0;
        s_in_scene = 0;
    }
}

/* Counts of the costly work, logged every two seconds, to find where a
 * slow frame goes on the console. */
static struct {
    int frames, scenes, finishes, uploads, reads, copies, draws;
    SceUInt64 since, begin_us, wait_us, queue_us, draw_us, fill_us, mesh_us, stream_us;
    u32 mesh_bytes, stream_bytes, fill_bytes;
    u32 ring_fills, rows_total, rows_skipped, rows_copied, ring_w, ring_h;
} s_stats;

/* Microseconds spent in a call that may block on the GPU. */
#define TIMED(field, call) \
    do { \
        SceUInt64 t0_ = sceKernelGetProcessTimeWide(); \
        call; \
        s_stats.field += sceKernelGetProcessTimeWide() - t0_; \
    } while (0)

static void stats_frame(void)
{
    SceUInt64 now = sceKernelGetProcessTimeWide();

    s_stats.frames++;
    if (s_stats.since == 0) {
        s_stats.since = now;
    } else if (now - s_stats.since >= 2000000) {
        double secs = (now - s_stats.since) / 1e6, n = s_stats.frames;

        double ms = secs * 1000.0 / n, begin = s_stats.begin_us / 1000.0 / n;
        double wait = s_stats.wait_us / 1000.0 / n;

        printf("gxm: %.1f fps; a frame: %.0f draws, %.1f scenes, %.1f copies, %.1f waits, %.1f uploads, "
               "%.1f reads; %.1f ms: %.1f the game, %.1f starting scenes, %.1f waiting for "
               "the GPU, %.1f queueing the display, %.1f in draws, %.1f filling textures, "
               "%.1f copying %.0f KB of meshes, %.1f copying %.0f KB of vertices, "
               "%.0f KB of texture rows, %.1f ending scenes\n",
               n / secs, s_stats.draws / n, s_stats.scenes / n, s_stats.copies / n,
               s_stats.finishes / n, s_stats.uploads / n, s_stats.reads / n, ms,
               ms - begin - wait - s_stats.queue_us / 1000.0 / n - s_stats.draw_us / 1000.0 / n,
               begin, wait, s_stats.queue_us / 1000.0 / n, s_stats.draw_us / 1000.0 / n,
               s_stats.fill_us / 1000.0 / n, s_stats.mesh_us / 1000.0 / n,
               s_stats.mesh_bytes / 1024.0 / n, s_stats.stream_us / 1000.0 / n,
               s_stats.stream_bytes / 1024.0 / n, s_stats.fill_bytes / 1024.0 / n,
               s_end_us / 1000.0 / n);
        if (s_stats.ring_fills > 0) {
            printf("gxm: a 2D picture of %ux%u: %.1f a frame, rows %.0f, empty and skipped %.0f, "
                   "copied %.0f\n", s_stats.ring_w, s_stats.ring_h,
                   (double)s_stats.ring_fills / n, (double)s_stats.rows_total / s_stats.ring_fills,
                   (double)s_stats.rows_skipped / s_stats.ring_fills,
                   (double)s_stats.rows_copied / s_stats.ring_fills);
        }
        s_end_us = 0;
        memset(&s_stats, 0, sizeof(s_stats));
        s_stats.since = now;
    }
}

/* Waits for the GPU to finish everything. */
static void finish(void)
{
    s_stats.finishes++;
    end_scene();
    TIMED(wait_us, sceGxmFinish(s_ctx));
}

static void set_viewport(int w, int h, int flip)
{
    sceGxmSetViewport(s_ctx, w * 0.5f, w * 0.5f, h * 0.5f, flip ? -h * 0.5f : h * 0.5f, 0.5f,
                      0.5f);
    sceGxmSetRegionClip(s_ctx, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, (u32)w - 1, (u32)h - 1);
}

/* Begins a scene on target t (0 the screen). */
static int bytes_per_texel(int format);

static int begin_scene(unsigned t)
{
    s_stats.scenes++;
    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.prog = s_cur.uprog = -1;
    struct target *g;
    struct texture *c;
    SceGxmRenderTarget *rt;
    SceGxmDepthStencilSurface *ds;

    end_scene();
    if (t == 0) {
        rt = render_target(SCREEN_W, SCREEN_H);
        int r = -1;

        if (rt != NULL) {
            TIMED(begin_us, r = sceGxmBeginScene(s_ctx, 0, rt, NULL, NULL, s_display_sync[s_back],
                                                 &s_display_surface[s_back], &s_display_depth));
        }
        if (r < 0) {
            return 0;
        }
        set_viewport(s_screen_w, s_screen_h, 1);
        s_in_scene = 1;
        s_scene = 0;
        return 1;
    }
    g = tgt_get(t);
    c = g != NULL ? tex_get(g->color) : NULL;
    if (c == NULL || (rt = render_target(c->w, c->h)) == NULL) {
        return 0;
    }
    sceGxmColorSurfaceInit(&g->surface,
                           c->format == RND_RG8 ? SCE_GXM_COLOR_FORMAT_U8U8_GR
                           : c->format == RND_RGBA8 ? SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR
                                                    : SCE_GXM_COLOR_FORMAT_U8_R,
                           SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                           SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, (u32)c->w, (u32)c->h,
                           c->stride / (u32)bytes_per_texel(c->format), c->data);
    if (g->depth) {
        if (g->depth_data == NULL || c->w > g->depth_w || c->h > g->depth_h) {
            int aw, ah;

            gpu_free(g->depth_data);
            g->depth_data = depth_alloc(c->w, c->h, &aw, &ah);
            g->depth_w = aw;
            g->depth_h = ah;
            depth_init(&g->ds, g->depth_data, aw, 1);
            g->depth_fresh = 1;
        }
        sceGxmDepthStencilSurfaceSetForceLoadMode(&g->ds, g->depth_fresh
                                                  ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED
                                                  : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        g->depth_fresh = 0;
        ds = &g->ds;
    } else if (g->depth_of != 0 && tgt_get(g->depth_of) != NULL
               && tgt_get(g->depth_of)->depth_data != NULL) {
        struct target *o = tgt_get(g->depth_of);

        sceGxmDepthStencilSurfaceSetForceLoadMode(&o->ds, o->depth_fresh
                                                  ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED
                                                  : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        ds = &o->ds;
    } else {
        if (c->w > s_scratch_w || c->h > s_scratch_h) {
            int aw, ah;

            gpu_free(s_scratch_data);
            s_scratch_data = depth_alloc(c->w > s_scratch_w ? c->w : s_scratch_w,
                                         c->h > s_scratch_h ? c->h : s_scratch_h, &aw, &ah);
            s_scratch_w = aw;
            s_scratch_h = ah;
            depth_init(&s_scratch, s_scratch_data, aw, 0);
        }
        ds = &s_scratch;
    }
    {
        int r;

        TIMED(begin_us, r = sceGxmBeginScene(s_ctx, 0, rt, NULL, NULL, NULL, &g->surface, ds));
        if (r < 0) {
            fprintf(stderr, "gxm: scene on %d x %d failed\n", c->w, c->h);
            return 0;
        }
    }
    c->drawn = 1;
    set_viewport(c->w, c->h, 0);
    s_in_scene = 1;
    s_scene = t;
    return 1;
}

static int scene_on(unsigned t)
{
    if (s_in_scene && s_scene == t) {
        return 1;
    }
    return begin_scene(t);
}

/* The frame's slot is free again: what it held can go. */
static void slot_begin(int slot)
{
    int i;

    if (s_fence[slot] != 0) {
        SceGxmNotification n;

        n.address = &s_notes[slot];
        n.value = s_fence[slot];
        while (s_notes[slot] != s_fence[slot]) {
            TIMED(wait_us, sceGxmNotificationWait(&n));
        }
        s_fence[slot] = 0;
    }
    for (i = 0; i < s_grave[slot].n; i++) {
        gpu_free_now(s_grave[slot].p[i]);
    }
    s_grave[slot].n = 0;
    s_arena[slot].used = 0;
    if (s_mesh_hi[slot] > s_mesh_lo[slot] && s_mesh_gpu[slot] != NULL) {
        s_stats.mesh_bytes += (u32)(s_mesh_hi[slot] - s_mesh_lo[slot]) * 8 * sizeof(float);
        TIMED(mesh_us, memcpy((float *)s_mesh_gpu[slot] + s_mesh_lo[slot] * 8,
                              s_mesh + s_mesh_lo[slot] * 8,
                              (size_t)(s_mesh_hi[slot] - s_mesh_lo[slot]) * 8 * sizeof(float)));
    }
    s_mesh_lo[slot] = s_mesh_hi[slot] = 0;
}

static void *arena_alloc(u32 len)
{
    u32 off = align_up(s_arena[s_slot].used, 16);

    if (off + len > ARENA_BYTES) {
        /* Full: wait for the GPU, and the arena is all free but for the
         * vertices draws still to come read. */
        u8 *base = s_arena[s_slot].base;
        u32 keep = 0;

        if (len > ARENA_BYTES) {
            return NULL;
        }
        finish();
        if ((const u8 *)s_stream >= base && (const u8 *)s_stream < base + ARENA_BYTES
            && s_stream_bytes + len <= ARENA_BYTES) {
            memmove(base, s_stream, s_stream_bytes);
            s_stream = base;
            keep = s_stream_bytes;
        }
        off = align_up(keep, 16);
    }
    s_arena[s_slot].used = off + len;
    return s_arena[s_slot].base + off;
}

void rnd_present(void)
{
    struct display_data d;
    SceGxmNotification n;

    if (s_ctx == NULL) {
        return;
    }
    prof_begin(PROF_P_SCENE);
    if (!scene_on(0)) {
        return;
    }
    prof_end(PROF_P_SCENE);
    n.address = &s_notes[s_slot];
    n.value = ++s_serial;
    s_fence[s_slot] = s_serial;
    prof_begin(PROF_P_END);
    sceGxmEndScene(s_ctx, NULL, &n);
    prof_end(PROF_P_END);
    s_in_scene = 0;
    prof_begin(PROF_P_PAD);
    sceGxmPadHeartbeat(&s_display_surface[s_back], s_display_sync[s_back]);
    prof_end(PROF_P_PAD);
    d.address = s_display[s_back];
    prof_begin(PROF_P_QUEUE);
    TIMED(queue_us, sceGxmDisplayQueueAddEntry(s_display_sync[s_front], s_display_sync[s_back], &d));
    prof_end(PROF_P_QUEUE);
    s_front = s_back;
    s_back = (s_back + 1) % DISPLAY_BUFFERS;
    s_slot = (s_slot + 1) % FRAMES;
    s_frame_no++;
    prof_begin(PROF_P_SLOT);
    slot_begin(s_slot);
    prof_end(PROF_P_SLOT);
    stats_frame();
}

/* ------------------------------------------------------------ shaders */

static u8 *read_file(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    u8 *d;
    long n;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = n > 0 ? malloc((size_t)n) : NULL;
    if (d != NULL && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *size = (u32)n;
    return d;
}

static SceShaccCgSourceFile s_source;

static SceShaccCgSourceFile *open_source(const char *name, const SceShaccCgSourceLocation *from,
                                         const SceShaccCgCompileOptions *opt, const char **err)
{
    (void)name;
    (void)from;
    (void)opt;
    (void)err;
    return &s_source;
}

static void *cg_malloc(unsigned int n)
{
    return malloc(n);
}

static int load_compiler(void)
{
    static int state;           /* 0 not tried, 1 loaded, 2 none */
    static const char *const paths[] = {
        "ur0:data/libshacccg.suprx", "ux0:data/ignition/libshacccg.suprx", NULL
    };
    int i;

    if (state == 0) {
        state = 2;
        for (i = 0; paths[i] != NULL; i++) {
            if (sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, NULL) >= 0) {
                sceShaccCgSetDefaultAllocator(cg_malloc, free);
                printf("gxm: shader compiler %s\n", paths[i]);
                state = 1;
                break;
            }
        }
    }
    return state == 1;
}

/* Compiles Cg with libshacccg; the program is malloc'd. */
static u8 *compile(const char *name, const char *src, int fragment, u32 *size)
{
    SceShaccCgCompileOptions opt;
    SceShaccCgCallbackList cb;
    const SceShaccCgCompileOutput *out;
    u8 *data = NULL;
    int i;

    if (!load_compiler()) {
        return NULL;
    }
    s_source.fileName = name;
    s_source.text = src;
    s_source.size = (SceUInt32)strlen(src);
    sceShaccCgInitializeCompileOptions(&opt);
    opt.mainSourceFile = name;
    opt.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    opt.entryFunctionName = "main";
    opt.locale = SCE_SHACCCG_ENGLISH;
    opt.useFx = 1;
    opt.optimizationLevel = 3;
    /* Indices go through floats and back: full precision. */
    opt.useFastmath = 0;
    opt.useFastprecision = 0;
    opt.useFastint = 0;
    sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
    cb.openFile = open_source;
    out = sceShaccCgCompileProgram(&opt, &cb, 0);
    if (out == NULL) {
        printf("gxm: %s: the compiler failed\n", name);
        return NULL;
    }
    for (i = 0; i < out->diagnosticCount; i++) {
        printf("gxm: %s line %u: %s\n", name,
               out->diagnostics[i].location != NULL
               ? (unsigned)out->diagnostics[i].location->lineNumber : 0u,
               out->diagnostics[i].message);
    }
    if (out->programData != NULL && out->programSize != 0) {
        *size = out->programSize;
        data = malloc(*size);
        memcpy(data, out->programData, *size);
    }
    sceShaccCgDestroyCompileOutput(out);
    return data;
}

static u32 fnv1a(const char *s)
{
    u32 h = 2166136261u;

    while (*s != '\0') {
        h = (h ^ (u8)*s++) * 16777619u;
    }
    return h;
}

/* The packaged program, one compiled before, or one compiled now. */
static const SceGxmProgram *load_program(const char *prog, const char *src, int fragment)
{
    char name[64], path[128];
    u32 size = 0;
    u8 *d;

    snprintf(name, sizeof(name), "%s_%c_%08x.gxp", prog, fragment ? 'f' : 'v',
             (unsigned)fnv1a(src));
    snprintf(path, sizeof(path), "app0:shaders/%s", name);
    d = read_file(path, &size);
    if (d == NULL) {
        snprintf(path, sizeof(path), SHADER_DIR "/%s", name);
        d = read_file(path, &size);
    }
    if (d == NULL) {
        d = compile(name, src, fragment, &size);
        if (d != NULL) {
            FILE *f;

            sceIoMkdir("ux0:data/ignition", 0777);
            sceIoMkdir(SHADER_DIR, 0777);
            snprintf(path, sizeof(path), SHADER_DIR "/%s", name);
            f = fopen(path, "wb");
            if (f != NULL) {
                fwrite(d, 1, size, f);
                fclose(f);
            }
            printf("gxm: compiled %s (%u bytes)\n", name, (unsigned)size);
        }
    }
    if (d == NULL) {
        printf("gxm: no %s in the package and no libshacccg.suprx to compile it\n", name);
        return NULL;
    }
    if (sceGxmProgramCheck((const SceGxmProgram *)d) < 0) {
        printf("gxm: %s is not a program\n", name);
        free(d);
        return NULL;
    }
    return (const SceGxmProgram *)d;     /* kept: the patcher refers to it */
}

static int make_program(int p)
{
    SceGxmVertexAttribute attrs[3];
    SceGxmVertexStream stream;
    const SceGxmBlendInfo *blend;
    unsigned n = 0, off = 0;
    int i;

    s_prog[p].vgxp = load_program(s_def[p].name, s_def[p].vs, 0);
    s_prog[p].fgxp = load_program(s_def[p].name, s_def[p].fs, 1);
    if (s_prog[p].vgxp == NULL || s_prog[p].fgxp == NULL
        || sceGxmShaderPatcherRegisterProgram(s_patcher, s_prog[p].vgxp, &s_prog[p].vid) < 0
        || sceGxmShaderPatcherRegisterProgram(s_patcher, s_prog[p].fgxp, &s_prog[p].fid) < 0) {
        return 0;
    }
    for (i = 0; i < 3 && s_def[p].attrs[i].name != NULL; i++) {
        const SceGxmProgramParameter *a =
            sceGxmProgramFindParameterByName(s_prog[p].vgxp, s_def[p].attrs[i].name);

        if (a != NULL) {
            attrs[n].streamIndex = 0;
            attrs[n].offset = (u16)(off * sizeof(float));
            attrs[n].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
            attrs[n].componentCount = (u8)s_def[p].attrs[i].floats;
            attrs[n].regIndex = (u16)sceGxmProgramParameterGetResourceIndex(a);
            n++;
        }
        off += (unsigned)s_def[p].attrs[i].floats;
    }
    {
        /* PARTICLE and LAYER blend, premultiplied: src + (1 - src.a) dst. */
        static const SceGxmBlendInfo premul = {
            SCE_GXM_COLOR_MASK_ALL, SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_ADD,
            SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
        };

        blend = p == RND_PARTICLE || p == RND_LAYER ? &premul : NULL;
    }
    stream.stride = (u16)(s_def[p].stride * sizeof(float));
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(s_patcher, s_prog[p].vid, attrs, n, &stream, 1,
                                               &s_prog[p].vp) < 0
        || sceGxmShaderPatcherCreateFragmentProgram(s_patcher, s_prog[p].fid,
                                                    SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                    SCE_GXM_MULTISAMPLE_NONE, blend,
                                                    s_prog[p].vgxp, &s_prog[p].fp) < 0) {
        printf("gxm: %s: patching failed\n", s_def[p].name);
        return 0;
    }
    for (i = 0; i < UNIFORMS; i++) {
        s_prog[p].uv[i] = sceGxmProgramFindParameterByName(s_prog[p].vgxp, s_uniform_names[i]);
        s_prog[p].uf[i] = sceGxmProgramFindParameterByName(s_prog[p].fgxp, s_uniform_names[i]);
    }
    for (i = 0; i < UNITS; i++) {
        const SceGxmProgramParameter *sp = s_def[p].samplers[i] != NULL
            ? sceGxmProgramFindParameterByName(s_prog[p].fgxp, s_def[p].samplers[i]) : NULL;

        s_prog[p].unit_index[i] = sp != NULL ? (int)sceGxmProgramParameterGetResourceIndex(sp) : -1;
    }
    return 1;
}

/* ------------------------------------------------------------ init */

static void *host_alloc(void *user, SceSize size)
{
    (void)user;
    return malloc(size);
}

static void host_free(void *user, void *mem)
{
    (void)user;
    free(mem);
}

static void *buffer_alloc(void *user, SceSize size)
{
    (void)user;
    return gpu_alloc(size, 16, 0);
}

static void buffer_free(void *user, void *mem)
{
    (void)user;
    gpu_free_now(mem);
}

static void *usse_alloc(int fragment, u32 size, unsigned int *offset)
{
    SceUID uid;
    void *p = NULL;

    size = align_up(size, 4096);
    uid = sceKernelAllocMemBlock("ignition usse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size,
                                 NULL);
    if (uid < 0 || sceKernelGetMemBlockBase(uid, &p) < 0) {
        return NULL;
    }
    if ((fragment ? sceGxmMapFragmentUsseMemory(p, size, offset)
                  : sceGxmMapVertexUsseMemory(p, size, offset)) < 0) {
        return NULL;
    }
    return p;
}

static void *vusse_alloc(void *user, SceSize size, unsigned int *offset)
{
    (void)user;
    return usse_alloc(0, size, offset);
}

static void *fusse_alloc(void *user, SceSize size, unsigned int *offset)
{
    (void)user;
    return usse_alloc(1, size, offset);
}

static void usse_free(void *user, void *mem)
{
    (void)user;
    (void)mem;                  /* programs live as long as the game */
}

/* As much as the system gives, within limits: what is free depends on
 * the firmware, plugins and the newlib heap. */
static int reserve_memory(void)
{
    SceKernelFreeMemorySizeInfo fm;
    const u32 mb = 1024 * 1024;
    u32 v = 112 * mb, r = 48 * mb;

    memset(&fm, 0, sizeof(fm));
    fm.size = sizeof(fm);
    sceKernelGetFreeMemorySize(&fm);
    printf("gxm: free memory: main %d MB, CDRAM %d MB\n", fm.size_user >> 20, fm.size_cdram >> 20);
    /* Room for what GXM still takes for itself (render targets' driver
     * memory, USSE code). */
    if (fm.size_cdram > 0 && (u32)fm.size_cdram < v + 8 * mb) {
        v = (u32)fm.size_cdram > 24 * mb ? ((u32)fm.size_cdram - 8 * mb) & ~(mb - 1) : 16 * mb;
    }
    if (fm.size_user > 0 && (u32)fm.size_user < r + 16 * mb) {
        r = (u32)fm.size_user > 40 * mb ? ((u32)fm.size_user - 16 * mb) & ~(mb - 1) : 24 * mb;
    }
    while (v >= 16 * mb && !heap_init(&s_vram, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, v,
                                      "ignition cdram")) {
        v -= 8 * mb;
    }
    while (r >= 24 * mb && !heap_init(&s_ram, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, r,
                                      "ignition ram")) {
        r -= 8 * mb;
    }
    if (s_vram.base == NULL || s_ram.base == NULL) {
        printf("gxm: could not reserve GPU memory\n");
        return 0;
    }
    printf("gxm: GPU heaps: CDRAM %u MB, main %u MB\n", (unsigned)(v / mb), (unsigned)(r / mb));
    return 1;
}

int rnd_init(void)
{
    SceGxmInitializeParams ip;
    SceGxmContextParams cp;
    SceGxmShaderPatcherParams pp;
    unsigned int usse_off = 0;
    int i, aw, ah;
    void *depth;
    static const float corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    static const u8 zero[4];

    if (s_ctx != NULL) {
        return 1;
    }
    memset(&ip, 0, sizeof(ip));
    ip.displayQueueMaxPendingCount = DISPLAY_BUFFERS - 1;
    ip.displayQueueCallback = display_callback;
    ip.displayQueueCallbackDataSize = sizeof(struct display_data);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (sceGxmInitialize(&ip) < 0) {
        printf("gxm: initialize failed\n");
        return 0;
    }
    if (!reserve_memory()) {
        return 0;
    }

    memset(&cp, 0, sizeof(cp));
    cp.hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = gpu_alloc(SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, 4096, 0);
    cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = gpu_alloc(4 * SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, 4096, 0);
    cp.vertexRingBufferMemSize = 4 * SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = gpu_alloc(4 * SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, 4096, 0);
    cp.fragmentRingBufferMemSize = 4 * SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = usse_alloc(1, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
                                              &usse_off);
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = usse_off;
    if (sceGxmCreateContext(&cp, &s_ctx) < 0) {
        printf("gxm: context failed\n");
        s_ctx = NULL;
        return 0;
    }

    for (i = 0; i < DISPLAY_BUFFERS; i++) {
        s_display[i] = gpu_alloc(SCREEN_W * SCREEN_H * 4, 256 * 1024, 1);
        if (s_display[i] == NULL) {
            return 0;
        }
        memset(s_display[i], 0, SCREEN_W * SCREEN_H * 4);
        sceGxmColorSurfaceInit(&s_display_surface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                               SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                               SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCREEN_W, SCREEN_H, SCREEN_W,
                               s_display[i]);
        sceGxmSyncObjectCreate(&s_display_sync[i]);
    }
    s_back = 0;
    s_front = 0;
    depth = depth_alloc(SCREEN_W, SCREEN_H, &aw, &ah);
    depth_init(&s_display_depth, depth, aw, 0);

    memset(&pp, 0, sizeof(pp));
    pp.hostAllocCallback = host_alloc;
    pp.hostFreeCallback = host_free;
    pp.bufferAllocCallback = buffer_alloc;
    pp.bufferFreeCallback = buffer_free;
    pp.vertexUsseAllocCallback = vusse_alloc;
    pp.vertexUsseFreeCallback = usse_free;
    pp.fragmentUsseAllocCallback = fusse_alloc;
    pp.fragmentUsseFreeCallback = usse_free;
    if (sceGxmShaderPatcherCreate(&pp, &s_patcher) < 0) {
        printf("gxm: shader patcher failed\n");
        return 0;
    }
    for (i = 0; i < PROGRAMS; i++) {
        if (!make_program(i)) {
            printf("gxm: shader programs failed\n");
            return 0;
        }
    }

    for (i = 0; i < FRAMES; i++) {
        s_arena[i].base = gpu_alloc(ARENA_BYTES, 4096, 0);
        if (s_arena[i].base == NULL) {
            return 0;
        }
    }
    s_indices = gpu_alloc((MAX_INDEX + 1) * sizeof(u16), 16, 0);
    s_corners = gpu_alloc(sizeof(corners), 16, 0);
    if (s_indices == NULL || s_corners == NULL) {
        return 0;
    }
    for (i = 0; i <= MAX_INDEX; i++) {
        s_indices[i] = (u16)i;
    }
    memcpy(s_corners, corners, sizeof(corners));

    s_notes = sceGxmGetNotificationRegion();
    for (i = 0; i < FRAMES; i++) {
        s_notes[i] = 0;
    }
    s_dummy = rnd_texture(RND_R8, 1, 1, zero, 1);
    printf("gxm: ready, %d x %d\n", SCREEN_W, SCREEN_H);
    return 1;
}

void rnd_shutdown(void)
{
    if (s_ctx == NULL) {
        return;
    }
    finish();
    sceGxmDisplayQueueFinish();
}

/* ------------------------------------------------------------ textures */

static int bytes_per_texel(int format)
{
    return format == RND_RG8 ? 2 : format == RND_RGB8 || format == RND_RGBA8 ? 4 : 1;
}

static SceGxmTextureFormat gxm_format(int format)
{
    return format == RND_RG8 ? SCE_GXM_TEXTURE_FORMAT_U8U8_GR
           : format == RND_RGB8 || format == RND_RGBA8 ? SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR
           : SCE_GXM_TEXTURE_FORMAT_U8_R;
}

/* Rows of `pixels` into texture memory; RGB8 gains an alpha. */
static void fill(u8 *dst, u32 stride, int format, int w, int h, const u8 *pixels, int pitch)
{
    int y, x, bpp = format == RND_RG8 ? 2 : format == RND_RGB8 ? 3 : format == RND_RGBA8 ? 4 : 1;

    if (pixels == NULL) {
        memset(dst, 0, (size_t)stride * h);
        return;
    }
    if (pitch == 0) {
        pitch = w;
    }
    for (y = 0; y < h; y++) {
        const u8 *s = pixels + (size_t)y * pitch * bpp;
        u8 *d = dst + (size_t)y * stride;

        if (format == RND_RGB8) {
            for (x = 0; x < w; x++) {
                d[4 * x] = s[3 * x];
                d[4 * x + 1] = s[3 * x + 1];
                d[4 * x + 2] = s[3 * x + 2];
                d[4 * x + 3] = 255;
            }
        } else {
            memcpy(d, s, (size_t)w * bpp);
        }
    }
}

static unsigned tex_new(void)
{
    unsigned i;

    for (i = 1; i < s_ntex; i++) {
        if (!s_tex[i].live) {
            break;
        }
    }
    if (i >= s_ntex) {
        unsigned n = s_ntex ? s_ntex * 2 : 256;

        s_tex = realloc(s_tex, n * sizeof(*s_tex));
        memset(s_tex + s_ntex, 0, (n - s_ntex) * sizeof(*s_tex));
        i = s_ntex ? s_ntex : 1;
        s_ntex = n;
    }
    memset(&s_tex[i], 0, sizeof(s_tex[i]));
    s_tex[i].live = 1;
    return i;
}

static u32 stride_of(int format, int w);

static void ring_free(struct texture *r)
{
    int k;

    for (k = 0; k < FRAMES; k++) {
        gpu_free(r->ring[k]);
        free(r->shadow[k]);
        free(r->rowzero[k]);
        r->ring[k] = NULL;
        r->shadow[k] = NULL;
        r->rowzero[k] = NULL;
    }
    r->ring_w = r->ring_h = 0;
    if (r->in_ring) {
        r->data = NULL;
        r->in_ring = 0;
    }
}

/* Fills this frame's buffer of the ring with the rows of `pixels` that
 * differ from what it holds. The buffer was last read by the frame
 * FRAMES ago, which slot_begin waited for. 0 when the ring cannot be
 * had. */
/* Whether n bytes are equal, a word at a time (the C library's memcmp
 * goes a byte at a time here, and a 2D screen has half a megabyte to
 * compare each frame). */
static int same_bytes(const u8 *a, const u8 *b, size_t n)
{
    size_t i = 0;

    for (; i + 16 <= n; i += 16) {
        u32 x[4], y[4];

        memcpy(x, a + i, 16);
        memcpy(y, b + i, 16);
        if (((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) != 0) {
            return 0;
        }
    }
    return memcmp(a + i, b + i, n - i) == 0;
}

/* Whether n bytes are all zero. */
static int is_zero(const u8 *a, size_t n)
{
    size_t i = 0;

    for (; i + 16 <= n; i += 16) {
        u32 x[4];

        memcpy(x, a + i, 16);
        if ((x[0] | x[1] | x[2] | x[3]) != 0) {
            return 0;
        }
    }
    for (; i < n; i++) {
        if (a[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int ring_fill(struct texture *r, int w, int h, const u8 *pixels, int pitch)
{
    u32 stride = stride_of(r->format, w);
    int bpp = r->format == RND_RG8 ? 2 : r->format == RND_RGBA8 ? 4 : 1, k, y;
    u8 *mem, *shadow;

    if (r->ring_w != w || r->ring_h != h) {
        ring_free(r);
        for (k = 0; k < FRAMES; k++) {
            r->ring[k] = gpu_alloc(stride * (u32)h, 256, 0);
            r->shadow[k] = malloc((size_t)w * bpp * h);
            r->rowzero[k] = calloc((size_t)h, 1);
            if (r->ring[k] == NULL || r->shadow[k] == NULL || r->rowzero[k] == NULL) {
                ring_free(r);
                return 0;
            }
            /* A copy that no frame matches, so that all rows go. */
            memset(r->shadow[k], 0xa5, (size_t)w * bpp * h);
        }
        r->ring_w = w;
        r->ring_h = h;
    }
    if (pitch == 0) {
        pitch = w;
    }
    s_stats.ring_fills++;
    s_stats.ring_w = (u32)w;
    s_stats.ring_h = (u32)h;
    mem = r->ring[s_slot];
    shadow = r->shadow[s_slot];
    for (y = 0; y < h; y++) {
        const u8 *src = pixels + (size_t)y * pitch * bpp;
        u8 *was = shadow + (size_t)y * w * bpp;
        u8 *zero = &r->rowzero[s_slot][y];

        /* A 2D screen is mostly empty rows: one that is empty now and was
         * when this buffer was last filled needs no comparing against the
         * copy, which is a second pass over memory. */
        s_stats.rows_total++;
        if (is_zero(src, (size_t)w * bpp)) {
            if (*zero) {
                s_stats.rows_skipped++;
                continue;
            }
            *zero = 1;
        } else {
            *zero = 0;
        }
        if (!same_bytes(was, src, (size_t)w * bpp)) {
            memcpy(was, src, (size_t)w * bpp);
            s_stats.fill_bytes += (u32)(w * bpp);
            s_stats.rows_copied++;
            TIMED(fill_us, memcpy(mem + (size_t)y * stride, src, (size_t)w * bpp));
        }
    }
    if (!r->in_ring) {
        gpu_free(r->data);
    }
    r->data = mem;
    r->in_ring = 1;
    r->w = w;
    r->h = h;
    r->stride = stride;
    return 1;
}

static void tex_init(struct texture *r)
{
    sceGxmTextureInitLinear(&r->t, r->data, gxm_format(r->format), (u32)r->w, (u32)r->h, 0);
    sceGxmTextureSetMinFilter(&r->t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&r->t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&r->t, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&r->t, SCE_GXM_TEXTURE_ADDR_CLAMP);
}

/* Linear textures: rows of a multiple of 8 texels. */
static u32 stride_of(int format, int w)
{
    return align_up((u32)w, 8) * (u32)bytes_per_texel(format);
}

unsigned rnd_texture(int format, int w, int h, const void *pixels, int pitch)
{
    unsigned t = tex_new();
    struct texture *r = &s_tex[t];

    r->format = format;
    r->w = w;
    r->h = h;
    r->stride = stride_of(format, w);
    r->data = gpu_alloc(r->stride * (u32)h, 256, 1);
    if (r->data == NULL) {
        r->live = 0;
        return 0;
    }
    TIMED(fill_us, fill(r->data, r->stride, format, w, h, pixels, pitch));
    r->cols = r->rows = 1;
    tex_init(r);
    return t;
}

void rnd_texture_set(unsigned tex, int w, int h, const void *pixels, int pitch)
{
    s_stats.uploads++;
    struct texture *r = tex_get(tex);
    u8 *data;

    if (r == NULL) {
        return;
    }
    r->alias = 0;
    if (r->drawn && w == r->w && h == r->h) {
        /* The GPU draws into it: fill it in place once it is done. */
        prof_begin(PROF_T_FIN);
        finish();
        prof_end(PROF_T_FIN);
        TIMED(fill_us, fill(r->data, r->stride, r->format, w, h, pixels, pitch));
        return;
    }
    /* Once a frame and big: the ring. */
    if (!r->drawn && pixels != NULL && r->format != RND_RGB8 && w * h >= 0x10000
        && r->set_frame != s_frame_no + 1) {
        r->set_frame = s_frame_no + 1;
        prof_begin(PROF_T_RING);
        if (ring_fill(r, w, h, pixels, pitch)) {
            prof_end(PROF_T_RING);
            tex_init(r);
            return;
        }
        prof_end(PROF_T_RING);
    }
    r->set_frame = s_frame_no + 1;
    /* New memory, as draws still to run may read the old. */
    prof_begin(PROF_T_ALLOC);
    data = gpu_alloc(stride_of(r->format, w) * (u32)h, 256, r->drawn);
    if (data == NULL) {
        return;
    }
    if (!r->in_ring) {
        gpu_free(r->data);
    }
    prof_end(PROF_T_ALLOC);
    r->in_ring = 0;
    r->data = data;
    r->w = w;
    r->h = h;
    r->stride = stride_of(r->format, w);
    prof_begin(PROF_T_FILL);
    TIMED(fill_us, fill(r->data, r->stride, r->format, w, h, pixels, pitch));
    prof_end(PROF_T_FILL);
    tex_init(r);
}

void rnd_texture_free(unsigned tex)
{
    struct texture *r = tex_get(tex);

    if (r == NULL) {
        return;
    }
    if (r->copy_target != 0) {
        rnd_target_free(r->copy_target);
    }
    if (!r->in_ring) {
        gpu_free(r->data);
    }
    ring_free(r);
    memset(r, 0, sizeof(*r));
}

/* Pages as tiles of one texture, up to 16 to a row (4096 texels, the
 * widest GXM takes); u_grid gives the shaders the layout. */
unsigned rnd_pages(const u8 *pixels, int count)
{
    int cols = count < 16 ? count : 16, rows, i;
    unsigned t;
    struct texture *r;

    if (count <= 0) {
        return 0;
    }
    rows = (count + cols - 1) / cols;
    if (rows > 16) {
        fprintf(stderr, "gxm: %d pages, only 256 kept\n", count);
        rows = 16;
        count = 256;
    }
    t = rnd_texture(RND_R8, cols * 256, rows * 256, NULL, 0);
    r = tex_get(t);
    if (r == NULL) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        int y;
        u8 *d = r->data + (size_t)(i / cols) * 256 * r->stride + (size_t)(i % cols) * 256;
        const u8 *s = pixels + (size_t)i * 256 * 256;

        for (y = 0; y < 256; y++) {
            memcpy(d + (size_t)y * r->stride, s + (size_t)y * 256, 256);
        }
    }
    r->cols = cols;
    r->rows = rows;
    return t;
}

void rnd_texture_size(unsigned tex, int *w, int *h)
{
    struct texture *r = tex_get(tex);

    *w = r != NULL ? r->w : 1;
    *h = r != NULL ? r->h : 1;
}

/* ------------------------------------------------------------ targets */

unsigned rnd_target(unsigned color, int depth)
{
    unsigned i;
    struct texture *c = tex_get(color);

    if (c == NULL) {
        return 0;
    }
    for (i = 1; i < s_ntgt; i++) {
        if (!s_tgt[i].live) {
            break;
        }
    }
    if (i >= s_ntgt) {
        unsigned n = s_ntgt ? s_ntgt * 2 : 64;

        s_tgt = realloc(s_tgt, n * sizeof(*s_tgt));
        memset(s_tgt + s_ntgt, 0, (n - s_ntgt) * sizeof(*s_tgt));
        i = s_ntgt ? s_ntgt : 1;
        s_ntgt = n;
    }
    memset(&s_tgt[i], 0, sizeof(s_tgt[i]));
    s_tgt[i].live = 1;
    s_tgt[i].color = color;
    s_tgt[i].depth = depth;
    s_tgt[i].depth_fresh = 1;
    c->drawn = 1;
    return i;
}

unsigned rnd_target_shared(unsigned color, unsigned other)
{
    unsigned t = rnd_target(color, 0);

    if (t != 0) {
        s_tgt[t].depth_of = other;
    }
    return t;
}

/* PARTICLE and LAYER are patched with the blend: always on for them. */
void rnd_blend(int mode)
{
    (void)mode;
}

void rnd_target_free(unsigned target)
{
    struct target *g = tgt_get(target);

    if (g == NULL) {
        return;
    }
    if (s_in_scene && s_scene == target) {
        end_scene();
    }
    gpu_free(g->depth_data);
    memset(g, 0, sizeof(*g));
}

void rnd_bind(unsigned target)
{
    s_bound = target;
}

void rnd_bind_screen(int w, int h)
{
    s_bound = 0;
    s_screen_w = w;
    s_screen_h = h;
}

/* ------------------------------------------------------------ drawing */

static void set_uniforms(int p)
{
    void *vb = NULL, *fb = NULL;
    int i;

    /* The buffers reserved last serve the draws after them. */
    if (s_cur.uprog == p && memcmp(s_cur.u, s_prog[p].u, sizeof(s_cur.u)) == 0) {
        return;
    }
    s_cur.uprog = p;
    memcpy(s_cur.u, s_prog[p].u, sizeof(s_cur.u));

    if (sceGxmProgramGetDefaultUniformBufferSize(s_prog[p].vgxp) > 0) {
        sceGxmReserveVertexDefaultUniformBuffer(s_ctx, &vb);
    }
    if (sceGxmProgramGetDefaultUniformBufferSize(s_prog[p].fgxp) > 0) {
        sceGxmReserveFragmentDefaultUniformBuffer(s_ctx, &fb);
    }
    for (i = 0; i < UNIFORMS; i++) {
        if (vb != NULL && s_prog[p].uv[i] != NULL) {
            sceGxmSetUniformDataF(vb, s_prog[p].uv[i], 0, (u32)s_uniform_floats[i], s_prog[p].u[i]);
        }
        if (fb != NULL && s_prog[p].uf[i] != NULL) {
            sceGxmSetUniformDataF(fb, s_prog[p].uf[i], 0, (u32)s_uniform_floats[i], s_prog[p].u[i]);
        }
    }
}

/* The uniforms the shaders need that OpenGL knows by itself: texture
 * sizes, the pages' layout, the scissor. */
static void derive_uniforms(int p)
{
    float *u;
    struct texture *t;

    u = s_prog[p].u[U_CLIP];
    if (s_scissor.on) {
        u[0] = (float)s_scissor.x;
        u[1] = (float)s_scissor.y;
        u[2] = (float)(s_scissor.x + s_scissor.w);
        u[3] = (float)(s_scissor.y + s_scissor.h);
    } else {
        u[0] = u[1] = -1e9f;
        u[2] = u[3] = 1e9f;
    }
    t = tex_get(s_units[0]);
    u = s_prog[p].u[U_SRC_SIZE];
    u[0] = t != NULL ? (float)t->w : 1.0f;
    u[1] = t != NULL ? (float)t->h : 1.0f;
    u = s_prog[p].u[U_GRID];
    u[0] = t != NULL ? (float)t->cols : 1.0f;
    u[1] = 1.0f / (u[0] * 256.0f);
    u[2] = 1.0f / ((t != NULL ? (float)t->rows : 1.0f) * 256.0f);
    t = read_as(tex_get(s_units[p == RND_CANVAS ? 3 : 2]));
    u = s_prog[p].u[U_DST_SIZE];
    u[0] = t != NULL ? (float)t->w : 1.0f;
    u[1] = t != NULL ? (float)t->h : 1.0f;
}

static void set_state(int depth_func, int depth_write, int cull, int offset)
{
    static const SceGxmDepthFunc funcs[3] = {
        SCE_GXM_DEPTH_FUNC_ALWAYS, SCE_GXM_DEPTH_FUNC_LESS, SCE_GXM_DEPTH_FUNC_LESS_EQUAL
    };
    SceGxmCullMode mode = SCE_GXM_CULL_NONE;

    if (s_cur.valid && s_cur.depth_func == depth_func && s_cur.depth_write == depth_write
        && s_cur.cull == cull && s_cur.offset == offset && s_cur.screen == (s_scene == 0)) {
        return;
    }
    s_cur.valid = 1;
    s_cur.depth_func = depth_func;
    s_cur.depth_write = depth_write;
    s_cur.cull = cull;
    s_cur.offset = offset;
    s_cur.screen = s_scene == 0;

    sceGxmSetFrontDepthFunc(s_ctx, funcs[depth_func]);
    sceGxmSetFrontDepthWriteEnable(s_ctx, depth_func != RND_DEPTH_OFF && depth_write
                                          ? SCE_GXM_DEPTH_WRITE_ENABLED
                                          : SCE_GXM_DEPTH_WRITE_DISABLED);
    /* A target is drawn the way up its memory is, so OpenGL's windings
     * hold as they are; GXM names them seen from its window, y down. */
    if (cull != RND_CULL_OFF) {
        int ccw_dropped = cull == RND_CULL_CCW;

        if (s_scene == 0) {
            ccw_dropped = !ccw_dropped;     /* the screen is upside down */
        }
        mode = ccw_dropped ? SCE_GXM_CULL_CW : SCE_GXM_CULL_CCW;
    }
    sceGxmSetCullMode(s_ctx, mode);
    sceGxmSetFrontDepthBias(s_ctx, offset ? -1 : 0, offset ? -4 : 0);
    sceGxmSetFrontPolygonMode(s_ctx, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
}

static void bind_textures(int p, const unsigned *units)
{
    int i;

    for (i = 0; i < UNITS; i++) {
        struct texture *t;

        if (s_prog[p].unit_index[i] < 0) {
            continue;
        }
        t = read_as(tex_get(units[i]));
        if (t == NULL) {
            t = tex_get(s_dummy);
        }
        if (s_cur.tex[i] == t && s_cur.data[i] == t->data) {
            continue;
        }
        s_cur.tex[i] = t;
        s_cur.data[i] = t->data;
        sceGxmSetFragmentTexture(s_ctx, (u32)s_prog[p].unit_index[i], &t->t);
    }
}

/* Draws `count` vertices of `verts` (program p's layout) into the bound
 * target. */
static void draw(int p, const void *verts, int prim, int count, const unsigned *units,
                 int depth_func, int depth_write, int cull, int offset)
{
    const u8 *v = verts;
    u32 stride = (u32)s_def[p].stride * sizeof(float);

    if (count <= 0 || verts == NULL || !scene_on(s_bound)) {
        return;
    }
    set_state(depth_func, depth_write, cull, offset);
    while (count > 0) {
        /* Triangles split at a multiple of three. */
        int n = count > MAX_INDEX ? MAX_INDEX : count;

        if (s_cur.prog != p) {
            /* The uniforms' buffers are sized by the programs set. */
            sceGxmSetVertexProgram(s_ctx, s_prog[p].vp);
            sceGxmSetFragmentProgram(s_ctx, s_prog[p].fp);
            s_cur.prog = p;
            s_cur.uprog = -1;
            memset(s_cur.tex, 0, sizeof(s_cur.tex));
        }
        set_uniforms(p);
        bind_textures(p, units);
        sceGxmSetVertexStream(s_ctx, 0, v);
        s_stats.draws++;
        TIMED(draw_us, sceGxmDraw(s_ctx, prim == RND_STRIP ? SCE_GXM_PRIMITIVE_TRIANGLE_STRIP
                                           : SCE_GXM_PRIMITIVE_TRIANGLES,
                   SCE_GXM_INDEX_FORMAT_U16, s_indices, (u32)n));
        v += (size_t)n * stride;
        count -= n;
    }
}

/* A quad over the clip rectangle x0, y0, x1, y1 for CLEAR (u ignored)
 * or COPY, with texel coordinates u0, v0, u1, v1. */
static const float *quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                         float v1, int with_uv)
{
    float *q = arena_alloc(16 * sizeof(float));
    int k = 0;

    if (q == NULL) {
        return NULL;
    }
#define CORNER(x, y, u, v) \
    do { q[k++] = (x); q[k++] = (y); if (with_uv) { q[k++] = (u); q[k++] = (v); } } while (0)
    CORNER(x0, y0, u0, v0);
    CORNER(x1, y0, u1, v0);
    CORNER(x0, y1, u0, v1);
    CORNER(x1, y1, u1, v1);
#undef CORNER
    return q;
}

void rnd_clear(const float *rgba, int depth)
{
    struct target *g = s_bound != 0 ? tgt_get(s_bound) : NULL;

    if (depth && g != NULL && g->depth) {
        /* A scene that begins without loading it. */
        if (s_in_scene && s_scene == s_bound) {
            end_scene();
        }
        g->depth_fresh = 1;
    }
    if (rgba != NULL) {
        static const unsigned none[UNITS];
        const float *q = quad(-1, -1, 1, 1, 0, 0, 0, 0, 0);

        memcpy(s_prog[P_CLEAR].u[U_COLOR], rgba, 4 * sizeof(float));
        derive_uniforms(P_CLEAR);
        draw(P_CLEAR, q, RND_STRIP, 4, none, RND_DEPTH_OFF, 0, RND_CULL_OFF, 0);
    } else if (depth && g != NULL && g->depth) {
        scene_on(s_bound);
    }
}

void rnd_read(int x, int y, int w, int h, int format, void *out, int pitch)
{
    s_stats.reads++;
    int row;

    finish();
    if (s_bound == 0) {
        /* The screen, bottom row first as OpenGL reads it. */
        const u8 *src = s_display[s_back];

        for (row = 0; row < h; row++) {
            const u8 *s = src + ((size_t)(SCREEN_H - 1 - (y + row)) * SCREEN_W + x) * 4;
            u8 *d = (u8 *)out + (size_t)row * pitch;
            int i;

            for (i = 0; i < w; i++) {
                d[3 * i] = s[4 * i];
                d[3 * i + 1] = s[4 * i + 1];
                d[3 * i + 2] = s[4 * i + 2];
            }
        }
        return;
    }
    {
        struct target *g = tgt_get(s_bound);
        struct texture *c = g != NULL ? tex_get(g->color) : NULL;
        int bpp = bytes_per_texel(c != NULL ? c->format : RND_R8);
        int want = format == RND_RG8 ? 2 : 1;

        if (c == NULL) {
            return;
        }
        for (row = 0; row < h; row++) {
            const u8 *s = c->data + (size_t)(y + row) * c->stride + (size_t)x * bpp;
            u8 *d = (u8 *)out + (size_t)row * pitch;

            if (bpp == want) {
                memcpy(d, s, (size_t)w * bpp);
            } else {
                int i;

                for (i = 0; i < w; i++) {
                    d[want * i] = s[bpp * i];
                    if (want == 2) {
                        d[2 * i + 1] = 0;
                    }
                }
            }
        }
    }
}

/* Draws all of texture `src` (sw x sh) over the rectangle x, y, w, h of
 * target `dst`'s pixels, nearest, as a scene of its own. */
static void copy_into(unsigned dst, unsigned src, int dw, int dh, int x, int y, int w, int h,
                      float u0, float v0, float u1, float v1)
{
    unsigned units[UNITS] = { 0 };
    unsigned saved = s_bound;
    const float *q;
    float *sz;
    struct texture *t = tex_get(src);

    if (t == NULL) {
        return;
    }
    units[0] = src;
    q = quad(2.0f * x / dw - 1.0f, 2.0f * y / dh - 1.0f, 2.0f * (x + w) / dw - 1.0f,
             2.0f * (y + h) / dh - 1.0f, u0, v0, u1, v1, 1);
    sz = s_prog[P_COPY].u[U_SRC_SIZE];
    sz[0] = (float)t->w;
    sz[1] = (float)t->h;
    s_bound = dst;
    end_scene();
    draw(P_COPY, q, RND_STRIP, 4, units, RND_DEPTH_OFF, 0, RND_CULL_OFF, 0);
    end_scene();
    s_bound = saved;
}

/* A copy is only ever read as the pixels under a blend. The GPU draws a
 * scene in its own tile memory and writes it out when the scene ends, so
 * a scene that reads its target as a texture sees the target as the
 * scene began: what a copy taken then holds. So `tex` reads the target
 * itself, and the scene ends, so that what was drawn so far is written
 * out. This costs one scene, where a copy costs two and the copy's
 * drawing. */
void rnd_copy(unsigned tex, int x, int y, int w, int h)
{
    s_stats.copies++;
    struct texture *d = tex_get(tex);
    struct target *g = tgt_get(s_bound);

    (void)x;
    (void)y;
    if (d == NULL || g == NULL || w <= 0 || h <= 0) {
        return;
    }
    d->alias = g->color;
    if (s_in_scene && s_scene == s_bound) {
        end_scene();
    }
}

void rnd_blit(unsigned src, unsigned dst, int sw, int sh, int dw, int dh)
{
    struct target *s = tgt_get(src);

    if (s == NULL || tgt_get(dst) == NULL) {
        return;
    }
    copy_into(dst, s->color, dw, dh, 0, 0, dw, dh, 0, 0, (float)sw, (float)sh);
}

void rnd_scissor(int on, int x, int y, int w, int h)
{
    s_scissor.on = on;
    s_scissor.x = x;
    s_scissor.y = y;
    s_scissor.w = w;
    s_scissor.h = h;
}

void rnd_depth(int func, int write)
{
    s_depth_func = func;
    s_depth_write = write;
}

void rnd_cull(int mode)
{
    s_cull = mode;
}

void rnd_offset(int on)
{
    s_offset = on;
}

void rnd_use(int program)
{
    s_use = program;
}

void rnd_uniform(int id, const float *v)
{
    memcpy(s_prog[s_use].u[id], v, (size_t)s_uniform_floats[id] * sizeof(float));
}

void rnd_texture_unit(int unit, unsigned tex)
{
    s_units[unit] = tex;
}

void rnd_stream(const float *verts, int count)
{
    u32 bytes = (u32)count * (u32)s_def[s_use].stride * sizeof(float);
    void *p;

    s_stream = NULL;
    p = arena_alloc(bytes);
    if (p != NULL) {
        s_stats.stream_bytes += bytes;
        TIMED(stream_us, memcpy(p, verts, bytes));
    }
    s_stream = p;
    s_stream_bytes = bytes;
}

void rnd_draw(int prim, int first, int count)
{
    const u8 *v;

    if (s_use == RND_PRESENT || s_use == RND_OVER || s_use == RND_LAYER) {
        v = (const u8 *)s_corners;
    } else if (s_stream == NULL) {
        return;
    } else {
        v = (const u8 *)s_stream + (size_t)first * s_def[s_use].stride * sizeof(float);
    }
    derive_uniforms(s_use);
    draw(s_use, v, prim, count, s_units, s_depth_func, s_depth_write, s_cull, s_offset);
}

void rnd_mesh_store(int count)
{
    int i;

    free(s_mesh);
    s_mesh = calloc((size_t)count * 8, sizeof(float));
    for (i = 0; i < FRAMES; i++) {
        gpu_free(s_mesh_gpu[i]);
        s_mesh_gpu[i] = gpu_alloc((u32)count * 8 * sizeof(float), 16, 1);
        if (s_mesh_gpu[i] != NULL) {
            memset(s_mesh_gpu[i], 0, (size_t)count * 8 * sizeof(float));
        }
        s_mesh_lo[i] = s_mesh_hi[i] = 0;
    }
    s_mesh_count = count;
}

void rnd_mesh_write(int first, int count, const float *verts)
{
    int i;

    if (s_mesh == NULL || first < 0 || first + count > s_mesh_count || count <= 0) {
        return;
    }
    memcpy(s_mesh + (size_t)first * 8, verts, (size_t)count * 8 * sizeof(float));
    if (s_mesh_gpu[s_slot] != NULL) {
        s_stats.mesh_bytes += (u32)count * 8 * sizeof(float);
        TIMED(mesh_us, memcpy((float *)s_mesh_gpu[s_slot] + (size_t)first * 8, verts,
                              (size_t)count * 8 * sizeof(float)));
    }
    /* The other frames' copies take it when their frame comes round. */
    for (i = 0; i < FRAMES; i++) {
        if (i == s_slot) {
            continue;
        }
        if (s_mesh_hi[i] <= s_mesh_lo[i]) {
            s_mesh_lo[i] = first;
            s_mesh_hi[i] = first + count;
        } else {
            s_mesh_lo[i] = first < s_mesh_lo[i] ? first : s_mesh_lo[i];
            s_mesh_hi[i] = first + count > s_mesh_hi[i] ? first + count : s_mesh_hi[i];
        }
    }
}

void rnd_draw_mesh(int first, int count)
{
    if (s_mesh_gpu[s_slot] == NULL) {
        return;
    }
    derive_uniforms(RND_MESH);
    draw(RND_MESH, (const float *)s_mesh_gpu[s_slot] + (size_t)first * 8, RND_TRIANGLES, count,
         s_units, s_depth_func, s_depth_write, s_cull, s_offset);
}

/* Each copy ends a scene and begins two: faces share copies. */
int rnd_batch_copies(void)
{
    return 1;
}
