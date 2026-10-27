/* Real page-pool/backend/cache boundary using the existing upload recorder. */
#define main backend_fixture_main
#include "test_rnd_dyn_fetch.c"
#undef main

#ifndef TEST_EXPECT_CACHE
#define TEST_EXPECT_CACHE 1
#endif

static void test_pool_cache(void)
{
    unsigned a, b, pool, reused;
    int layers, vao, vbo;

    fake_reset();
    CHECK(rnd_init() != 0);
    a = rnd_texture(RND_R8, 4, 4, NULL, 0);
    b = rnd_texture(RND_R8, 4, 4, NULL, 0);
    rnd_use(RND_DYN);
    vao = s_current_vao;
    vbo = s_current_vbo;
    rnd_texture_unit(0, a);
    s_max_array_layers = 8;
    pool = rnd_page_pool_create(&layers);
    CHECK(pool != 0 && layers == 8);
    rnd_texture_unit(0, b);
    CHECK(s_active_unit == 0 && s_bound_2d[0] == b);
    CHECK(rnd_page_pool_upload(pool, 0, s_pool_pixels, 1) == 1);
    rnd_texture_unit(0, a);
    CHECK(s_active_unit == 0 && s_bound_2d[0] == a);
    CHECK(s_bound_array[7] == pool);
    CHECK(s_current_vao == vao && s_current_vbo == vbo);
    rnd_page_pool_release(pool);
    CHECK(s_bound_array[7] == 0);

    /* Failed allocations delete their GL name. Reusing it must bind the
     * new object even if the state cache previously saw the same integer. */
    reused = s_next_texture;
    s_fail_tex_image = 1;
    CHECK(rnd_page_pool_create(&layers) == 0);
    CHECK(s_bound_array[7] == 0);
    s_fail_tex_image = 0;
    s_next_texture = reused;
    pool = rnd_page_pool_create(&layers);
    CHECK(pool == reused);
    CHECK(s_bound_array[7] == pool);
    rnd_page_pool_release(pool);
    rnd_texture_free(a);
    rnd_texture_free(b);
    rnd_shutdown();
}

int main(void)
{
    int old = backend_fixture_main();
    test_pool_cache();
    if (s_failures) return 1;
    puts("pool/cache integration ok");
    return old;
}
