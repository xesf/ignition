/*
 * The DYN fast path in src/gl3d.c: no copies, stable far-to-near order,
 * and batching only consecutive faces that share texture and table.
 *
 * With IGN_GLES_DYN_ARENA=1 the whole fetch pass is uploaded once and the
 * runs draw from its offsets. The same file builds with the flag off to
 * keep the old one-stream-per-run behavior as the regression control.
 *
 * The test includes the production file to drive its static draw_dyns
 * directly with recording rnd stubs. Pixel equivalence belongs to the
 * device fixture.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IGN_GLES 1

#if defined(IGN_GLES_DYN_ARENA) && IGN_GLES_DYN_ARENA == 1
#define ARENA 1
#else
#define ARENA 0
#endif

/* Staging growth runs through this so a test can fail it safely. */
static int s_realloc_budget = -1;

void *test_realloc(void *p, size_t n)
{
    if (s_realloc_budget == 0) {
        return NULL;
    }
    if (s_realloc_budget > 0) {
        s_realloc_budget--;
    }
    return realloc(p, n);
}

#define realloc test_realloc

static int s_fetch_active;
static int s_batch_copies;

int rnd_dyn_framebuffer_fetch(void)
{
    return s_fetch_active;
}

int rnd_batch_copies(void)
{
    return s_batch_copies;
}

#include "../src/gl3d.c"

/* The software 3D boundary. The production path includes soft3d.h but the
 * tests drive the GL path; if --soft3d is entered here, the fixture is wrong. */
int g_soft3d;

void soft3d_raster(u8 *target, int w, int h, int pitch, const struct raster_cmd *cmds, int count,
                   const u8 *pages)
{
    (void)target;
    (void)w;
    (void)h;
    (void)pitch;
    (void)cmds;
    (void)count;
    (void)pages;
    fprintf(stderr, "FAIL soft3d_raster: software 3D entered from the GL fixture\n");
    abort();
}

enum {
    MAX_EVENTS = 16384,
    MAX_STREAM = 256,
    MAX_TRACKED_TEXTURES = 64,
    MAX_PASS = MAX_DYN * 6
};

u8 *g_scene_textures[SCENE_TEXTURES];
int g_scene_texture_pages[SCENE_TEXTURES];

void scene_view_rows(const struct scene *sc, double rows[3][3])
{
    memset(rows, 0, sizeof(double) * 9);
    rows[0][0] = rows[1][1] = rows[2][2] = 1.0;
    (void)sc;
}

void scene_obj_rows(const struct scene_obj *o, double rows[3][3])
{
    memset(rows, 0, sizeof(double) * 9);
    rows[0][0] = rows[1][1] = rows[2][2] = 1.0;
    (void)o;
}

int canvas_is_gpu(const u8 *key)
{
    (void)key;
    return 0;
}

void canvas_extent(const u8 *key, float *mx, float *my)
{
    (void)key;
    *mx = 0.0f;
    *my = 0.0f;
}

unsigned canvas_bind(u8 *key, int *pw, int *ph, int clear_depth)
{
    (void)key;
    (void)clear_depth;
    *pw = 64;
    *ph = 64;
    return 1;
}

unsigned canvas_snapshot(const u8 *key)
{
    (void)key;
    return 1;
}

unsigned canvas_snapshot_rect(const u8 *key, float x0, float y0, float x1, float y1)
{
    (void)key;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    return 1;
}

struct draw_rec {
    int first;
    int vertices;
    float first_uv;
    float at_uv;
    unsigned tex;
    int table;
    int unit1;
    int vertex_bytes;
    int pass_bytes;
    int depth_write;
    int offset;
    int copies_before;
    int passes_before;
    int streams_before;
};

static struct draw_rec s_draws[MAX_EVENTS];
static int s_draw_count;
static int s_copy_count;
static float s_stream[MAX_STREAM * DYN_FLOATS];
static int s_stream_count;
static int s_stream_vert_count;
static int s_stream_truncated;
static float s_pass[MAX_PASS * DYN_FLOATS];
static int s_pass_count;
static int s_pass_vert_count;
static int s_pass_bytes;
static int s_pass_truncated;
static int s_legacy_stream_calls;
static unsigned s_texture_ids[MAX_TRACKED_TEXTURES];
static int s_texture_count;
static int s_texture_free_count;
static int s_texture_unit0, s_texture_unit1, s_texture_unit2;
static int s_use_table_uniform;
static int s_depth_calls, s_offset_calls;
static int s_last_depth_func, s_last_depth_write, s_last_offset;
static int s_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++; \
        } \
    } while (0)

unsigned rnd_texture(int format, int w, int h, const void *pixels, int pitch)
{
    unsigned id = 700u + (unsigned)s_texture_count;

    (void)format;
    (void)w;
    (void)h;
    (void)pixels;
    (void)pitch;
    if (s_texture_count < MAX_TRACKED_TEXTURES) {
        s_texture_ids[s_texture_count] = id;
    }
    s_texture_count++;
    return id;
}

void rnd_texture_free(unsigned tex)
{
    (void)tex;
    s_texture_free_count++;
}

unsigned rnd_pages(const u8 *pixels, int count)
{
    (void)pixels;
    (void)count;
#ifdef PERF_PAGE_TEXTURE_ID
    return PERF_PAGE_TEXTURE_ID;
#else
    return 900u;
#endif
}

void rnd_copy(unsigned tex, int x, int y, int w, int h)
{
    (void)tex;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    s_copy_count++;
}

void rnd_texture_unit(int unit, unsigned tex)
{
    if (unit == 0) {
        s_texture_unit0 = (int)tex;
    } else if (unit == 1) {
        s_texture_unit1 = (int)tex;
    } else if (unit == 2) {
        s_texture_unit2 = (int)tex;
    }
}

void rnd_depth(int func, int write)
{
    s_depth_calls++;
    s_last_depth_func = func;
    s_last_depth_write = write;
}

void rnd_offset(int on)
{
    s_offset_calls++;
    s_last_offset = on;
}

void rnd_uniform(int id, const float *v)
{
    if (id == RND_U_USE_TABLE) {
        s_use_table_uniform = (int)v[0];
    }
}

void rnd_use(int program)
{
    (void)program;
}

void rnd_cull(int mode)
{
    (void)mode;
}

void rnd_stream(const float *verts, int count)
{
    int copy = count < MAX_STREAM ? count : MAX_STREAM;
    size_t floats = (size_t)copy * DYN_FLOATS;

    s_legacy_stream_calls++;
    CHECK(floats <= sizeof(s_stream) / sizeof(s_stream[0]));
    s_stream_vert_count = count;
    if (floats > sizeof(s_stream) / sizeof(s_stream[0])) {
        s_stream_truncated = 1;
        s_stream_count = 0;
        return;
    }
    memcpy(s_stream, verts, floats * sizeof(float));
    s_stream_count = copy;
}

void rnd_stream_pass(const float *verts, int count)
{
    size_t floats = (size_t)count * DYN_FLOATS;

#if defined(ARENA_DEBUG)
    {
        int q;

        fprintf(stderr, "pass count=%d", count);
        for (q = 0; q < count; q++) {
            fprintf(stderr, " %g", verts[(size_t)q * DYN_FLOATS + 4]);
        }
        fprintf(stderr, "\n");
    }
#endif
    s_pass_count++;
    s_pass_vert_count = count;
    s_pass_bytes = (int)(floats * sizeof(float));
    if (floats > sizeof(s_pass) / sizeof(s_pass[0])) {
        s_pass_truncated = 1;
        s_pass_bytes = 0;
        return;
    }
    memcpy(s_pass, verts, floats * sizeof(float));
}

void rnd_draw(int prim, int first, int count)
{
    struct draw_rec *d;

    (void)prim;
    if (s_draw_count >= MAX_EVENTS) {
        return;
    }
    d = &s_draws[s_draw_count++];
    d->first = first;
    d->vertices = count;
    d->first_uv = s_stream_count > 0 ? s_stream[4] : -1.0f;
    d->at_uv = s_pass_count > 0 && first >= 0 && first < s_pass_vert_count
                   ? s_pass[(size_t)first * DYN_FLOATS + 4]
                   : -1.0f;
    d->tex = (unsigned)s_texture_unit0;
    d->table = s_use_table_uniform;
    d->unit1 = s_texture_unit1;
    d->vertex_bytes = count * DYN_FLOATS * (int)sizeof(float);
    d->pass_bytes = s_pass_bytes;
    d->offset = s_last_offset;
    d->depth_write = s_last_depth_write;
    d->copies_before = s_copy_count;
    d->passes_before = s_pass_count;
    d->streams_before = s_legacy_stream_calls;
#ifdef PERF_STREAM_DRAW_OBSERVER
    PERF_STREAM_DRAW_OBSERVER(first, count);
#endif
}

void rnd_draw_mesh(int first, int count)
{
    (void)first;
    (void)count;
}

void rnd_mesh_store(int count)
{
    (void)count;
}

void rnd_mesh_write(int first, int count, const float *verts)
{
    (void)first;
    (void)count;
    (void)verts;
}

void rnd_read(int x, int y, int w, int h, int format, void *out, int pitch)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)format;
    (void)out;
    (void)pitch;
}

void rnd_scissor(int on, int x, int y, int w, int h)
{
    (void)on;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

unsigned rnd_target(unsigned color, int depth)
{
    (void)color;
    (void)depth;
    return 1;
}

void rnd_target_free(unsigned target)
{
    (void)target;
}

void rnd_bind(unsigned target)
{
    (void)target;
}

void rnd_bind_screen(int w, int h)
{
    (void)w;
    (void)h;
}

void rnd_clear(const float *rgba, int depth)
{
    (void)rgba;
    (void)depth;
}

void rnd_blit(unsigned src, unsigned dst, int sw, int sh, int dw, int dh)
{
    (void)src;
    (void)dst;
    (void)sw;
    (void)sh;
    (void)dw;
    (void)dh;
}

void rnd_texture_set(unsigned tex, int w, int h, const void *pixels, int pitch)
{
    (void)tex;
    (void)w;
    (void)h;
    (void)pixels;
    (void)pitch;
}

void rnd_texture_size(unsigned tex, int *w, int *h)
{
    (void)tex;
    *w = 64;
    *h = 64;
}

void rnd_present(void)
{
}

static void reset_calls(void)
{
    memset(s_draws, 0, sizeof(s_draws));
    memset(s_stream, 0, sizeof(s_stream));
    memset(s_pass, 0, sizeof(s_pass));
    s_draw_count = 0;
    s_copy_count = 0;
    s_stream_count = 0;
    s_stream_vert_count = 0;
    s_stream_truncated = 0;
    s_pass_count = 0;
    s_pass_vert_count = 0;
    s_pass_bytes = 0;
    s_pass_truncated = 0;
    s_legacy_stream_calls = 0;
    s_texture_count = 0;
    s_texture_free_count = 0;
    s_texture_unit0 = s_texture_unit1 = s_texture_unit2 = 0;
    s_use_table_uniform = 0;
    s_depth_calls = 0;
    s_offset_calls = 0;
    s_last_depth_func = -1;
    s_last_depth_write = -1;
    s_last_offset = -1;
}

static void free_staging(void)
{
    free(s_batch);
    s_batch = NULL;
    s_batch_cap = 0;
}

static void face(int i, float depth, unsigned tex, const u8 *table, float uv)
{
    static const float px[3] = { -0.5f, 0.5f, 0.0f };
    static const float py[3] = { -0.5f, -0.5f, 0.5f };
    struct dyn *d = &s_dyns[i];
    int k;

    CHECK(i < MAX_DYN);
    memset(d, 0, sizeof(*d));
    d->count = 3;
    d->depth = depth;
    d->tex = tex;
    d->table = table;
    for (k = 0; k < 3; k++) {
        d->v[k][0] = px[k];
        d->v[k][1] = py[k];
        d->v[k][2] = 0.0f;
        d->v[k][3] = 1.0f;
        d->v[k][4] = uv;
    }
}

static void offscreen_face(int i, float depth, unsigned tex, const u8 *table,
                           float uv, float w)
{
    struct dyn *d = &s_dyns[i];
    int k;

    face(i, depth, tex, table, uv);
    d = &s_dyns[i];
    for (k = 0; k < 3; k++) {
        d->v[k][0] = 2.0f + (float)k;
        d->v[k][1] = 2.0f;
        d->v[k][3] = w;
    }
}

static void face6(int i, float depth, unsigned tex, const u8 *table, float uv)
{
    static const float px[6] = { -0.5f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f };
    static const float py[6] = { -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f };
    struct dyn *d = &s_dyns[i];
    int k;

    CHECK(i < MAX_DYN);
    memset(d, 0, sizeof(*d));
    d->count = 6;
    d->depth = depth;
    d->tex = tex;
    d->table = table;
    for (k = 0; k < 6; k++) {
        d->v[k][0] = px[k];
        d->v[k][1] = py[k];
        d->v[k][3] = 1.0f;
        d->v[k][4] = uv;
    }
}

static void prepare(int count)
{
    memset(s_dyns, 0, sizeof(s_dyns));
    memset(s_order, 0, sizeof(s_order));
    s_dyn_count = count;
    s_w = 64;
    s_h = 64;
    s_dyn_dst = 0;
    s_dyn_dst_w = 0;
    s_dyn_dst_h = 0;
}

static void two_table_faces(void)
{
    static const u8 table_a[256 * 256];

    memset(s_dyns, 0, sizeof(s_dyns));
    s_dyn_count = 2;
    face(0, 30.0f, 41, table_a, 1.0f);
    face(1, 20.0f, 41, table_a, 2.0f);
}

static void test_empty(void)
{
    int started = s_failures;

    prepare(0);
    reset_calls();
    s_fetch_active = 1;
    draw_dyns();

    CHECK(s_copy_count == 0);
    CHECK(s_draw_count == 0);
    CHECK(s_dyn_dst == 0);
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 0);
    if (s_failures == started) {
        puts("empty DYN list                   ok");
    }
}

static void test_fast_grouping(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(5);
    face(0, 20.0f, 11, table_a, 1.0f);
    face(1, 30.0f, 11, table_a, 2.0f);
    face(2, 10.0f, 12, table_a, 3.0f);
    face(3, 5.0f, 11, table_a, 4.0f);
    face(4, 30.0f, 11, table_a, 5.0f);
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_copy_count == 0);
    CHECK(s_dyn_dst == 0);
    CHECK(s_draw_count == 3);
    if (s_draw_count == 3) {
        CHECK(s_draws[0].vertices == 9);
#if ARENA
        CHECK(s_draws[0].at_uv == 2.0f);
#else
        CHECK(s_draws[0].first_uv == 2.0f);
#endif
        CHECK(s_draws[0].tex == 11);
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[0].depth_write == 0);
        CHECK(s_draws[0].offset == 1);
        CHECK(s_draws[1].vertices == 3);
#if ARENA
        CHECK(s_draws[1].at_uv == 3.0f);
#else
        CHECK(s_draws[1].first_uv == 3.0f);
#endif
        CHECK(s_draws[1].tex == 12);
        CHECK(s_draws[2].vertices == 3);
#if ARENA
        CHECK(s_draws[2].at_uv == 4.0f);
#else
        CHECK(s_draws[2].first_uv == 4.0f);
#endif
        CHECK(s_draws[2].tex == 11);
#if ARENA
        CHECK(s_draws[0].first == 0);
        CHECK(s_draws[1].first == 9);
        CHECK(s_draws[2].first == 12);
#endif
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(!s_pass_truncated);
    CHECK(s_pass_vert_count == 15);
    if (s_pass_vert_count == 15) {
        CHECK(s_pass[4] == 2.0f);
        CHECK(s_pass[9 * DYN_FLOATS + 4] == 3.0f);
        CHECK(s_pass[12 * DYN_FLOATS + 4] == 4.0f);
    }
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 3);
#endif
    CHECK(s_last_offset == 0);
    CHECK(s_last_depth_func == RND_DEPTH_OFF);
    CHECK(s_last_depth_write == 1);
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("fast grouping                    ok");
    }
}

static void test_fast_mixed_depth(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(3);
    face(0, 30.0f, 21, table_a, 1.0f);
    face(1, 20.0f, 21, NULL, 2.0f);
    face(2, 10.0f, 21, NULL, 3.0f);
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_copy_count == 0);
    CHECK(s_draw_count == 2);
    if (s_draw_count == 2) {
        CHECK(s_draws[0].vertices == 3);
#if ARENA
        CHECK(s_draws[0].at_uv == 1.0f);
#else
        CHECK(s_draws[0].first_uv == 1.0f);
#endif
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[0].depth_write == 0);
        CHECK(s_draws[1].vertices == 6);
#if ARENA
        CHECK(s_draws[1].at_uv == 2.0f);
#else
        CHECK(s_draws[1].first_uv == 2.0f);
#endif
        CHECK(s_draws[1].table == 0);
        CHECK(s_draws[1].depth_write == 1);
#if ARENA
        CHECK(s_draws[0].first == 0);
        CHECK(s_draws[1].first == 3);
#endif
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_pass_vert_count == 9);
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 2);
#endif
    CHECK(s_last_offset == 0);
    CHECK(s_last_depth_func == RND_DEPTH_OFF);
    CHECK(s_last_depth_write == 1);
    if (s_failures == started) {
        puts("fast mixed depth order           ok");
    }
}

static void test_fast_long_group(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;
    int i;

    prepare(10);
    for (i = 0; i < 10; i++) {
        face(i, 100.0f - (float)i, 51, table_a, (float)(i + 1));
    }
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_copy_count == 0);
    CHECK(s_dyn_dst == 0);
    CHECK(s_draw_count == 1);
    if (s_draw_count == 1) {
        CHECK(s_draws[0].vertices == 30);
        CHECK(s_draws[0].tex == 51);
        CHECK(s_draws[0].table == 1);
    }
    for (i = 0; i < 10; i++) {
        int k;

        for (k = 0; k < 3; k++) {
            CHECK(s_batch[(i * 3 + k) * DYN_FLOATS + 4] == (float)(i + 1));
        }
    }
#if !ARENA
    CHECK(s_stream_vert_count == 30);
    if (!s_stream_truncated) {
        CHECK(s_draws[0].first_uv == 1.0f);
        CHECK(s_stream_count == 30);
        for (i = 0; i < 10; i++) {
            int k;

            for (k = 0; k < 3; k++) {
                CHECK(s_stream[(i * 3 + k) * DYN_FLOATS + 4] == (float)(i + 1));
            }
        }
    }
#endif
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_pass_vert_count == 30);
    CHECK(s_draws[0].first == 0);
    if (!s_pass_truncated && s_pass_vert_count == 30) {
        for (i = 0; i < 10; i++) {
            int k;

            for (k = 0; k < 3; k++) {
                CHECK(s_pass[(i * 3 + k) * DYN_FLOATS + 4] == (float)(i + 1));
            }
        }
    }
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 1);
#endif
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("fast long compatible group       ok");
    }
}

static void test_fast_clipped_faces(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(3);
    offscreen_face(0, 30.0f, 31, table_a, 1.0f, 1.0f);
    offscreen_face(1, 20.0f, 31, table_a, 2.0f, -1.0f);
    face(2, 10.0f, 31, table_a, 3.0f);
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_copy_count == 0);
    CHECK(s_draw_count == 1);
    if (s_draw_count == 1) {
        CHECK(s_draws[0].vertices == 9);
#if ARENA
        CHECK(s_draws[0].at_uv == 1.0f);
#else
        CHECK(s_draws[0].first_uv == 1.0f);
#endif
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_pass_vert_count == 9);
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 1);
#endif
    CHECK(s_last_offset == 0);
    CHECK(s_last_depth_func == RND_DEPTH_OFF);
    CHECK(s_last_depth_write == 1);
    if (s_failures == started) {
        puts("fast offscreen/near clipping     ok");
    }
}

/* The pass's one upload keeps run order and draws from its offsets. */
static void test_arena_three_runs(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(5);
    face(0, 30.0f, 11, table_a, 1.0f);
    face(1, 29.0f, 11, table_a, 2.0f);
    face(2, 20.0f, 12, table_a, 3.0f);
    face(3, 10.0f, 11, table_a, 4.0f);
    face(4, 9.0f, 11, table_a, 5.0f);
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_copy_count == 0);
    CHECK(s_draw_count == 3);
    if (s_draw_count == 3) {
        CHECK(s_draws[0].vertices == 6);
        CHECK(s_draws[1].vertices == 3);
        CHECK(s_draws[2].vertices == 6);
        CHECK(s_draws[0].tex == 11);
        CHECK(s_draws[1].tex == 12);
        CHECK(s_draws[2].tex == 11);
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[1].table == 1);
        CHECK(s_draws[2].table == 1);
#if ARENA
        CHECK(s_draws[0].first == 0);
        CHECK(s_draws[1].first == 6);
        CHECK(s_draws[2].first == 9);
        CHECK(s_draws[0].at_uv == 1.0f);
        CHECK(s_draws[1].at_uv == 3.0f);
        CHECK(s_draws[2].at_uv == 4.0f);
#else
        CHECK(s_draws[0].first == 0);
        CHECK(s_draws[1].first == 0);
        CHECK(s_draws[2].first == 0);
#endif
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(!s_pass_truncated);
    CHECK(s_pass_vert_count == 15);
    if (s_pass_vert_count == 15) {
        int k;

        for (k = 0; k < 5; k++) {
            int m;

            for (m = 0; m < 3; m++) {
                CHECK(s_pass[(k * 3 + m) * DYN_FLOATS + 4] == (float)(k + 1));
            }
        }
    }
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 3);
#endif
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("arena three material runs        ok");
    }
}

static void test_arena_singleton(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(1);
    face(0, 10.0f, 7, table_a, 9.0f);
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_draw_count == 1);
    if (s_draw_count == 1) {
        CHECK(s_draws[0].vertices == 3);
        CHECK(s_draws[0].tex == 7);
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[0].first == 0);
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_pass_vert_count == 3);
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 1);
#endif
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("arena singleton                  ok");
    }
}

/* One scene texture, two tables in A/B/A order: the table key splits the
 * pass into three runs, and the two A runs keep one table handle while
 * staying separate draws at their own offsets. */
static void test_arena_table_switch(void)
{
    static const u8 table_a[256 * 256];
    static const u8 table_b[256 * 256];
    int started = s_failures;

    prepare(5);
    face(0, 30.0f, 60, table_a, 1.0f);
    face(1, 25.0f, 60, table_a, 2.0f);
    face(2, 20.0f, 60, table_b, 3.0f);
    face(3, 15.0f, 60, table_a, 4.0f);
    face(4, 10.0f, 60, table_a, 5.0f);
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_copy_count == 0);
    CHECK(s_draw_count == 3);
    if (s_draw_count == 3) {
        CHECK(s_draws[0].vertices == 6);
        CHECK(s_draws[1].vertices == 3);
        CHECK(s_draws[2].vertices == 6);
        CHECK(s_draws[0].tex == 60);
        CHECK(s_draws[1].tex == 60);
        CHECK(s_draws[2].tex == 60);
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[1].table == 1);
        CHECK(s_draws[2].table == 1);
        CHECK(s_draws[0].depth_write == 0);
        CHECK(s_draws[1].depth_write == 0);
        CHECK(s_draws[2].depth_write == 0);
        /* Distinct non-NULL table identities reach texture unit 1, and
         * the outer A runs use the same handle as each other. */
        CHECK(s_draws[0].unit1 != 0);
        CHECK(s_draws[1].unit1 != 0);
        CHECK(s_draws[0].unit1 != s_draws[1].unit1);
        CHECK(s_draws[2].unit1 == s_draws[0].unit1);
        CHECK(s_draws[0].vertex_bytes == 6 * DYN_FLOATS * (int)sizeof(float));
        CHECK(s_draws[1].vertex_bytes == 3 * DYN_FLOATS * (int)sizeof(float));
        CHECK(s_draws[2].vertex_bytes == 6 * DYN_FLOATS * (int)sizeof(float));
#if ARENA
        /* Far to near A, B, A: no reordering, no merging of the A runs. */
        CHECK(s_draws[0].first == 0);
        CHECK(s_draws[1].first == 6);
        CHECK(s_draws[2].first == 9);
        CHECK(s_draws[0].at_uv == 1.0f);
        CHECK(s_draws[1].at_uv == 3.0f);
        CHECK(s_draws[2].at_uv == 4.0f);
        CHECK(s_draws[0].pass_bytes == 15 * DYN_FLOATS * (int)sizeof(float));
        CHECK(s_draws[1].pass_bytes == s_draws[0].pass_bytes);
        CHECK(s_draws[2].pass_bytes == s_draws[0].pass_bytes);
        CHECK((s_draws[2].first + s_draws[2].vertices) * DYN_FLOATS *
                  (int)sizeof(float) == s_draws[2].pass_bytes);
#else
        CHECK(s_draws[0].first == 0);
        CHECK(s_draws[0].first_uv == 1.0f);
        CHECK(s_draws[1].first_uv == 3.0f);
        CHECK(s_draws[2].first_uv == 4.0f);
        CHECK(s_draws[0].pass_bytes == 0);
#endif
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(!s_pass_truncated);
    CHECK(s_pass_vert_count == 15);
    if (!s_pass_truncated && s_pass_vert_count == 15) {
        CHECK(s_pass[0 * DYN_FLOATS + 4] == 1.0f);
        CHECK(s_pass[6 * DYN_FLOATS + 4] == 3.0f);
        CHECK(s_pass[9 * DYN_FLOATS + 4] == 4.0f);
    }
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 3);
#endif
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("arena table A/B/A runs           ok");
    }
}

/* Equal depths keep their spawn order; a key change splits the run. */
static void test_arena_ties_and_keys(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(4);
    face(0, 10.0f, 31, table_a, 1.0f);
    face(1, 10.0f, 31, table_a, 2.0f);
    face(2, 10.0f, 32, table_a, 3.0f);
    face(3, 10.0f, 31, NULL, 4.0f);
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_draw_count == 3);
    if (s_draw_count == 3) {
        CHECK(s_draws[0].vertices == 6);
        CHECK(s_draws[0].tex == 31);
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[1].vertices == 3);
        CHECK(s_draws[1].tex == 32);
        CHECK(s_draws[1].table == 1);
        CHECK(s_draws[2].vertices == 3);
        CHECK(s_draws[2].tex == 31);
        CHECK(s_draws[2].table == 0);
#if ARENA
        CHECK(s_draws[0].first == 0);
        CHECK(s_draws[1].first == 6);
        CHECK(s_draws[2].first == 9);
        CHECK(s_draws[0].at_uv == 1.0f);
        CHECK(s_draws[1].at_uv == 3.0f);
        CHECK(s_draws[2].at_uv == 4.0f);
#endif
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_pass_vert_count == 12);
    CHECK(s_pass[6 * DYN_FLOATS + 4] == 3.0f);
    CHECK(s_pass[9 * DYN_FLOATS + 4] == 4.0f);
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 3);
#endif
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("arena ties and key changes       ok");
    }
}

static void test_arena_max_faces(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;
    int i;

    prepare(MAX_DYN);
    for (i = 0; i < MAX_DYN; i++) {
        face6(i, (float)(MAX_DYN - i), 77, table_a, (float)i);
    }
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    CHECK(s_draw_count == 1);
    if (s_draw_count == 1) {
        CHECK(s_draws[0].vertices == MAX_DYN * 6);
        CHECK(s_draws[0].tex == 77);
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[0].first == 0);
    }
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(!s_pass_truncated);
    CHECK(s_pass_vert_count == MAX_DYN * 6);
    if (!s_pass_truncated && s_pass_vert_count == MAX_DYN * 6) {
        CHECK(s_pass[4] == 0.0f);
        CHECK(s_pass[((size_t)MAX_DYN * 6 - 1) * DYN_FLOATS + 4] == (float)(MAX_DYN - 1));
    }
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 1);
#endif
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("arena maximum 4096 faces         ok");
    }
}

#if ARENA
/* A staging allocation failure draws every face through the old path. */
static void test_arena_staging_failure(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(5);
    face(0, 30.0f, 41, table_a, 1.0f);
    face(1, 25.0f, 41, NULL, 2.0f);
    face(2, 20.0f, 42, table_a, 3.0f);
    face(3, 15.0f, 42, NULL, 4.0f);
    face(4, 10.0f, 41, table_a, 5.0f);
    free_staging();
    s_realloc_budget = 0;
    reset_calls();
    s_fetch_active = 1;

    draw_dyns();

    s_realloc_budget = -1;
    CHECK(s_copy_count == 0);
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 5);
    CHECK(s_draw_count == 5);
    if (s_draw_count == 5) {
        int i;

        for (i = 0; i < 5; i++) {
            CHECK(s_draws[i].vertices == 3);
            CHECK(s_draws[i].first == 0);
            CHECK(s_draws[i].first_uv == (float)(i + 1));
        }
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[1].table == 0);
        CHECK(s_draws[2].table == 1);
        CHECK(s_draws[3].table == 0);
        CHECK(s_draws[4].table == 1);
    }
    CHECK(s_last_offset == 0);
    CHECK(s_last_depth_func == RND_DEPTH_OFF);
    CHECK(s_last_depth_write == 1);
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("arena staging failure fallback   ok");
    }
}
#endif

/* Two DYN passes in one frame keep separate uploads and offsets. */
static void test_arena_two_passes(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(2);
    face(0, 20.0f, 51, table_a, 1.0f);
    face(1, 10.0f, 51, table_a, 2.0f);
    reset_calls();
    s_fetch_active = 1;
    draw_dyns();

    CHECK(s_draw_count == 1);
#if ARENA
    CHECK(s_pass_count == 1);
    CHECK(s_pass_vert_count == 6);
    CHECK(s_draws[0].first == 0);
    CHECK(s_draws[0].at_uv == 1.0f);
#else
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 1);
#endif

    prepare(2);
    face(0, 20.0f, 52, table_a, 11.0f);
    face(1, 10.0f, 53, table_a, 12.0f);
    draw_dyns();

    CHECK(s_draw_count == 3);
    if (s_draw_count == 3) {
        CHECK(s_draws[1].vertices == 3);
        CHECK(s_draws[1].tex == 52);
        CHECK(s_draws[2].vertices == 3);
        CHECK(s_draws[2].tex == 53);
#if ARENA
        CHECK(s_pass_count == 2);
        CHECK(s_draws[1].first == 0);
        CHECK(s_draws[2].first == 3);
        CHECK(s_draws[1].at_uv == 11.0f);
        CHECK(s_draws[1].passes_before == 2);
        CHECK(s_draws[0].at_uv == 1.0f);
#else
        CHECK(s_legacy_stream_calls == 3);
        CHECK(s_draws[1].first == 0);
#endif
    }
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("arena separate DYN passes        ok");
    }
}

static void test_fallback(void)
{
    int started = s_failures;

    prepare(2);
    CHECK(s_dyn_dst == 0);
    two_table_faces();
    reset_calls();
    s_fetch_active = 0;
    s_batch_copies = 0;

    draw_dyns();

    CHECK(s_copy_count == 2);
    CHECK(s_draw_count == 2);
    if (s_draw_count == 2) {
        CHECK(s_draws[0].first_uv == 1.0f);
        CHECK(s_draws[0].table == 1);
        CHECK(s_draws[1].first_uv == 2.0f);
        CHECK(s_draws[1].table == 1);
    }
    CHECK(s_last_offset == 0);
    CHECK(s_last_depth_func == RND_DEPTH_OFF);
    CHECK(s_last_depth_write == 1);
    CHECK(s_texture_free_count == 0);
    CHECK(s_dyn_dst != 0);
    CHECK(s_dyn_dst_w == s_w);
    CHECK(s_dyn_dst_h == s_h);
    CHECK(s_texture_count == 2);     /* snapshot and cached table */

    /* The existing snapshot is reused at the same size ... */
    two_table_faces();
    reset_calls();
    draw_dyns();
    CHECK(s_copy_count == 2);
    CHECK(s_texture_free_count == 0);
    CHECK(s_texture_count == 0);

    /* ... and replaced when either size changes. */
    two_table_faces();
    s_dyn_dst_w = 63;
    reset_calls();
    draw_dyns();
    CHECK(s_copy_count == 2);
    CHECK(s_texture_free_count == 1);
    CHECK(s_texture_count == 1);

    two_table_faces();
    s_dyn_dst_h = 63;
    reset_calls();
    draw_dyns();
    CHECK(s_copy_count == 2);
    CHECK(s_texture_free_count == 1);
    CHECK(s_texture_count == 1);

    if (s_failures == started) {
        puts("copy fallback unchanged          ok");
    }
}

/* Eight table faces overlap, and each table maps what the previous ones
 * wrote, so every one needs the frame as it stood after the face before
 * it. With batch copies on and no framebuffer fetch the layer path must
 * take one copy per dependency layer. The offscreen table face is not
 * drawn; ordinary faces stay drawn (one offscreen, one in the pile). */
static void test_layered_dependencies(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;
    int k;
#if defined(__vita__) || defined(IGN_GXM)
    int copies = MAX_LAYER;
#else
    int copies = 8;
#endif

    prepare(11);
    offscreen_face(0, 110.0f, 101, table_a, 1.0f, 1.0f);
    offscreen_face(1, 105.0f, 100, NULL, 2.0f, 1.0f);
    for (k = 0; k < 8; k++) {
        face(2 + k, 100.0f - 10.0f * (float)k, (unsigned)(102 + k), table_a,
             (float)(3 + k));
    }
    face(10, 20.0f, 200, NULL, 11.0f);
    reset_calls();
    s_fetch_active = 0;
    s_batch_copies = 1;

    draw_dyns();

    CHECK(s_copy_count == copies);
    CHECK(s_draw_count == 10);
    if (s_draw_count == 10) {
        /* Offscreen ordinary face: drawn at layer 0, before any copy. */
        CHECK(s_draws[0].first_uv == 2.0f);
        CHECK(s_draws[0].tex == 100);
        CHECK(s_draws[0].table == 0);
        CHECK(s_draws[0].copies_before == 0);
        for (k = 0; k < 8; k++) {
            int expect_copies = k + 1 < copies ? k + 1 : copies;

            CHECK(s_draws[1 + k].first_uv == (float)(3 + k));
            CHECK(s_draws[1 + k].tex == (unsigned)(102 + k));
            CHECK(s_draws[1 + k].table == 1);
            CHECK(s_draws[1 + k].copies_before == expect_copies);
        }
        /* Ordinary face in the pile: after the last table draw, no copy. */
        CHECK(s_draws[9].first_uv == 11.0f);
        CHECK(s_draws[9].tex == 200);
        CHECK(s_draws[9].table == 0);
        CHECK(s_draws[9].copies_before == copies);
    }
    CHECK(s_last_offset == 0);
    CHECK(s_last_depth_func == RND_DEPTH_OFF);
    CHECK(s_last_depth_write == 1);
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
#if defined(__vita__) || defined(IGN_GXM)
        puts("layered dependencies (GXM cap)   ok");
#else
        puts("layered dependencies (GLES)      ok");
#endif
    } else {
        for (k = 0; k < s_draw_count; k++) {
            fprintf(stderr, "  draw %d: uv=%.1f tex=%u table=%d copies_before=%d\n", k,
                    (double)s_draws[k].first_uv, s_draws[k].tex, s_draws[k].table,
                    s_draws[k].copies_before);
        }
        fprintf(stderr, "layered dependencies: expected %d copies for 8 table faces, "
                        "got %d (table draw copies before: 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d)\n",
                copies, s_copy_count, s_draw_count > 1 ? s_draws[1].copies_before : -1,
                s_draw_count > 2 ? s_draws[2].copies_before : -1,
                s_draw_count > 3 ? s_draws[3].copies_before : -1,
                s_draw_count > 4 ? s_draws[4].copies_before : -1,
                s_draw_count > 5 ? s_draws[5].copies_before : -1,
                s_draw_count > 6 ? s_draws[6].copies_before : -1);
    }
}

int main(void)
{
    test_empty();
    test_fast_grouping();
    test_fast_mixed_depth();
    test_fast_long_group();
    test_fast_clipped_faces();
    test_arena_three_runs();
    test_arena_singleton();
    test_arena_table_switch();
    test_arena_ties_and_keys();
    test_arena_max_faces();
#if ARENA
    test_arena_staging_failure();
#endif
    test_arena_two_passes();
    test_fallback();
    test_layered_dependencies();

    if (s_failures != 0) {
        fprintf(stderr, "test_gl3d_dyn_fetch: %d failures\n", s_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
