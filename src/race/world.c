/*
 * race/world.c - the race's 3D world (0x41b470): the scene, its
 * textures, and an object for each placement of the track.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "game.h"
#include "platform/platform.h"
#include "fx3d.h"
#include "gl3d.h"
#include "prof.h"
#include "lisa/font.h"
#include "lisa/gfx.h"
#include "lisa/log.h"
#include "lisa/mem.h"
#include "race/race.h"
#include "race/cars.h"
#include "race/init.h"
#include "structs/coll_obj.h"
#include "scene.h"
#include "soft3d/soft3d.h"
#include "startup.h"
#include "stubs.h"
#include "video.h"

struct scene_desc *g_world;             /* 0x54f908 */
s32 r_552d80, r_552f5c;                 /* read by the car 7 setup */
s32 r_563c80[0x18];                     /* 0x563c80, view sizes */
s32 r_563d98;
s32 r_54f9d4, r_527f2c;
s32 r_54f954, r_563cf0, r_563d78, r_552f78, r_54f9b8;

/* Collision objects (0x54f904, 0x98 bytes), sound sources (0x553078),
 * lens flares (0x563c68) and moving objects (0x525e6c). */
struct coll_obj *g_coll_objs;           /* 0x54f904 */
struct sound_src *g_sound_srcs;         /* 0x553078 */
struct flare *g_flares;                 /* 0x563c68 */
struct scene_desc *g_flare_objs;        /* 0x553460 */
struct mover *g_movers;                 /* 0x525e6c */
struct scene_desc *g_car_objs;          /* 0x5db02c */
struct scene_desc *g_wheel_objs;        /* 0x552fbc, 4 a car */
struct scene_desc *g_car7_objs;         /* 0x552f70 */
struct scene_desc *g_shadow_objs;       /* 0x552f20 */
struct scene_desc *g_arrow_objs;        /* 0x553788, cars then 2 arrows */
s32 r_54f930;                           /* set: cars and shadows unlinked */

/* The lens flare's sprite, a mesh of one point (0x498448). */
static s32 s_flare_mesh[14] = {
    1, 1, 0, 0, 0, 7, 0, 0, 0, 0x4000, 0x4000, 0, 6553500, 6553500
};
/* The arrow over the players' cars (0x552f80). */
static s32 s_arrow_mesh[14] = {
    1, 1, 0, -100, 0, 8, 0, 0x1f00, 0x7500, 0x4a00, 0x9900, 0, 0xc000, 0xb333
};
/* Network players' names over their cars (0x639940, 0x3c each). */
static s32 s_name_meshes[RACE_DRIVERS][15];

static int skipped(int i, int type)
{
    if (r_553084) {
        return 0;
    }
    return (type >= 300 && type < 350) || (type >= 150 && type <= 199) || type == 357
        || ((type == 350 || type == 351) && g_level_pos[i] != -1);
}

static void create(int i, struct plc_entry *e)
{
    struct scene_desc *d = &g_world[i];

    if (scene_obj_create(d, g_level_msh + e->mesh, 0, e->type, g_road_order[i].order, -1,
                         g_plc_extra[i].f0) == -1) {
        log_printf("Error while creating the world");
        exit(1);
    }
    d->x = e->x + 0x6400;
    d->y = e->y;
    d->z = e->z + 0x6400;
    d->ax = d->ay = d->az = 0;
    scene_obj_link(d);
}

static void *pool_calloc(size_t size)
{
    void *p = mem_calloc(g_race_pool, (u32)size);

    g_race_bytes += (s32)size;
    if (p == NULL) {
        mem_out_of_memory();
    }
    return p;
}

/* 0x41bb1f */
static void make_collision_objects(void)
{
    int n = g_level_plc[0], i, k = 0, phase = 0;

    if (r_54f954 <= 0) {
        return;
    }
    g_coll_objs = pool_calloc((size_t)r_54f954 * sizeof(*g_coll_objs));
    for (i = 0; i < n; i++) {
        s32 t = plc_at(g_level_plc, i)->type;

        if (t >= 150 && t < 200) {
            g_coll_objs[k++].plc = i;
        }
    }
    for (k = 0; k < r_54f954; k++) {
        struct coll_obj *o = &g_coll_objs[k];
        const struct plc_entry *p = plc_at(g_level_plc, o->plc);

        o->type = p->type - 150;
        o->x = p->x + 0x6400;
        o->y = p->y;
        o->z = p->z + 0x6400;
        memset(o->f24, 0, sizeof(o->f24));
        o->f08 = 0;
        switch (p->type) {
        case 150:
        case 154:
            /* These go round in phases of 4 to 30. */
            o->f84 = 0.3f;
            o->f88 = 100.0f;
            o->f90 = (float)phase;
            o->f94 = 30;
            phase += 4;
            if (o->f94 <= phase) {
                phase = 0;
            }
            break;
        case 152:
            o->f84 = 25.0f;
            o->f88 = 8000.0f;
            break;
        case 153:
            o->f84 = 2.0f;
            o->f88 = 1500.0f;
            break;
        default:
            log_printf("Ingen info om kollisions objektet!!\n");
            exit(0);
        }
    }
}

/* 0x41be37 */
static void make_sound_sources(int first357)
{
    int n = g_level_plc[0], i, k = 0;

    if (r_563cf0 <= 0) {
        return;
    }
    g_sound_srcs = pool_calloc((size_t)r_563cf0 * sizeof(*g_sound_srcs));
    for (i = 0; i < n; i++) {
        s32 t = plc_at(g_level_plc, i)->type;

        if (!(t >= 400 && t < 499) && i != first357) {
            continue;
        }
        if (t == 406 && g_track == 1 && !r_553084 && i != first357) {
            continue;
        }
        g_sound_srcs[k].plc = i;
        g_sound_srcs[k].voice = race_sound_3d(1, 0, i == first357 ? 0 : t - 400, -1, 0, 11000,
                                              0, 0.0f, 0.0f);
        k++;
    }
}

/* 0x41bf2b */
static void make_flares(void)
{
    int n = g_level_plc[0], i, e = 0, k = 0;

    if (r_563d78 <= 0) {
        return;
    }
    g_flares = pool_calloc((size_t)r_563d78 * sizeof(*g_flares));
    if (r_54f9b8 > 0) {
        g_flare_objs = pool_calloc((size_t)r_54f9b8 * sizeof(*g_flare_objs));
    }
    for (i = 0; i < n; i++) {
        const struct plc_entry *p = plc_at(g_level_plc, i);
        s32 t = p->type;

        if (t >= 350 && t < 360 && !(t == 357 && !r_553084)
            && !((t == 350 || t == 351) && g_level_pos[i] != -1 && !r_553084)) {
            g_flares[e].plc = i;
            g_flares[e].kind = t - 350;
            if (g_flares[e].kind == 4 && r_54f9b8 > 0) {
                struct scene_desc *d = &g_flare_objs[k++];

                if (scene_obj_create(d, s_flare_mesh, 3, 200, 0, -1, 0) != 0) {
                    log_printf("Error while creating lens flaires");
                    exit(1);
                }
                d->x = p->x + 0x6400;
                d->y = p->y + 80;
                d->z = p->z + 0x6400;
                d->ax = d->ay = d->az = 0;
                scene_obj_link(d);
            }
            e++;
        }
        if (t == 304 && r_553084 == 1) {
            g_flares[e].plc = i;
            g_flares[e].kind = t - 350;
            e++;
        }
    }
}

/* 0x41c1aa */
static void make_movers(void)
{
    int n = g_level_plc[0], i, k = 0;
    s32 step = 0;

    if (r_552f78 <= 0) {
        return;
    }
    g_movers = pool_calloc((size_t)r_552f78 * sizeof(*g_movers));
    for (i = 0; i < n; i++) {
        s32 t = plc_at(g_level_plc, i)->type;

        if (t < 300 || t >= 350 || (t == 302 && g_track == 1 && !r_553084)) {
            continue;
        }
        g_movers[k].plc = i;
        g_movers[k].type = t;
        if (t == 305) {
            /* Type 305 objects start 8 frames apart, up to 32. */
            g_movers[k].phase = step;
            step += 8;
            if (step > 0x20) {
                step = 0;
            }
        } else {
            g_movers[k].phase = 0;
        }
        g_movers[k].f0c = 0;
        k++;
    }
}

/* Puts a car's object desc at its car, turned as it is. */
static void at_car(struct scene_desc *d, const struct car *c)
{
    d->x = (s32)c->x;
    d->y = (s32)c->y;
    d->z = (s32)c->z;
    d->ax = (s32)(c->f108 * 572.957795130822);
    d->ay = (s32)(c->heading * 572.957795130822);
    d->az = (s32)(c->f110 * 572.957795130822);
}

/* The shadows (a port's change): the original's is the collisions' box
 * (g_car_hulls) at the ground, two faces of type 20 through the level's
 * shades. Here it is an octagon over the same box, its corners cut by
 * a third of the narrow side, six faces of the same shade. */
enum { SHADOW_VERTS = 8, SHADOW_FACES = 6 };
static s32 s_shadows[RACE_DRIVERS][2 + 3 * SHADOW_VERTS + 5 * SHADOW_FACES + 1];

static void shadow_shape(int i)
{
    const struct car *c = &g_cars[i];
    s32 *m = s_shadows[i];
    double x0 = 1e9, x1 = -1e9, z0 = 1e9, z1 = -1e9, cut;
    double px[SHADOW_VERTS], pz[SHADOW_VERTS];
    int k;

    for (k = 0; k < 4; k++) {
        x0 = fmin(x0, c->corner_x[k]);
        x1 = fmax(x1, c->corner_x[k]);
        z0 = fmin(z0, c->corner_z[k] * 1.2);
        z1 = fmax(z1, c->corner_z[k] * 1.2);
    }
    cut = fmin(x1 - x0, z1 - z0) / 3.0;
    px[0] = x0 + cut; pz[0] = z0;
    px[1] = x1 - cut; pz[1] = z0;
    px[2] = x1;       pz[2] = z0 + cut;
    px[3] = x1;       pz[3] = z1 - cut;
    px[4] = x1 - cut; pz[4] = z1;
    px[5] = x0 + cut; pz[5] = z1;
    px[6] = x0;       pz[6] = z1 - cut;
    px[7] = x0;       pz[7] = z0 + cut;
    m[0] = SHADOW_VERTS;
    m[1] = SHADOW_FACES;
    for (k = 0; k < SHADOW_VERTS; k++) {
        m[2 + 3 * k] = (s32)px[k];
        m[3 + 3 * k] = 0;
        m[4 + 3 * k] = (s32)pz[k];
    }
    for (k = 0; k < SHADOW_FACES; k++) {
        s32 *f = m + 2 + 3 * SHADOW_VERTS + 5 * k;

        f[0] = 20;
        f[1] = 0;
        f[2] = k + 1;
        f[3] = k + 2;
        f[4] = 40;
    }
}

/* 0x41c29f */
static void make_car_objects(void)
{
    int i, w;

    g_car_objs = pool_calloc((size_t)g_driver_count * sizeof(*g_car_objs));
    g_arrow_objs = pool_calloc((size_t)(g_driver_count + 2) * sizeof(*g_arrow_objs));
    g_race_bytes -= 2 * (s32)sizeof(*g_arrow_objs);
    g_wheel_objs = pool_calloc((size_t)g_driver_count * 4 * sizeof(*g_wheel_objs));
    g_car7_objs = pool_calloc((size_t)g_driver_count * sizeof(*g_car7_objs));
    g_shadow_objs = pool_calloc((size_t)g_driver_count * sizeof(*g_shadow_objs));
    for (i = 0; i < g_driver_count; i++) {
        const struct car *c = &g_cars[i];
        const struct race_driver *d = &g_drivers[i];

        at_car(&g_car_objs[i], c);
        if (scene_obj_create(&g_car_objs[i], g_car_meshes[d->car], 1, 100 + i, 0, -1, 0) != 0) {
            log_printf("Error while creating car\n");
            exit(1);
        }
        if (!r_54f930) {
            scene_obj_link(&g_car_objs[i]);
        }
        for (w = 0; w < 4; w++) {
            struct scene_desc *wd = &g_wheel_objs[4 * i + w];

            at_car(wd, c);
            if (scene_obj_create(wd, g_driver_wheels[i][w], 1, 210, 0, -1, 0) != 0) {
                log_printf("Error while creating wheel\n");
                exit(1);
            }
            scene_obj_link(wd);
        }
        if (d->car == 7 && !r_54f930) {
            at_car(&g_car7_objs[i], c);
            if (scene_obj_create(&g_car7_objs[i], g_car7[i], 2, 200, 0, -1, 0) != 0) {
                log_printf("Error while creating car\n");
                exit(1);
            }
            scene_obj_link(&g_car7_objs[i]);
        }
        at_car(&g_shadow_objs[i], c);
        shadow_shape(i);
        if (scene_obj_create(&g_shadow_objs[i], s_shadows[i], 0, 200, 0, -1, 0) != 0) {
            log_printf("Error while creating shadow\n");
            exit(1);
        }
        if (!r_54f930) {
            scene_obj_link(&g_shadow_objs[i]);
        }
        if (d->control == 2 && i != 0) {
            /* A network player's name, drawn into the blank page. */
            static const s32 name[15] = {
                1, 1, 0, -100, 0, 8, 0, 0, 0, 0x1700, 0x900, 0, 0x18000, 0x16666, 0
            };
            struct scene_desc *ad = &g_arrow_objs[i];

            memcpy(s_name_meshes[i], name, sizeof(name));
            s_name_meshes[i][7] = i * 0x1800;
            s_name_meshes[i][9] = i * 0x1800 + 0x1700;
            ad->x = (s32)c->x;
            ad->y = (s32)c->y;
            ad->z = (s32)c->z;
            ad->ax = ad->ay = ad->az = 0;
            if (scene_obj_create(ad, s_name_meshes[i], 8, 200, 0, -1, 0) != 0) {
                log_printf("Error while creating car name obj\n");
                exit(1);
            }
            scene_obj_link(ad);
        }
    }
    /* The arrows over the players' cars, made but shown later. */
    for (i = 0; i < 1 + (g_two_players == 1); i++) {
        struct scene_desc *ad = &g_arrow_objs[g_driver_count + i];
        const struct car *c = &g_cars[i];

        ad->x = (s32)c->x;
        ad->y = (s32)c->y;
        ad->z = (s32)c->z;
        ad->ax = ad->ay = ad->az = 0;
        if (scene_obj_create(ad, s_arrow_mesh, 2, 200, 0, -1, 0) != 0) {
            log_printf("Error while creating car arrow obj\n");
            exit(1);
        }
    }
    /* A network game: every driver's name into the blank texture page,
     * 24 pixels apart along its top rows, where the name meshes above
     * take them from (0x41cc62). The page is uploaded when it is first
     * drawn, after this. */
    if (g_race_net.mode != 2) {
        int font = g_fonts_menu[6];

        set_target(g_race_tex[RACE_TEX_SLOTS - 1], 0x100, 0x100, 0x0a, 8);
        g_gfx_color_table = g_identity_table;
        g_fonts[font].align = FONT_ALIGN_CENTER;
        for (i = 0; i < g_driver_count; i++) {
            text_draw(g_drivers[i].name, font, 0x0c + 0x18 * i, 0);
        }
        g_gfx_color_table = g_level_pan;
        g_fonts[font].align = FONT_ALIGN_LEFT;
        set_target(g_back_buffer, g_screen_width, g_screen_width, g_screen_height, 8);
    }
}

/* 0x41b470 */
void race_make_world(void)
{
    static const s32 sizes[0x18] = {
        1, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 1, 1, 0
    };
    struct scene *sc = scene_create();
    int n = g_level_plc[0], i, first357 = -1;

    sc->perspective = r_525e68;
    sc->zoom = g_mirror >= 3 ? -1.0 : 1.0;
    for (i = 0; i < RACE_TEX_SLOTS; i++) {
        g_scene_textures[i] = g_race_tex[i];
        g_scene_texture_pages[i] = g_race_tex_pages[i];
    }
    /* 0x447280 builds blurred levels of the track's texture when
     * r_527f78 is set; GL samples the pages as they are. */
    if (g_soft3d) {
        soft3d_race_textures();
    }
    memcpy(r_563c80, sizes, sizeof(sizes));
    r_527f2c = g_screen_size != 0 ? 7 : 6;
    r_54f9d4 = r_563c80[g_two_players + 2 * (r_527f28 + 4 * g_screen_size)];

    if (raster_set_grid(0xc800, 0xc800, 1500) == -1) {
        mem_out_of_memory();
    }
    g_world = pool_calloc((size_t)n * sizeof(*g_world));
    r_54f954 = r_563cf0 = r_563d78 = r_552f78 = r_54f9b8 = 0;
    for (i = 0; i < n; i++) {
        struct plc_entry *e = plc_at(g_level_plc, i);

        if (e->type < 100) {
            create(i, e);
        }
    }
    for (i = 0; i < n; i++) {
        struct plc_entry *e = plc_at(g_level_plc, i);
        s32 t = e->type;

        if (t < 100 || skipped(i, t)) {
            continue;
        }
        create(i, e);
        if (t >= 150 && t < 200) {
            r_54f954++;
        }
        if (t >= 400 && t < 500 && !(t == 406 && g_track == 1 && !r_553084)) {
            r_563cf0++;
        }
        if (((t >= 350 && t < 360) || t == 304) && !(t == 357 && !r_553084)) {
            r_563d78++;
            if (t == 357 && first357 == -1) {
                first357 = i;
                r_563cf0++;
            }
        }
        if (t >= 300 && t < 350 && !(t == 302 && g_track == 1 && !r_553084)) {
            r_552f78++;
        }
        if (t == 354) {
            r_54f9b8++;
        }
    }
    make_collision_objects();
    make_sound_sources(first357);
    make_flares();
    make_movers();
    make_car_objects();
    r_563d98 = 0;
    sc->gather_radius = g_track == 2 ? 25 : 0;
}

/* An all zero screen: the back buffer's colour 0 shows the 3D. */
static const u8 s_clear[SCREEN_MAX_W * SCREEN_MAX_H];

/* Rows the view fills with a colour before drawing, as the original
 * fills the back buffer (the sky, track 2's band). */
static struct { double y0, y1; int index; } s_fills[4];
static int s_fill_count;

void race_view_fill(double y0, double y1, int index)
{
    if (g_soft3d) {
        soft3d_race_fill((int)y0, (int)y1, index);
        return;
    }
    if (s_fill_count < 4) {
        s_fills[s_fill_count].y0 = y0;
        s_fills[s_fill_count].y1 = y1;
        s_fills[s_fill_count].index = index;
        s_fill_count++;
    }
}

/* Draws the 3D at the drawable's size, its field of view widened to
 * the drawable's shape; the back buffer is left clear for the HUD, and
 * presenting shows it over the 3D where it is not 0. */
void race_draw_view(const struct scene *sc, struct scene_obj *const *objs, int n, int x0, int x1,
                    int first)
{
    int w, h;
    double vw, ox;

    if (g_soft3d) {
        soft3d_race_view(first);
        return;
    }
    if (first) {
        memset(g_back_buffer, 0, (size_t)g_screen_width * g_screen_height);
    }
    video_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }
    /* The race's screen (320 x 200 .. 800 x 600, or NATIVE's) fills the
     * drawable's height; a drawable wider than the screen sees more at
     * the sides. The view is drawn at the internal resolution, as wide
     * as the drawable's shape. */
    {
        /* 320 x 200's pixels are 1.2 times as tall as they are wide. */
        double tall = g_screen_width == 320 && g_screen_height == 200 ? 1.2 : 1.0;
        int w43, ih;

        vw = g_screen_height * tall * w / h;
        canvas_internal_size(&w43, &ih);
        /* A host may draw the 3D at less than its full height
         * (g_view_scale); it is shown stretched to the drawable. */
        ih = (int)(ih * g_view_scale + 0.5);
        w = (int)((double)ih * w / h + 0.5);
        h = ih;
    }
    ox = (vw - g_screen_width) / 2;
    if (first) {
        gl3d_view_begin(w, h, vw, g_screen_height);
    } else {
        gl3d_view_resume();
    }
    /* The fills queued for the frame (both views'), then the view's
     * columns of the screen; one at an edge takes in the drawable past
     * it. */
    for (int i = 0; i < s_fill_count; i++) {
        gl3d_view_fill(s_fills[i].y0, s_fills[i].y1, s_fills[i].index);
    }
    s_fill_count = 0;
    if (x0 > 0 || x1 < g_screen_width) {
        gl3d_view_columns(x0 <= 0 ? -1e9 : x0 + ox, x1 >= g_screen_width ? 1e9 : x1 + ox);
    }
    gl3d_draw(sc, objs, n, ox, 0);
    gl3d_view_columns(0, 0);
    prof_begin(PROF_VIEW_END);
    gl3d_view_end();
    prof_end(PROF_VIEW_END);
    video_set_layer(gl3d_view_texture(), s_clear);
    if (fx3d_enabled()) {
        video_set_overlay(gl3d_particle_texture());
    }
}

/* Until the view (0x436990) is ported: the chase camera of the car
 * being watched, drawn into the back buffer. Angles are in tenths of a
 * degree. */
void race_debug_view(void)
{
    struct scene *sc = g_scene;
    static struct scene_obj *objs[3000];
    const struct race_cam *cam;
    const struct car *c;
    int n;

    if (sc == NULL || g_cars == NULL || g_race_cams == NULL) {
        return;
    }
    c = &g_cars[g_view_car];
    cam = &g_race_cams[g_view_car];
    sc->stage_lookat = 0;
    sc->x = c->look_back ? cam->back_x : cam->x;
    sc->z = c->look_back ? cam->back_z : cam->z;
    sc->y = cam->height + cam->f38;
    sc->pitch = cam->f40 + cam->f48 + cam->f30;
    sc->yaw = c->look_back ? cam->back_yaw : cam->yaw;
    sc->roll = 0.0;
    n = scene_gather(sc, 24, objs, 3000);
    race_draw_view(sc, objs, n, 0, g_screen_width, 1);
    video_set_palette(g_level_col + 8);
    present_back_buffer();
}

/* An object at the car's place, with angles in tenths of a degree. */
static void place_at(struct scene_desc *d, const struct car *c, s32 ax, s32 az)
{
    d->x = (s32)c->x;
    d->y = (s32)c->y;
    d->z = (s32)c->z;
    d->ax = ax;
    d->ay = (s32)(c->heading * 57.295779513082195 * 10.0);
    d->az = az;
}

/* A front wheel turned by the steering: the model's wheel rotated about
 * its hub, then put back at the hub. */
static void steer_wheel(s32 *dst, int w, int model, double cs, double sn)
{
    const s32 *src = g_wheel_meshes[4 * model + w];
    int v;

    for (v = 0; v < g_wheel_info[4 * model + w].verts; v++) {
        double x = src[2 + 3 * v], z = src[4 + 3 * v];

        dst[2 + 3 * v] = (s32)(x * cs - sn * z + g_wheel_dx[4 * model + w]);
        dst[4 + 3 * v] = (s32)(cs * z + sn * x + g_wheel_dz[4 * model + w]);
    }
}

/* 0x43d540
 * Puts car i's objects where the car is: the body (and car 7's extra
 * part), the front wheels turned by the steering, the four wheels, the
 * shadow on the ground (pushed out by the height, as the sun is low)
 * and the name over other players' cars. A squashed car's wheels lie
 * flat. Also sets the box the collisions use. */
void race_place_car(int i)
{
    const struct car *c = &g_cars[i];
    int model = g_drivers[i].car, w, v;
    double up = c->y - c->f120, sx, sy, sz;
    double t = c->steer_shown * 0.017453292519943334, cs = cos(t), sn = sin(t);
    struct car_hull *h = &g_car_hulls[i];

    place_at(&g_car_objs[i], c, (s32)c->f584, (s32)c->f58c);
    if (r_54f930 == 0 && scene_obj_move(&g_car_objs[i]) != 0) {
        log_printf("FEL VID LI_MOVEOBJECT HANDLE CAR\n");
    }
    if (c->f558 == 0) {
        steer_wheel(g_driver_wheels[i][0], 0, model, cs, sn);
        steer_wheel(g_driver_wheels[i][1], 1, model, cs, sn);
    } else {
        for (w = 0; w < 4; w++) {
            const s32 *src = g_wheel_meshes[4 * model + w];
            s32 *dst = g_driver_wheels[i][w];

            for (v = 0; v < g_wheel_info[4 * model + w].verts; v++) {
                dst[2 + 3 * v] = (s32)((double)src[2 + 3 * v] + g_wheel_dx[4 * model + w]);
                dst[3 + 3 * v] = src[4 + 3 * v];
                dst[4 + 3 * v] = (s32)((double)src[3 + 3 * v] + g_wheel_dz[4 * model + w] * 1.5);
            }
        }
    }
    for (w = 0; w < 4; w++) {
        struct scene_desc *d = &g_wheel_objs[4 * i + w];

        d->x = (s32)c->x;
        d->y = (s32)c->y;
        d->z = (s32)c->z;
        d->ax = (s32)(c->f108 * 572.957795130822);
        d->ay = (s32)(c->heading * 572.957795130822);
        d->az = (s32)(c->f110 * 572.957795130822);
        if (scene_obj_move(d) != 0) {
            log_printf("FEL VID LI_MOVEOBJECT HANDLE WHEEL\n");
        }
    }
    if (model == 7 && r_54f930 == 0) {
        place_at(&g_car7_objs[i], c, (s32)c->f584, (s32)c->f58c);
        if (scene_obj_move(&g_car7_objs[i]) != 0) {
            log_printf("FEL VID LI_MOVEOBJECT HANDLE CAR\n");
        }
    }
    sx = c->x + up;
    sz = c->z + up;
    sy = c->f120;
    if (!(0.0 <= sx) || !(sx <= 50000.0)) {
        sx = 28000.0;
    }
    if (sy < -2500.0 || sy > 2500.0) {
        sx = 0.0;
    }
    if (!(0.0 <= sz) || !(sz <= 50000.0)) {
        sz = 28000.0;
    }
    shadow_shape(i);
    h->v[2] = (s32)c->corner_x[2];
    h->v[4] = (s32)(c->corner_z[2] * 1.2);
    h->v[5] = (s32)c->corner_x[3];
    h->v[7] = (s32)(c->corner_z[3] * 1.2);
    h->v[8] = (s32)c->corner_x[1];
    h->v[10] = (s32)(c->corner_z[1] * 1.2);
    h->v[11] = (s32)c->corner_x[0];
    h->v[13] = (s32)(c->corner_z[0] * 1.2);
    {
        struct scene_desc *d = &g_shadow_objs[i];

        d->x = (s32)sx;
        d->y = (s32)sy;
        d->z = (s32)sz;
        d->ax = (s32)(c->f108 * 572.957795130822);
        d->ay = (s32)(c->heading * 572.957795130822);
        d->az = (s32)(c->f110 * 572.957795130822);
        if (r_54f930 == 0 && scene_obj_move(d) != 0) {
            log_printf("FEL VID LI_MOVEOBJECT HANDLE SHADOW\n");
        }
    }
    if (g_drivers[i].control == 2 && i != 0) {
        struct scene_desc *d = &g_arrow_objs[i];

        d->x = (s32)c->x;
        d->y = (s32)c->y;
        d->z = (s32)c->z;
        d->ax = d->ay = d->az = 0;
        if (d->in_grid == 1) {
            if (scene_obj_move(d) != 0) {
                log_printf("FEL VID LI_MOVEOBJECT HANDLE CAR NAME\n");
            }
        } else if (scene_obj_link(d) != 0) {
            log_printf("FEL VID LI_PLACEOBJECT HANDLE CAR NAME\n");
        }
    }
}
