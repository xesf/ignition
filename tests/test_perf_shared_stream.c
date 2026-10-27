/* Real collect/draw entry points and the accepted arena byte recorder. */
static unsigned page_source_id;
static void capture_shared_draw(int first, int count);
#define PERF_STREAM_DRAW_OBSERVER capture_shared_draw
#define PERF_PAGE_TEXTURE_ID (900u + page_source_id++)
#define main arena_fixture_main
#include "test_perf_stream_arena.c"
#undef main

static int pool_creates, pool_uploads, pool_releases;
static int fail_pool_upload;
static float uploaded[32 * DYN_FLOATS];
static int uploaded_count;

static void capture_shared_draw(int first, int count)
{
    const float *source = s_pass_count ? s_pass : s_stream;
    CHECK(uploaded_count + count <= 32);
    if (uploaded_count + count > 32) return;
    memcpy(uploaded + uploaded_count * DYN_FLOATS, source + first * DYN_FLOATS,
           (size_t)count * DYN_FLOATS * sizeof(float));
    uploaded_count += count;
}

unsigned rnd_page_pool_create(int *layers) { pool_creates++; *layers = 256; return 1001; }
int rnd_page_pool_upload(unsigned tex, int base, const u8 *pixels, int pages)
{ (void)tex; (void)base; (void)pixels; (void)pages; pool_uploads++; return !fail_pool_upload; }
void rnd_page_pool_release(unsigned tex) { (void)tex; pool_releases++; }

static void shared_scene(int fail)
{
    static u8 pages_a[2 * 256 * 256], pages_b[3 * 256 * 256];
    static const s32 mesh[] = {
        3, 1, -10, -10, 0, 10, -10, 0, 0, 10, 0,
        18, 0, 1, 2, 0, 0, 65536, 0, 0, 65536, 1 << 16
    };
    struct scene sc = {0};
    struct scene_obj a = {0}, b = {0};
    float m[16];

    gl3d_flush();
    pool_creates = pool_uploads = pool_releases = 0;
    fail_pool_upload = fail;
    page_source_id = 0;
    s_dyn_range_count = 0;
    prepare(0);
    gl3d_begin(64, 64);
    reset_calls();
    uploaded_count = 0;
    s_fetch_active = 1;
    g_scene_textures[0] = pages_a;
    g_scene_texture_pages[0] = 2;
    g_scene_textures[1] = pages_b;
    g_scene_texture_pages[1] = 3;
    sc.focal_x = sc.focal_y = 100;
    sc.near_ofs = 1;
    sc.zoom = 1;
    a.mesh = b.mesh = mesh;
    b.tex = 1;
    a.z = b.z = 30;
    gl3d_set_dynamic(mesh, mesh + sizeof(mesh) / sizeof(mesh[0]));
    object_matrix(&sc, &a, 0, 0, m);
    collect(&sc, &a, m, 1);
    object_matrix(&sc, &b, 0, 0, m);
    collect(&sc, &b, m, 1);
    CHECK(s_dyn_count == 2);
    draw_dyns();
    CHECK(s_draw_count == (IGN_SHARED_PAGES_BUILD && !fail ? 1 : 2));
    CHECK(s_pass_count == 1 && s_pass_vert_count == 6);
    CHECK(pool_creates == (IGN_SHARED_PAGES_BUILD ? 1 : 0));
    CHECK(pool_uploads == (IGN_SHARED_PAGES_BUILD ? 2 : 0));
    CHECK(s_draws[0].tex == (unsigned)(IGN_SHARED_PAGES_BUILD && !fail ? 1001 : 900));
    CHECK(s_pass[6] == 1 && s_pass[3 * DYN_FLOATS + 6] == (IGN_SHARED_PAGES_BUILD && !fail ? 3 : 1));
    gl3d_flush();
    CHECK(pool_releases == (IGN_SHARED_PAGES_BUILD ? 1 : 0));
}

static void mixed_survivors(int fail_allocation)
{
    static u8 pages_a[2 * 256 * 256], pages_b[3 * 256 * 256];
    static const u8 table_a[256 * 256], table_b[256 * 256];
    s32 mesh[] = {3, 1, -10, -10, 0, 10, -10, 0, 0, 10, 0,
                   18, 0, 1, 2, 0, 0, 65536, 0, 0, 65536, 1 << 16};
    const int order[] = {0, 2, 3, 4, 5, 6};
    const int slots[] = {0, 1, 1, 0, 1, 1, 0};
    const int depths[] = {30, 30, 30, 20, 20, 10, 5};
    float expected[18 * DYN_FLOATS], raw[18 * DYN_FLOATS], m[16];
    struct scene sc = {0};
    int i, v;

    gl3d_flush();
    fail_pool_upload = 0;
    pool_creates = pool_uploads = pool_releases = 0;
    page_source_id = 0;
    s_dyn_range_count = 0;
    prepare(0);
    gl3d_begin(64, 64);
    reset_calls();
    uploaded_count = 0;
    s_fetch_active = 1;
    g_scene_textures[0] = pages_a; g_scene_texture_pages[0] = 2;
    g_scene_textures[1] = pages_b; g_scene_texture_pages[1] = 3;
    sc.focal_x = sc.focal_y = 100; sc.near_ofs = 1; sc.zoom = 1;
    sc.center_x = sc.center_y = 32;
    gl3d_set_dynamic(mesh, mesh + sizeof(mesh) / sizeof(mesh[0]));
    for (i = 0; i < 7; i++) {
        struct scene_obj object = {0};
        object.mesh = mesh; object.tex = slots[i]; object.z = depths[i];
        if (i == 1) object.x = 100000;
        mesh[11] = i >= 3 && i <= 5 ? 19 : 18;
        mesh[21] = i == 6 ? (s32)0xffff0000u : 1 << 16;
        s_tables_shd = i == 4 ? table_b : table_a;
        object_matrix(&sc, &object, 0, 0, m);
        collect(&sc, &object, m, 1);
    }
    CHECK(s_dyn_count == 7);
    for (i = 0; i < 6; i++) {
        memcpy(raw + i * 3 * DYN_FLOATS, s_dyns[order[i]].v,
               3 * DYN_FLOATS * sizeof(float));
    }
    memcpy(expected, raw, sizeof(raw));
    if (IGN_SHARED_PAGES_BUILD) {
        for (i = 0; i < 5; i++) for (v = 0; v < 3; v++)
            expected[(i * 3 + v) * DYN_FLOATS + 6] += slots[order[i]] ? 2 : 0;
    }
    if (fail_allocation) { free_staging(); s_realloc_budget = 0; }
    draw_dyns();
    s_realloc_budget = -1;
    CHECK(s_dyn_count == 0 && uploaded_count == 18);
    CHECK(memcmp(uploaded, fail_allocation ? raw : expected, sizeof(expected)) == 0);
    CHECK(s_copy_count == 0);
    if (fail_allocation) {
        CHECK(s_pass_count == 0 && s_legacy_stream_calls == 6 && s_draw_count == 6);
        for (i = 0; i < 6; i++) {
            CHECK(s_draws[i].first == 0 && s_draws[i].vertices == 3);
            CHECK(s_draws[i].tex == (unsigned)(900 + slots[order[i]]));
        }
    } else {
        CHECK(s_pass_count == 1 && s_pass_vert_count == 18 && s_legacy_stream_calls == 0);
        CHECK(s_draw_count == (IGN_SHARED_PAGES_BUILD ? 5 : 6));
        if (IGN_SHARED_PAGES_BUILD) {
            const int starts[] = {0, 6, 9, 12, 15};
            for (i = 0; i < 5; i++) {
                CHECK(s_draws[i].first == starts[i]);
                CHECK(s_draws[i].vertices == (i == 0 ? 6 : 3));
                CHECK(s_draws[i].tex == (unsigned)(i == 4 ? 900 : 1001));
                CHECK(s_draws[i].table == (i >= 1 && i <= 3));
            }
            CHECK(s_draws[1].unit1 == s_draws[3].unit1);
            CHECK(s_draws[1].unit1 != s_draws[2].unit1);
        }
    }
    gl3d_flush();
}

int main(void)
{
    init();
    shared_scene(0);
    shared_scene(0); /* Real flush/recollection must restart layer allocation. */
    shared_scene(1);
    mixed_survivors(0);
    mixed_survivors(1);
    free_staging();
    puts(s_failures ? "FAIL" : "ok");
    return s_failures != 0;
}
