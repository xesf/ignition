/*
 * menu/car.c - the front end's 3D cars.
 *
 * MENUCAR.PLC places one mesh of MENUCAR.MSH for each car; the cars all
 * stand at (1000, 1000, 1000), and car3d_draw turns one and draws it
 * with the camera set back by its scale.
 */

#include <stdlib.h>
#include <string.h>

#include "frontend/frontend.h"
#include "frontend/state.h"
#include "gl3d.h"
#include "lisa/file.h"
#include "menu/bg.h"
#include "menu/car.h"
#include "menu/pages.h"
#include "scene.h"
#include "soft3d/soft3d.h"
#include "stubs.h"

enum { MENU_CARS = 11 };

static struct scene_desc s_cars[MENU_CARS];     /* 0x4be570 */
long fe_4be630_size;                            /* ElementCount */

static void *load_whole(const char *name, long *size_out)
{
    long size = file_size(name);
    void *buf;

    if (size <= 0) {
        return NULL;
    }
    buf = calloc(1, (size_t)size + 4);
    if (buf != NULL) {
        file_read_at(name, buf, (u32)size, 0);
    }
    if (size_out != NULL) {
        *size_out = size;
    }
    return buf;
}

/* 0x40ca60 */
int menucar_load(void)
{
    struct scene *sc = g_scene;
    long tex_size = 0, pfm_size = 0;
    const s32 *plc;
    int i;

    fe_4be898 = malloc(0x1dffff);
    fe_4be804 = malloc(0x2ffff);
    fe_4be630 = load_whole("baltazar\\data\\default.psq", &fe_4be630_size);
    fe_4be5dc = 0;
    fe_4be75c = 0;
    fe_4be5f8 = load_whole("baltazar\\data\\test.pfm", &pfm_size);
    if (fe_4be5f8 == NULL || fe_4be630 == NULL) {
        /* The GOG and Steam releases (the DOS data) have the shorter film
         * only: TEST2.PFM, which DEFAULT2.PSQ plays. */
        free(fe_4be5f8);
        free(fe_4be630);
        fe_4be630 = load_whole("baltazar\\data\\default2.psq", &fe_4be630_size);
        fe_4be5f8 = load_whole("baltazar\\data\\test2.pfm", &pfm_size);
    }
    menubg_load_pages(fe_4be898);
    menubg_load_shades(fe_4be804);
    if (fe_4be5f8 != NULL) {
        menubg_parse(fe_4be5f8, pfm_size);
    }
    g_menu_background_t = 0;
    fe_4be604 = load_whole("baltazar\\data\\menucar.plc", NULL);
    fe_4be5f4 = load_whole("baltazar\\data\\menucar.msh", NULL);
    fe_4be658 = load_whole("baltazar\\data\\menucar.tex", &tex_size);
    /* 0x447280 makes slot 0 a texture handle; with these arguments it
     * builds no blurred levels. */
    g_scene_textures[0] = fe_4be658;
    g_scene_texture_pages[0] = (int)(tex_size / 0x10000);

    sc->x = 1000.0;
    sc->y = 1100.0;
    sc->z = 800.0;
    sc->target_x = 1000;
    sc->target_y = 1000;
    sc->target_z = 1000;
    sc->target_roll = 0;
    sc->center_x = g_menu_modes[g_menu_mode_index][0] / 2;
    sc->zoom = 1.0;

    plc = fe_4be604;
    for (i = 0; i < MENU_CARS; i++) {
        s_cars[i].x = 1000;
        s_cars[i].y = 1000;
        s_cars[i].z = 1000;
        s_cars[i].ax = 0;
        s_cars[i].ay = 0;
        s_cars[i].az = 0;
        scene_obj_create(&s_cars[i], (const s32 *)fe_4be5f4 + plc[1 + 5 * i], 0, 0, 0, -1, 0);
    }
    return 0;
}

/* 0x410920: frees the background's pool. */
int menucar_free(void)
{
    menubg_free();
    gl3d_flush();
    g_scene_textures[0] = NULL;
    return 0;
}

/* 0x40c5a0
 * Draws car `car` turned on by `spin` tenths of a degree, centred on
 * (x, y); the camera stands back 240 / scale up and 460 / scale away. */
int car3d_draw(s32 car, int spin, double scale, int x, int y)
{
    const s32 *plc = fe_4be604;
    const s32 *mode = g_menu_modes[g_menu_mode_index];
    struct scene *sc = g_scene;
    struct scene_desc *d;
    int i;

    if (plc == NULL) {
        return 0;
    }
    for (i = 0; i < MENU_CARS && plc[1 + 5 * i + 1] != car; i++) {
    }
    if (i == MENU_CARS) {
        i = 0;
    }
    sc->y = (double)(int)(240.0 / scale + 1000.0);
    sc->z = (double)(int)(-460.0 / scale + 1000.0);
    d = &s_cars[i];
    d->ay += spin;
    if (d->ay >= 3600) {
        d->ay -= 3600;
    }
    scene_obj_move(d);
    g_scene_visible[0] = d->obj;
    sc->obj_count = 1;

    /* The original draws a view of the menu's size centred on (x, y),
     * into the menu screen. */
    scene_look_at(sc);
    if (g_soft3d) {
        soft3d_menu_car(x - mode[0] / 2, y - mode[1] / 2, mode[0], mode[1]);
    } else if (gl3d_surface_begin(g_menu_screen, mode[0], mode[1])) {
        gl3d_draw(sc, g_scene_visible, sc->obj_count, x - mode[0] / 2, y - mode[1] / 2);
        gl3d_surface_end();
    } else {
        gl3d_begin(mode[0], mode[1]);
        gl3d_draw(sc, g_scene_visible, sc->obj_count, x - mode[0] / 2, y - mode[1] / 2);
        gl3d_end(g_menu_screen, mode[0]);
    }
    return 0;
}
