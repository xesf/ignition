/* The reviewed state recorder drives the real backend's arena entry points.
 * Samplers, VAO/VBO identity and all nine DYN floats are observed at draws. */
#define main state_fixture_main
#include "test_perf_state_cache.c"
#undef main

static void check_pass_draw(const float *vertices, int first, int count,
                            GLuint program, GLuint vao, GLuint buffer,
                            unsigned tex, unsigned table)
{
    const struct draw_snapshot *d = &s_gl.last_draw;
    int a;

    CHECK(d->program == program && d->vao == vao);
    CHECK(d->first == first && d->count == count);
    CHECK(d->tex2d[0] == tex && d->tex2d[1] == table);
    CHECK(d->attrs[0].bytes >= (size_t)(first + count) * 9 * sizeof(float));
    CHECK(memcmp(d->attrs[0].data + first * 9 * sizeof(float), vertices + first * 9,
                 (size_t)count * 9 * sizeof(float)) == 0);
    for (a = 0; a < 3; a++) {
        CHECK(d->attrs[a].enabled);
        CHECK(d->attrs[a].stride == 9 * (int)sizeof(float));
        CHECK(d->attrs[a].buffer == buffer);
    }
    check_uniform1i(RND_U_USE_TABLE, 1);
    check_draw_depth(1, GL_LEQUAL, GL_FALSE);
    check_draw_poly(1, -1.0f, -4.0f);
}

static void test_state_arena(int fetch)
{
#if TEST_HAVE_FETCH_BUILD
    static const char *const extension[] = {"GL_EXT_shader_framebuffer_fetch"};
#endif
    float pass[9 * 9], next[3 * 9], canvas[8] = {0};
    float use_table = 1;
    unsigned tex_a, tex_b, table, scratch;
    GLuint program, vao, buffer;
    struct draw_snapshot first_pass;
    int i;

    for (i = 0; i < 9 * 9; i++) pass[i] = (float)(i + 1);
    for (i = 0; i < 3 * 9; i++) next[i] = (float)(i + 301);
    fake_reset();
#if TEST_HAVE_FETCH_BUILD
    if (fetch) fake_extensions(extension, 1);
#endif
    CHECK(rnd_init() != 0);
    CHECK(rnd_dyn_framebuffer_fetch() == (TEST_HAVE_FETCH_BUILD && fetch));
    tex_a = rnd_texture(RND_R8, 4, 4, NULL, 0);
    tex_b = rnd_texture(RND_R8, 4, 4, NULL, 0);
    table = rnd_texture(RND_R8, 4, 4, NULL, 0);
    scratch = rnd_texture(RND_R8, 4, 4, NULL, 0);
    rnd_use(RND_DYN);
    rnd_texture_unit(0, tex_a);
    rnd_texture_unit(1, table);
    rnd_uniform(RND_U_USE_TABLE, &use_table);
    rnd_depth(RND_DEPTH_LEQUAL, 0);
    rnd_offset(1);
    rnd_stream_pass(pass, 9);
    rnd_draw(RND_TRIANGLES, 0, 6);
    program = s_gl.last_draw.program;
    vao = s_gl.last_draw.vao;
    buffer = s_gl.last_draw.attrs[0].buffer;
    check_pass_draw(pass, 0, 6, program, vao, buffer, tex_a, table);
    first_pass = s_gl.last_draw;

    /* Repeated material state may disappear; the second run and its
     * post-arena vertex offset must still reach the driver unchanged. */
    calls_clear();
    rnd_use(RND_DYN);
    rnd_texture_unit(1, table);
    rnd_uniform(RND_U_USE_TABLE, &use_table);
    rnd_depth(RND_DEPTH_LEQUAL, 0);
    rnd_offset(1);
    CHECK(s_calls.use_program == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.bind_vao == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.uniform1i == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    rnd_texture_unit(0, tex_b);
    rnd_draw(RND_TRIANGLES, 6, 3);
    check_pass_draw(pass, 6, 3, program, vao, buffer, tex_b, table);

    /* Work-unit uploads/copies, then another program's stream, cannot
     * replace DYN's uploaded arena or sampler bindings. */
    rnd_texture_set(scratch, 4, 4, NULL, 0);
    rnd_copy(scratch, 0, 0, 4, 4);
    rnd_use(RND_CANVAS);
    rnd_stream(canvas, 2);
    rnd_use(RND_DYN);
    rnd_draw(RND_TRIANGLES, 0, 6);
    check_pass_draw(pass, 0, 6, program, vao, buffer, tex_b, table);

    /* A smaller next pass still uploads new bytes after orphaning the
     * retained arena; the earlier draw snapshot keeps its original data. */
    rnd_stream_pass(next, 3);
    rnd_draw(RND_TRIANGLES, 0, 3);
    check_pass_draw(next, 0, 3, program, vao, buffer, tex_b, table);
    CHECK(memcmp(first_pass.attrs[0].data, pass, sizeof(pass)) == 0);
    rnd_texture_free(tex_a);
    rnd_texture_free(tex_b);
    rnd_texture_free(table);
    rnd_texture_free(scratch);
    rnd_shutdown();
}

int main(void)
{
    int original = state_fixture_main();
    test_state_arena(0);
    test_state_arena(1);
    if (s_failures) {
        fprintf(stderr, "state + arena integration: %d failures\n", s_failures);
        return 1;
    }
    puts("state + arena integration ok");
    return original;
}
