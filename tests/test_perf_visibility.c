/*
 * Conservative GPU visibility rejection. The fixture is the DYN fast
 * path's: include it for the recording rnd stubs and the real draw_dyns,
 * then drive gl3d_draw for the cached static-mesh path.
 *
 * Build with -D__ANDROID__=1 -DIGN_GLES_VISIBILITY_CULL=1 for the
 * experiment; build without either for the feature-off control.
 */

#define main dyn_fetch_fixture_main
#include "test_gl3d_dyn_fetch.c"
#undef main

#if defined(IGN_GLES) && defined(__ANDROID__) && defined(IGN_GLES_VISIBILITY_CULL) && \
    IGN_GLES_VISIBILITY_CULL == 1
#define TEST_CULL 1
#else
#define TEST_CULL 0
#endif

static void setup_scene(struct scene *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->zoom = 1.0;
    sc->near_ofs = 1;
    sc->focal_x = 253;
    sc->focal_y = 207;
    sc->center_x = 32;
    sc->center_y = 32;
}

static void setup_object(struct scene_obj *o, const s32 *mesh, s32 x, s32 y, s32 z)
{
    memset(o, 0, sizeof(*o));
    o->mesh = mesh;
    o->x = x;
    o->y = y;
    o->z = z;
}

static void make_flat_mesh(s32 *mesh, s32 x, s32 y, s32 z, s32 extent)
{
    u8 *f = (u8 *)(mesh + 2 + 3 * 3);
    const s32 face[] = { 15, 0, 1, 2, 0 };

    memset(mesh, 0, 2 * sizeof(s32) + 3 * 3 * sizeof(s32) + sizeof(face));
    mesh[0] = 3;
    mesh[1] = 1;
    mesh[2] = x;
    mesh[3] = y;
    mesh[4] = z;
    mesh[5] = x + extent;
    mesh[6] = y;
    mesh[7] = z;
    mesh[8] = x;
    mesh[9] = y + extent;
    mesh[10] = z;
    memcpy(f, face, sizeof(face));
}

static void clip_face(float x, float y, float w)
{
    int k;

    prepare(1);
    s_dyns[0].count = 3;
    s_dyns[0].depth = 30.0f;
    s_dyns[0].tex = 77;
    s_dyns[0].table = NULL;
    for (k = 0; k < 3; k++) {
        s_dyns[0].v[k][0] = x;
        s_dyns[0].v[k][1] = y;
        s_dyns[0].v[k][2] = 0.0f;
        s_dyns[0].v[k][3] = w;
        s_dyns[0].v[k][4] = (float)(k + 1);
    }
}

static void test_dyn_clip_sides(void)
{
    int started = s_failures;
    int side;

    for (side = 0; side < 4; side++) {
        float x = side == 0 ? 2.0f : side == 1 ? -2.0f : 0.0f;
        float y = side == 2 ? 2.0f : side == 3 ? -2.0f : 0.0f;

        clip_face(x, y, 1.0f);
        reset_calls();
        s_fetch_active = 1;
        draw_dyns();
        CHECK(s_draw_count == (TEST_CULL ? 0 : 1));
        CHECK(s_dyn_count == 0);
    }
    if (s_failures == started) {
        puts("DYN four clip sides               ok");
    }
}

static void test_dyn_ambiguity_kept(void)
{
    int started = s_failures;
    float cases[][2] = {
        { 1.0f, 1.0f },             /* touching right */
        { -1.0f, 1.0f },            /* touching left */
        { 0.0f, 1.0f },             /* dead centre */
        { 2.0f, 0.0f },             /* behind the eye */
        { 2.0f, -1.0f },            /* wholly behind the eye */
        { 3.0e38f, 3.0e38f },       /* overflow-shaped, finite */
        { NAN, 1.0f },
        { 0.0f, NAN },
        { 0.0f, NAN },
        { INFINITY, 1.0f }
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        clip_face(cases[i][0], 0.0f, cases[i][1]);
        reset_calls();
        s_fetch_active = 1;
        draw_dyns();
        CHECK(s_draw_count == 1);
        CHECK(s_dyn_count == 0);
    }
    clip_face(0.0f, NAN, 1.0f);
    reset_calls();
    s_fetch_active = 1;
    draw_dyns();
    CHECK(s_draw_count == 1);
    CHECK(s_dyn_count == 0);
    /* A face crossing the eye, not merely one wholly behind it. */
    prepare(1);
    s_dyns[0].count = 3;
    s_dyns[0].tex = 77;
    s_dyns[0].v[0][0] = -2.0f; s_dyns[0].v[0][3] = 1.0f;
    s_dyns[0].v[1][0] = 2.0f;  s_dyns[0].v[1][3] = 1.0f;
    s_dyns[0].v[2][0] = 2.0f;  s_dyns[0].v[2][3] = -1.0f;
    reset_calls();
    s_fetch_active = 1;
    draw_dyns();
    CHECK(s_draw_count == 1);
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("DYN ambiguity kept                ok");
    }
}

static void test_dyn_straddle_and_stable_order(void)
{
    static const u8 table_a[256 * 256];
    int started = s_failures;

    prepare(4);
    face(0, 20.0f, 31, table_a, 1.0f);
    offscreen_face(1, 30.0f, 31, table_a, 2.0f, 1.0f);
    face(2, 20.0f, 31, table_a, 3.0f);
    face(3, 10.0f, 31, table_a, 4.0f);
    reset_calls();
    s_fetch_active = 1;
    draw_dyns();

#if TEST_CULL
    CHECK(s_draw_count == 1);
    if (s_draw_count == 1) {
        CHECK(s_draws[0].vertices == 9);
        CHECK(s_draws[0].first_uv == 1.0f);
        CHECK(s_batch[4] == 1.0f);
        CHECK(s_batch[3 * DYN_FLOATS + 4] == 3.0f);
        CHECK(s_batch[6 * DYN_FLOATS + 4] == 4.0f);
    }
#else
    CHECK(s_draw_count == 1);
#endif
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("DYN straddle/stable order        ok");
    }
}

static void test_dyn_copy_fallback(void)
{
    int started = s_failures;

    prepare(2);
    face(0, 20.0f, 40, NULL, 1.0f);
    offscreen_face(1, 30.0f, 41, NULL, 2.0f, 1.0f);
    reset_calls();
    s_fetch_active = 0;
    s_batch_copies = 0;
    draw_dyns();

    CHECK(s_draw_count == (TEST_CULL ? 1 : 2));
    CHECK(s_dyn_count == 0);
    if (s_failures == started) {
        puts("DYN copy fallback after cull     ok");
    }
}

static void test_static_clip_sides(void)
{
    s32 mesh[2 + 3 * 3 + 5];
    struct scene sc;
    struct scene_obj obj;
    struct scene_obj *objs[1];
    int started = s_failures;
    int side;

    for (side = 0; side < 4; side++) {
        s32 x = side == 0 ? 10000 : side == 1 ? -10000 : 0;
        s32 y = side == 2 ? 10000 : side == 3 ? -10000 : 0;

        make_flat_mesh(mesh, 0, 0, 0, 10);
        setup_scene(&sc);
        setup_object(&obj, mesh, x, y, 0);
        objs[0] = &obj;
        gl3d_flush();
        reset_calls();
        gl3d_begin(64, 64);
        gl3d_draw(&sc, objs, 1, 0, 0);
        CHECK(s_mesh_draw_count == (TEST_CULL ? 0 : 1));
    }
    gl3d_flush();
    if (s_failures == started) {
        puts("static four clip sides           ok");
    }
}

static void test_static_straddle_mirror_and_transform(void)
{
    s32 mesh[2 + 3 * 3 + 5];
    struct scene sc;
    struct scene_obj obj;
    struct scene_obj *objs[1];
    int started = s_failures;

    /* A box spanning the view is never wholly outside. */
    make_flat_mesh(mesh, -10000, 0, 0, 20000);
    setup_scene(&sc);
    setup_object(&obj, mesh, 0, 0, 0);
    objs[0] = &obj;
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);

    /* The mirrored view reaches the same matrix path. */
    make_flat_mesh(mesh, 0, 0, 0, 10);
    sc.zoom = -1.0;
    setup_object(&obj, mesh, 0, 0, 0);
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);

    setup_object(&obj, mesh, 10000, 0, 0);
    reset_calls();
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == (TEST_CULL ? 0 : 1));

    /* A scaling transform brings the same world point into the view. */
    sc.zoom = 1.0e-6;
    obj.x = 10000;
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);
    gl3d_flush();
    if (s_failures == started) {
        puts("static straddle/mirror/scaling   ok");
    }
}

static void test_static_mesh_change_and_cache(void)
{
    s32 mesh[2 + 3 * 3 + 5];
    struct scene sc;
    struct scene_obj obj;
    struct scene_obj *objs[1];
    int started = s_failures;

    make_flat_mesh(mesh, 10000, 0, 0, 10);
    setup_scene(&sc);
    setup_object(&obj, mesh, 10000, 0, 0);
    objs[0] = &obj;
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);      /* cold, outside */
    gl3d_draw(&sc, objs, 1, 0, 0);      /* warm, still outside */
    CHECK(s_mesh_draw_count == (TEST_CULL ? 0 : 2));

    make_flat_mesh(mesh, 0, 0, 0, 10);
    gl3d_mesh_changed(mesh);
    obj.x = 0;
    reset_calls();
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);
    gl3d_flush();
    if (s_failures == started) {
        puts("static cache/mesh change         ok");
    }
}

static void test_static_deferred_kept_when_body_culled(void)
{
    static const u8 table_a[256 * 256];
    s32 mesh[2 + 3 * 6 + 11 + 5];
    struct scene sc;
    struct scene_obj obj;
    struct scene_obj *objs[1];
    u8 *f;
    s32 *w;
    int started = s_failures;

    memset(mesh, 0, sizeof(mesh));
    mesh[0] = 6;
    mesh[1] = 2;
    mesh[2] = 0;     mesh[3] = 0;  mesh[4] = 100;
    mesh[5] = 10;    mesh[6] = 0;  mesh[7] = 100;
    mesh[8] = 0;     mesh[9] = 10; mesh[10] = 100;
    mesh[11] = 10000; mesh[12] = 0;  mesh[13] = 100;
    mesh[14] = 10010; mesh[15] = 0;  mesh[16] = 100;
    mesh[17] = 10000; mesh[18] = 10; mesh[19] = 100;
    f = (u8 *)(mesh + 2 + 3 * 6);
    w = (s32 *)f;
    w[0] = 19;
    w[1] = 0;
    w[2] = 1;
    w[3] = 2;
    f += 44;
    w = (s32 *)f;
    w[0] = 15;
    w[1] = 3;
    w[2] = 4;
    w[3] = 5;

    g_scene_textures[0] = (u8 *)table_a;
    g_scene_texture_pages[0] = 1;
    gl3d_set_tables(table_a, NULL);
    setup_scene(&sc);
    setup_object(&obj, mesh, 0, 0, 0);
    objs[0] = &obj;
    gl3d_flush();
    reset_calls();
    s_fetch_active = 1;
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);

    CHECK(s_mesh_draw_count == (TEST_CULL ? 0 : 1));
    CHECK(s_draw_count == 1);
    CHECK(s_dyn_count == 0);
    gl3d_flush();
    gl3d_set_tables(NULL, NULL);
    g_scene_textures[0] = NULL;
    if (s_failures == started) {
        puts("static deferred body culled      ok");
    }
}

static void test_static_degenerate_and_nonfinite(void)
{
    s32 mesh[2 + 3 * 3 + 5];
    s32 empty[2];
    struct scene sc;
    struct scene_obj obj;
    struct scene_obj *objs[1];
    int started = s_failures;

    /* Zero-area boxes are still accepted or rejected as a whole. */
    make_flat_mesh(mesh, 0, 0, 0, 0);
    setup_scene(&sc);
    setup_object(&obj, mesh, 10000, 0, 0);
    objs[0] = &obj;
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == (TEST_CULL ? 0 : 1));

    setup_object(&obj, mesh, 0, 0, 0);
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);

    /* A later uploaded vertex lowers the cached box. */
    make_flat_mesh(mesh, 0, 0, 0, 10);
    mesh[2] = 10000; mesh[3] = 10000; mesh[4] = 10000;
    mesh[5] = 0;     mesh[6] = 0;     mesh[7] = 0;
    mesh[8] = 5000;  mesh[9] = 5000;  mesh[10] = 5000;
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);

    /* Building no triangles leaves no bounds to trust. */
    memset(empty, 0, sizeof(empty));
    setup_object(&obj, empty, 0, 0, 0);
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 0);

    /* Ambiguous transforms keep the draw rather than guessing. */
    make_flat_mesh(mesh, 0, 0, 0, 10);
    setup_object(&obj, mesh, 0, 0, 0);
    sc.zoom = NAN;
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);

    setup_scene(&sc);
    sc.y = NAN;
    {
        float m[16];
        object_matrix(&sc, &obj, 0, 0, m);
        CHECK(!isfinite(m[13]));
    }
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);

    setup_scene(&sc);
    sc.near_ofs = -10000;               /* w <= 0: behind the eye */
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);
    gl3d_flush();
    if (s_failures == started) {
        puts("static degenerate/nonfinite      ok");
    }
}

#if TEST_CULL
/* The exact real sum, without the float roundings the CPU and GPU each
 * choose for a four-term dot product. */
static double exact_dot(const float m[16], int row, double px, double py, double pz)
{
    return (double)m[row] * px + (double)m[4 + row] * py + (double)m[8 + row] * pz
         + (double)m[12 + row];
}

/* A degenerate uploaded box and a matrix row whose float product and
 * addition cancel: the CPU's separate float operations put x at 4, while
 * the exact sum and a contracted/FMA GPU sum can put it at 0.079...,
 * inside w = 1. The cull must keep it rather than trust the last bit. */
static void test_static_cancellation_margin(void)
{
    struct mesh_entry e;
    float m[16];
    volatile float mul, add;
    double exact;
    int started = s_failures;

    memset(&e, 0, sizeof(e));
    memset(m, 0, sizeof(m));
    e.bounds_valid = 1;
    e.lo[0] = e.hi[0] = 100000000.0f;
    e.lo[1] = e.hi[1] = 100000008.0f;
    e.lo[2] = e.hi[2] = 0.0f;
    m[0] = -1.00000011920928955078125f;
    m[4] = 1.0f;
    m[12] = 4.0f;
    m[15] = 1.0f;

    exact = exact_dot(m, 0, e.lo[0], e.lo[1], e.lo[2]);
    CHECK(fabs(exact) < 1.0);
    /* An independently associated high-precision sum agrees it is inside. */
    CHECK(fabs(((double)m[0] * e.lo[0] + (double)m[4] * e.lo[1])
               + ((double)m[8] * e.lo[2] + (double)m[12])) < 1.0);

    mul = m[0] * e.lo[0];
    add = mul + m[4] * e.lo[1];
    add = add + m[8] * e.lo[2];
    add = add + m[12];
    CHECK(add > 1.0f);              /* the CPU's separate float result */

    CHECK(mesh_visible(&e, m) == 1);

    /* The same row with a genuinely far box is still rejected. */
    m[12] = 1000000.0f;
    CHECK(mesh_visible(&e, m) == 0);
    if (s_failures == started) {
        puts("static cancellation margin      ok");
    }
}

/* The rounding envelope follows ordinary FP32, not arbitrary precision.
 * A float product may overflow even while the exact double sum and the
 * old relative error both stayed finite. A nonzero subnormal coefficient
 * may also be flushed to zero before a large position amplifies it. */
static void test_static_clip_dot_envelope(void)
{
    struct mesh_entry e;
    float m[16], error;
    double exact;
    volatile float product0, product1, product2, float_sum;
    int started = s_failures;

    /* Exact x is 1e35 and w is 1, but the separate float products are
     * +Inf and -Inf, whose sum is NaN. The GPU cannot be trusted here. */
    memset(&e, 0, sizeof(e));
    memset(m, 0, sizeof(m));
    e.bounds_valid = 1;
    e.lo[0] = e.hi[0] = 2.0f;
    e.lo[1] = e.hi[1] = 2.0f;
    e.lo[2] = e.hi[2] = 0.0f;
    m[0] = FLT_MAX;
    m[4] = -FLT_MAX;
    m[12] = 1.0e35f;
    m[15] = 1.0f;
    exact = clip_dot(m, 0, e.lo[0], e.lo[1], e.lo[2], &error);
    CHECK(exact > 1.0e34);
    CHECK(error == INFINITY);
    product0 = m[0] * e.lo[0];
    product1 = m[4] * e.lo[1];
    float_sum = product0 + product1;
    CHECK(!isfinite(float_sum));
    CHECK(mesh_visible(&e, m) == 1);

    /* The coefficient is subnormal, so a GPU may flush it before the
     * multiply. The exact x is 5.88e-31, just outside w, while the
     * flushed evaluation is exactly 0 and inside. Keep the box. */
    memset(&e, 0, sizeof(e));
    memset(m, 0, sizeof(m));
    e.bounds_valid = 1;
    e.lo[0] = e.hi[0] = 1.0e8f;
    m[0] = FLT_MIN / 2.0f;
    m[15] = 2.94e-31f;
    CHECK(m[0] > 0.0f && m[0] < FLT_MIN);
    exact = clip_dot(m, 0, e.lo[0], e.lo[1], e.lo[2], &error);
    CHECK(exact > (double)m[15]);
    CHECK(mesh_visible(&e, m) == 1);

    m[0] = -FLT_MIN / 2.0f;
    exact = clip_dot(m, 0, e.lo[0], e.lo[1], e.lo[2], &error);
    CHECK(exact < -(double)m[15]);
    CHECK(mesh_visible(&e, m) == 1);

    /* Ordinary small values and exact zero still get a finite, generous
     * bound; zero includes the operation FTZ allowance. */
    memset(m, 0, sizeof(m));
    m[0] = 2.0f;
    m[12] = 1.0f;
    exact = clip_dot(m, 0, 3.0f, 0.0f, 0.0f, &error);
    CHECK(exact == 7.0);
    CHECK(isfinite(error));
    memset(m, 0, sizeof(m));
    exact = clip_dot(m, 0, 3.0f, 4.0f, 5.0f, &error);
    CHECK(exact == 0.0);
    CHECK(error == 8.0f * FLT_MIN);

    /* The exact product is subnormal, so an FTZ operation may erase it
     * while an ordinary rounding operation keeps it. The absolute
     * allowance must cover either result. */
    m[0] = FLT_MIN;
    exact = clip_dot(m, 0, 0.5f, 0.0f, 0.0f, &error);
    CHECK(0.0 < exact && exact < FLT_MIN);
    CHECK(error >= 8.0f * FLT_MIN);

    /* S below FLT_MAX is not enough: ordinary roundings can still push a
     * product or association over it. Every value here is exact in
     * binary32; the exact S/U is 33554430 - 5*2^-21, while the separately
     * rounded products sum to 33554431*U, the RN overflow midpoint. */
    memset(&e, 0, sizeof(e));
    memset(m, 0, sizeof(m));
    e.bounds_valid = 1;
    e.lo[0] = e.hi[0] = 11184807.0f;
    e.lo[1] = e.hi[1] = 11184807.0f;
    e.lo[2] = e.hi[2] = 11184808.0f;
    m[0] = m[4] = m[8] = 0x1p103f + 0x1p81f;      /* (1 + 2^-22) * 2^103 */
    m[15] = 1.0f;
    exact = clip_dot(m, 0, e.lo[0], e.lo[1], e.lo[2], &error);
    CHECK(exact < (double)FLT_MAX);
    CHECK(exact > 3.4e38);
    CHECK(error == INFINITY);
    product0 = m[0] * e.lo[0];
    product1 = m[4] * e.lo[1];
    product2 = m[8] * e.lo[2];
    float_sum = (product0 + product1) + product2;
    CHECK(!isfinite(float_sum));
    CHECK(mesh_visible(&e, m) == 1);

    /* The finite boundary itself must reserve the same headroom, while a
     * comfortably near-max sum still rejects normally. */
    m[0] = FLT_MAX / 2.0f;
    exact = clip_dot(m, 0, 2.0f, 0.0f, 0.0f, &error);
    CHECK(exact == FLT_MAX);
    CHECK(error == INFINITY);

    memset(&e, 0, sizeof(e));
    memset(m, 0, sizeof(m));
    e.bounds_valid = 1;
    e.lo[0] = e.hi[0] = 2.0f;
    m[0] = FLT_MAX / 4.0f;
    m[15] = 1.0f;
    exact = clip_dot(m, 0, e.lo[0], 0.0f, 0.0f, &error);
    CHECK(exact == (double)FLT_MAX / 2.0);
    CHECK(isfinite(error));
    CHECK(error > 1.0e30f);
    CHECK(mesh_visible(&e, m) == 0);

    m[0] = 1.0e38f;
    exact = clip_dot(m, 0, 1.0e7f, 0.0f, 0.0f, &error);
    CHECK(exact > FLT_MAX);
    CHECK(error == INFINITY);

    /* Non-finite and nonzero subnormal positions are outside the
     * envelope too. */
    memset(&e, 0, sizeof(e));
    memset(m, 0, sizeof(m));
    e.bounds_valid = 1;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    e.lo[0] = e.hi[0] = NAN;
    CHECK(mesh_visible(&e, m) == 1);
    e.lo[0] = e.hi[0] = INFINITY;
    CHECK(mesh_visible(&e, m) == 1);
    e.lo[0] = e.hi[0] = FLT_MIN / 2.0f;
    CHECK(mesh_visible(&e, m) == 1);
    e.lo[0] = -FLT_MIN / 2.0f;
    e.hi[0] = FLT_MIN / 2.0f;
    CHECK(mesh_visible(&e, m) == 1);
    e.lo[0] = 0.0f;
    CHECK(mesh_visible(&e, m) == 1);
    if (s_failures == started) {
        puts("static clip-dot envelope         ok");
    }
}
#endif

/* The production object_matrix with nonidentity camera and object rows,
 * a nonzero screen offset and scene coordinates, then the real static
 * draw path. */
#if TEST_CULL
static void test_static_rotated_offset_projection(void)
{
    static s32 mesh[2 + 3 * 3 + 5];
    struct scene sc;
    struct scene_obj obj;
    struct scene_obj *objs[1];
    struct mesh_entry *e;
    double view[3][3], rot[3][3], d[3], ax, ay, bx, by, w, x, y;
    float m[16];
    int started = s_failures;

    make_flat_mesh(mesh, -10, -10, 0, 20);
    setup_scene(&sc);
    sc.pitch = 170;
    sc.yaw = 330;
    sc.roll = 70;
    sc.x = 1000.0;
    sc.y = 200.0;
    sc.z = -3000.0;
    setup_object(&obj, mesh, 0, 0, 0);
    obj.ax = 150;
    obj.ay = 250;
    obj.az = 90;
    objs[0] = &obj;

    scene_view_rows(&sc, view);
    scene_obj_rows(&obj, rot);
    CHECK(fabs(view[0][2]) > 1e-3);
    CHECK(fabs(rot[0][1]) > 1e-3);

    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    e = mesh_get(mesh);
    CHECK(e != NULL);

    /* Far along the camera's right axis, at depth 500: wholly off one
     * side with the whole rotated box in front of the eye. */
    d[0] = view[0][0] * 10000.0 + view[2][0] * 500.0;
    d[1] = view[0][1] * 10000.0 + view[2][1] * 500.0;
    d[2] = view[0][2] * 10000.0 + view[2][2] * 500.0;
    obj.x = (s32)(sc.x + d[0]);
    obj.y = (s32)(sc.y + d[1]);
    obj.z = (s32)(sc.z + d[2]);
    object_matrix(&sc, &obj, 37, -21, m);
    CHECK(fabsf(m[4]) > 1e-4f);     /* the rows really are rotated */
    CHECK(mesh_visible(e, m) == 0);
    reset_calls();
    gl3d_draw(&sc, objs, 1, 37, -21);
    CHECK(s_mesh_draw_count == (TEST_CULL ? 0 : 1));

    /* The offset centre ray at depth 500, intersected with the view. */
    w = 500.0 + sc.near_ofs;
    ax = -2.0 * sc.focal_x * sc.zoom / s_vw;
    ay = -2.0 * sc.focal_y / s_vh;
    bx = 2.0 * (37.0 + s_ox + sc.center_x) / s_vw - 1.0;
    by = 2.0 * (-21.0 + s_oy + sc.center_y) / s_vh - 1.0;
    x = -bx * w / ax;
    y = -by * w / ay;
    d[0] = view[0][0] * x + view[1][0] * y + view[2][0] * 500.0;
    d[1] = view[0][1] * x + view[1][1] * y + view[2][1] * 500.0;
    d[2] = view[0][2] * x + view[1][2] * y + view[2][2] * 500.0;
    obj.x = (s32)(sc.x + d[0] + 0.5);
    obj.y = (s32)(sc.y + d[1] + 0.5);
    obj.z = (s32)(sc.z + d[2] + 0.5);
    object_matrix(&sc, &obj, 37, -21, m);
    CHECK(fabs((double)m[12] / m[15]) < 0.2);
    CHECK(fabs((double)m[13] / m[15]) < 0.2);
    CHECK(mesh_visible(e, m) == 1);
    reset_calls();
    gl3d_draw(&sc, objs, 1, 37, -21);
    CHECK(s_mesh_draw_count == 1);

    /* The same placed object, seen from the unrotated identity camera. */
    setup_scene(&sc);
    obj.ax = obj.ay = obj.az = 0;
    gl3d_flush();
    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);
    CHECK(s_mesh_draw_count == 1);
    gl3d_flush();
    if (s_failures == started) {
        puts("static rotated/offset projection ok");
    }
}
#endif

/* Offscreen static meshes must still call texture_get in admission order:
 * first 64 slots fill MAX_TEXTURES, so the visible 65th gets 0 exactly as
 * the feature-off path does. */
static void test_static_texture_saturation_order(void)
{
    enum { OBJECTS = 65 };
    static s32 meshes[OBJECTS][2 + 3 * 3 + 5];
    static u8 pages[OBJECTS][256 * 256];
    static struct scene_obj storage[OBJECTS];
    struct scene_obj *objs[OBJECTS];
    struct scene sc;
    int started = s_failures;
    int i;

    setup_scene(&sc);
    gl3d_flush();
    for (i = 0; i < OBJECTS; i++) {
        make_flat_mesh(meshes[i], i == OBJECTS - 1 ? 0 : 10000, 0, 0, 10);
        setup_object(&storage[i], meshes[i], 0, 0, 0);
        storage[i].tex = i;
        objs[i] = &storage[i];
        g_scene_textures[i] = pages[i];
        g_scene_texture_pages[i] = 1;
    }

    reset_calls();
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, OBJECTS, 0, 0);

    CHECK(s_texture_count == MAX_TEXTURES);
    CHECK(s_pages_count == MAX_TEXTURES);
    CHECK(s_texture_unit0 == 0);
    CHECK(s_mesh_draw_count == (TEST_CULL ? 1 : OBJECTS));
    for (i = 0; i < MAX_TEXTURES; i++) {
        CHECK(s_pages_ids[i] == 1000u + (unsigned)i);
        CHECK(s_pages_pixels[i] == pages[i]);
        CHECK(s_textures[i].pixels == pages[i]);
        CHECK(s_textures[i].pages == 1);
        CHECK(s_textures[i].tex == 1000u + (unsigned)i);
    }

    gl3d_flush();
    for (i = 0; i < OBJECTS; i++) {
        g_scene_textures[i] = NULL;
        g_scene_texture_pages[i] = 0;
    }
    if (s_failures == started) {
        puts("static texture saturation order ok");
    }
}

/* The real box test with transformations that cannot be produced from
 * the scene's integer object fields in this fixture. */
#if TEST_CULL
static void test_static_matrix_guards(void)
{
    struct mesh_entry e;
    float m[16];
    int started = s_failures;

    memset(&e, 0, sizeof(e));
    memset(m, 0, sizeof(m));
    CHECK(mesh_visible(&e, m) == 1);    /* no uploaded bounds: keep */

    e.bounds_valid = 1;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    CHECK(mesh_visible(&e, m) == 1);

    m[12] = NAN;
    CHECK(mesh_visible(&e, m) == 1);
    m[12] = 0.0f;
    m[13] = NAN;
    CHECK(mesh_visible(&e, m) == 1);
    m[13] = 0.0f;
    m[15] = NAN;
    CHECK(mesh_visible(&e, m) == 1);
    m[15] = -1.0f;
    CHECK(mesh_visible(&e, m) == 1);

    /* The uploaded coordinates can make the product-sum bound overflow:
     * an uncertain box is kept, not culled. */
    e.lo[0] = e.hi[0] = 3.0e38f;
    m[15] = 1.0f;
    m[0] = 3.0e38f;
    CHECK(mesh_visible(&e, m) == 1);

    /* Finite corner values whose absolute-product-sum bounds overflow
     * are kept, whichever row's bound overflows. */
    memset(m, 0, sizeof(m));
    e.lo[0] = e.hi[0] = 3.0e38f;
    e.lo[1] = e.hi[1] = 3.0e38f;
    e.lo[2] = e.hi[2] = 0.0f;
    m[0] = 3.0e38f;
    m[4] = -3.0e38f;
    m[15] = 1.0f;
    CHECK(mesh_visible(&e, m) == 1);    /* x bound overflows */

    memset(m, 0, sizeof(m));
    e.lo[2] = e.hi[2] = 3.0e38f;
    m[0] = 1.0f;
    m[5] = 3.0e38f;
    m[9] = -3.0e38f;
    m[15] = 1.0f;
    CHECK(mesh_visible(&e, m) == 1);    /* y bound overflows */

    m[5] = m[9] = 0.0f;
    m[3] = 3.0e38f;
    m[7] = -3.0e38f;
    m[15] = 0.0f;
    CHECK(mesh_visible(&e, m) == 1);    /* w bound overflows */
    if (s_failures == started) {
        puts("static matrix guards             ok");
    }
}
#endif

static s32 *make_dynamic_admission_mesh(int faces)
{
    size_t ints = 2 + 3 * 6 + (size_t)faces * 5;
    s32 *mesh = calloc(ints, sizeof(*mesh));
    u8 *f;
    int i;

    mesh[0] = 6;
    mesh[1] = faces;
    /* Vertices 0..2 are visible; 3..5 are wholly left. */
    mesh[2] = 0;      mesh[3] = 0;  mesh[4] = 100;
    mesh[5] = 10;     mesh[6] = 0;  mesh[7] = 100;
    mesh[8] = 0;      mesh[9] = 10; mesh[10] = 100;
    mesh[11] = 10000; mesh[12] = 0; mesh[13] = 100;
    mesh[14] = 10010; mesh[15] = 0; mesh[16] = 100;
    mesh[17] = 10000; mesh[18] = 10; mesh[19] = 100;
    f = (u8 *)(mesh + 2 + 3 * 6);
    for (i = 0; i < faces; i++) {
        s32 *w = (s32 *)(f + (size_t)i * 20);
        int base = i + 1 == faces ? 0 : 3;

        w[0] = 15;
        w[1] = base;
        w[2] = base + 1;
        w[3] = base + 2;
        w[4] = 0;
    }
    return mesh;
}

static void test_max_dyn_admission_preserved(void)
{
    s32 *mesh = make_dynamic_admission_mesh(MAX_DYN + 1);
    struct scene sc;
    struct scene_obj obj;
    struct scene_obj *objs[1];
    int started = s_failures;

    setup_scene(&sc);
    setup_object(&obj, mesh, 0, 0, 0);
    objs[0] = &obj;
    gl3d_flush();
    s_dyn_range_count = 0;
    gl3d_set_dynamic(mesh, mesh + 2 + 3 * 6 + (MAX_DYN + 1) * 5);
    reset_calls();
    s_fetch_active = 1;
    gl3d_begin(64, 64);
    gl3d_draw(&sc, objs, 1, 0, 0);

    /* The visible face is face MAX_DYN + 1 and was never admitted. */
    CHECK(s_draw_count == (TEST_CULL ? 0 : 1));
    CHECK(s_dyn_count == 0);
    gl3d_flush();
    s_dyn_range_count = 0;
    free(mesh);
    if (s_failures == started) {
        puts("MAX_DYN admission preserved      ok");
    }
}

int main(void)
{
    test_dyn_clip_sides();
    test_dyn_ambiguity_kept();
    test_dyn_straddle_and_stable_order();
    test_dyn_copy_fallback();
    test_static_clip_sides();
    test_static_straddle_mirror_and_transform();
    test_static_mesh_change_and_cache();
    test_static_deferred_kept_when_body_culled();
    test_static_degenerate_and_nonfinite();
#if TEST_CULL
    test_static_cancellation_margin();
    test_static_clip_dot_envelope();
    test_static_rotated_offset_projection();
    test_static_matrix_guards();
#endif
    test_static_texture_saturation_order();
    test_max_dyn_admission_preserved();

    if (s_failures != 0) {
        fprintf(stderr, "test_perf_visibility: %d failures\n", s_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
