/* Joint arena/culling regression through real draw_dyns. Reuse the existing
 * recorder; its optional observer also captures every fallback draw's bytes.
 * The original arena suite remains the arena-only/off control. */
static void capture_draw(int first, int count);
#define PERF_STREAM_DRAW_OBSERVER capture_draw
#define main stream_arena_fixture_main
#include "test_perf_stream_arena.c"
#undef main

#if !IGN_DYN_ARENA
#error This fixture requires IGN_GLES_DYN_ARENA=1
#endif

enum { JOINT_VERTS = 64 };
static float drawn[JOINT_VERTS * DYN_FLOATS];
static int drawn_count;
static const u8 joint_table_a[256 * 256];
static const u8 joint_table_b[256 * 256];

static void capture_draw(int first, int count)
{
    const float *src = s_pass_count ? s_pass : s_stream;

    CHECK(first >= 0 && count >= 0 && drawn_count + count <= JOINT_VERTS);
    if (first < 0 || count < 0 || drawn_count + count > JOINT_VERTS) {
        return;
    }
    memcpy(drawn + drawn_count * DYN_FLOATS, src + first * DYN_FLOATS,
           (size_t)count * DYN_FLOATS * sizeof(float));
    drawn_count += count;
}

static void reset_joint_calls(void)
{
    reset_calls();
    memset(drawn, 0, sizeof(drawn));
    drawn_count = 0;
}

static void tagged_face(int i, float depth, unsigned tex, const u8 *table, int tag)
{
    int v, a;

    face(i, depth, tex, table, (float)tag);
    for (v = 0; v < 3; v++) {
        s_dyns[i].v[v][2] = 0.125f;
        for (a = 4; a < DYN_FLOATS; a++) {
            s_dyns[i].v[v][a] = (float)(tag * 100 + v * 10 + a);
        }
    }
}

/* Sorted survivor order is 2,0,3,5,8,6. At depth 20, input order wins.
 * The offscreen faces occupy the far end, a tie, and the near end. Face 3
 * crosses the right clip plane and must retain its original vertices. */
static void mixed_faces(float expected[18 * DYN_FLOATS])
{
    const int order[] = { 2, 0, 3, 5, 8, 6 };
    int i, v;

    prepare(9);
    tagged_face(0, 20, 11, joint_table_a, 1);
    tagged_face(1, 90, 99, joint_table_b, 2);
    tagged_face(2, 30, 11, joint_table_a, 3);
    tagged_face(3, 20, 12, joint_table_b, 4);
    tagged_face(4, 20, 11, joint_table_a, 5);
    tagged_face(5, 20, 11, joint_table_a, 6);
    tagged_face(6, 10, 11, NULL, 7);
    tagged_face(7, 5, 99, NULL, 8);
    tagged_face(8, 20, 11, joint_table_a, 9);
    for (i = 1; i <= 7; i += 3) {
        for (v = 0; v < 3; v++) {
            s_dyns[i].v[v][0] = 2.0f + (float)v;
        }
    }
    s_dyns[3].v[1][0] = 2.0f;
    for (i = 0; i < 6; i++) {
        memcpy(expected + i * 3 * DYN_FLOATS, s_dyns[order[i]].v,
               3 * DYN_FLOATS * sizeof(float));
    }
}

static void check_run(int at, int first, int count, unsigned tex, int table)
{
    const struct draw_rec *d = &s_draws[at];

    CHECK(d->first == first);
    CHECK(d->vertices == count);
    CHECK(d->tex == tex);
    CHECK(d->table == table);
    CHECK(d->depth_write == !table);
    CHECK(d->offset == table);
    CHECK(d->copies_before == 0);
}

static void test_joint_compacted_pass(void)
{
    float expected[18 * DYN_FLOATS];

    mixed_faces(expected);
    reset_joint_calls();
    s_fetch_active = 1;
    draw_dyns();

    CHECK(s_dyn_count == 0);
    CHECK(s_pass_count == 1);
    CHECK(s_pass_vert_count == 18);
    CHECK(s_pass_bytes == sizeof(expected));
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_copy_count == 0);
    CHECK(s_draw_count == 4);
    CHECK(drawn_count == 18);
    CHECK(memcmp(s_pass, expected, sizeof(expected)) == 0);
    CHECK(memcmp(drawn, expected, sizeof(expected)) == 0);
    check_run(0, 0, 6, 11, 1);
    check_run(1, 6, 3, 12, 1);
    check_run(2, 9, 6, 11, 1);
    check_run(3, 15, 3, 11, 0);
    CHECK(s_draws[0].unit1 != 0);
    CHECK(s_draws[1].unit1 != 0);
    CHECK(s_draws[0].unit1 != s_draws[1].unit1);
    CHECK(s_draws[2].unit1 == s_draws[0].unit1);
    CHECK(s_draws[3].first + s_draws[3].vertices == s_pass_vert_count);
    puts("joint survivor bytes/order/runs   checked");
}

static void test_joint_allocation_fallback(void)
{
    float expected[18 * DYN_FLOATS];
    int i;

    mixed_faces(expected);
    free_staging();
    reset_joint_calls();
    s_realloc_budget = 0;
    s_fetch_active = 1;
    draw_dyns();
    s_realloc_budget = -1;

    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 6);
    CHECK(s_draw_count == 6);
    CHECK(s_copy_count == 0);
    CHECK(s_dyn_count == 0);
    CHECK(drawn_count == 18);
    CHECK(memcmp(drawn, expected, sizeof(expected)) == 0);
    for (i = 0; i < 6; i++) {
        check_run(i, 0, 3, i == 2 ? 12 : 11, i != 5);
    }
    CHECK(s_draws[0].unit1 == s_draws[1].unit1);
    CHECK(s_draws[0].unit1 != s_draws[2].unit1);
    CHECK(s_draws[3].unit1 == s_draws[0].unit1);
    CHECK(s_draws[4].unit1 == s_draws[0].unit1);
    puts("joint allocation fallback bytes   checked");
}

static void test_joint_empty_then_live_pass(void)
{
    float expected[6 * DYN_FLOATS];

    free_staging();
    prepare(2);
    offscreen_face(0, 30, 91, joint_table_a, 1, 1);
    offscreen_face(1, 20, 92, NULL, 2, 1);
    reset_joint_calls();
    s_realloc_budget = 1;
    s_fetch_active = 1;
    draw_dyns();
    CHECK(s_realloc_budget == 1);
    s_realloc_budget = -1;
    CHECK(s_batch == NULL);
    CHECK(s_batch_cap == 0);
    CHECK(s_dyn_count == 0);
    CHECK(s_pass_count == 0);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_draw_count == 0);
    CHECK(s_copy_count == 0);
    CHECK(s_texture_count == 0);

    /* No recorder reset: the next pass must start at vertex zero. */
    prepare(2);
    tagged_face(0, 20, 51, joint_table_a, 11);
    tagged_face(1, 10, 52, joint_table_b, 12);
    memcpy(expected, s_dyns[0].v, 3 * DYN_FLOATS * sizeof(float));
    memcpy(expected + 3 * DYN_FLOATS, s_dyns[1].v, 3 * DYN_FLOATS * sizeof(float));
    draw_dyns();
    CHECK(s_pass_count == 1);
    CHECK(s_pass_vert_count == 6);
    CHECK(s_draw_count == 2);
    CHECK(drawn_count == 6);
    CHECK(memcmp(s_pass, expected, sizeof(expected)) == 0);
    CHECK(memcmp(drawn, expected, sizeof(expected)) == 0);
    check_run(0, 0, 3, 51, 1);
    check_run(1, 3, 3, 52, 1);
    CHECK(s_draws[0].passes_before == 1);
    CHECK(s_draws[1].passes_before == 1);
    CHECK(s_legacy_stream_calls == 0);
    CHECK(s_copy_count == 0);
    puts("joint culled then live pass       checked");
}

static void test_joint_copy_dependencies(void)
{
    float expected[18 * DYN_FLOATS];
    int batch, i;

    for (batch = 0; batch <= 1; batch++) {
        mixed_faces(expected);
        reset_joint_calls();
        s_fetch_active = 0;
        s_batch_copies = batch;
        draw_dyns();
        CHECK(s_pass_count == 0);
        CHECK(s_legacy_stream_calls == 6);
        CHECK(s_draw_count == 6);
        CHECK(s_copy_count == 5);
        CHECK(s_dyn_count == 0);
        CHECK(drawn_count == 18);
        CHECK(memcmp(drawn, expected, sizeof(expected)) == 0);
        for (i = 0; i < 6; i++) {
            CHECK(s_draws[i].first == 0);
            CHECK(s_draws[i].vertices == 3);
            CHECK(s_draws[i].tex == (unsigned)(i == 2 ? 12 : 11));
            CHECK(s_draws[i].table == (i != 5));
            /* All five table faces overlap; each reads the previous draw.
             * The final ordinary face reads no table and needs no copy. */
            CHECK(s_draws[i].copies_before == (i < 5 ? i + 1 : 5));
        }
    }
    s_batch_copies = 0;
    puts("joint nonfetch copy dependencies  checked");
}

int main(void)
{
    test_joint_compacted_pass();
    test_joint_allocation_fallback();
    test_joint_empty_then_live_pass();
    test_joint_copy_dependencies();
    free_staging();
    gl3d_flush();
    if (s_failures) {
        fprintf(stderr, "test_perf_stream_visibility: %d failures\n", s_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
