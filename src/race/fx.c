/*
 * race/fx.c - the race's effects (0x434190, 0x434380): smoke, fire,
 * sparks, flying parts, lightning, splashes, and the sequences that
 * blow a car up, sink it or drop it off the track and put it back.
 *
 * An effect is a request (struct race_effect) copied into one of 200
 * slots; each slot has a scene object and a mesh (0x528800) the effect
 * rewrites every step. Positions and velocities are world units times
 * 1024. Points and sprites in those meshes are drawn by gl3d's deferred
 * pass, as the meshes change each frame; sprites (face type 7) go
 * through the level's .SHD table over what is under them.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gl3d.h"
#include "lisa/log.h"
#include "race/race.h"
#include "race/cars.h"
#include "race/contact.h"
#include "race/data.h"
#include "anim_tex.h"
#include "race/fx.h"
#include "race/hud.h"
#include "race/init.h"
#include "srf.h"
#include "structs/coll_obj.h"
#include "scene.h"
#include "../sound.h"
#include "stubs.h"
#include "video.h"

extern struct scene_desc *g_part_objs;          /* 0x54f994, one a slot */
extern s32 g_part_meshes[FX_SLOTS][200];        /* 0x528800 */
extern s32 r_5285d0;                            /* lightning is striking */
extern s32 r_527f24;                            /* the place marked in an elimination */

struct race_effect g_effects[FX_SLOTS];         /* 0x5db040 */
float r_563d64;                                 /* the animated textures' clock */

/* A frame table by its address in the original (0x497ef8..0x498448). */
static const s32 *fx_frames(s32 va)
{
    if (va < FX_FRAMES_BASE || va >= FX_FRAMES_BASE + 340 * 4) {
        return NULL;
    }
    return &g_fx_frames[(va - FX_FRAMES_BASE) / 4];
}

/* C's division truncates toward 0, as the original's cdq; and; sar. */
static s32 fx(s32 v)
{
    return v / 1024;
}

/* (a * b) / 1024 through a 32 bit imul. */
static s32 mul_fx(s32 a, s32 b)
{
    return (s32)(u32)((u32)a * (u32)b) / 1024;
}

static s32 mesh_count_ok(int slot)
{
    return slot >= 0 && slot < FX_SLOTS;
}

/* Puts the slot's object at the effect's place; links it the first
 * time. */
static void put(int slot, s32 x, s32 y, s32 z, s32 ax, s32 ay, s32 az, const char *what)
{
    struct scene_desc *d = &g_part_objs[slot];

    d->x = x;
    d->y = y;
    d->z = z;
    d->ax = ax;
    d->ay = ay;
    d->az = az;
    if (d->in_grid) {
        if (scene_obj_move(d) != 0) {
            log_printf("FEL VID LI_MOVEOBJECT %s\n", what);
        }
    } else {
        scene_obj_link(d);
    }
}

static void kill(struct race_effect *e, int slot)
{
    scene_obj_unlink(&g_part_objs[slot]);
    e->type = 0;
}

/* The velocity's drag and gravity, as types 2, 4, 5 and 8 work it out
 * (through the FPU). */
static void move_float(struct race_effect *e)
{
    e->x += e->f10;
    e->y += e->f14;
    e->z += e->f18;
    e->f10 = (s32)((double)e->f10 * (double)e->f1c * 0.0009765625);
    e->f14 += e->f20;
    e->f18 = (s32)((double)e->f1c * (double)e->f18 * 0.0009765625);
}

/* One sprite face, the mesh's only: vertex (0, 0, 0) or the given
 * offset. */
static void sprite_mesh(s32 *m, s32 vx, s32 vy, s32 vz)
{
    m[0] = 1;
    m[1] = 1;
    m[2] = vx;
    m[3] = vy;
    m[4] = vz;
    m[5] = 7;
    memset(m + 6, 0, 8 * sizeof(s32));
}

/* The effects' scale from 320 x 200, x and y (0x552d80, 0x552f5c),
 * times FX_SIZE / 10: the port draws the effects smaller than the
 * original, which on the Vita also means fewer of them overlapping. */
enum { FX_SIZE = 4 };

static s32 fx_scale_x(void)
{
    return r_552d80 / 10 * FX_SIZE;
}

static s32 fx_scale_y(void)
{
    return r_552f5c / 10 * FX_SIZE;
}

/* The sprite's frame and size: frame f28 / 1024 of the table, the size
 * f30 scaled by the screen (0x552d80, 0x552f5c). */
static void sprite_frame(s32 *m, const s32 *frames, s32 frame, s32 size)
{
    const s32 *f = frames != NULL ? frames + 4 * fx(frame) : NULL;

    if (f != NULL) {
        m[7] = f[0] << 8;
        m[8] = f[1] << 8;
        m[9] = f[2] << 8;
        m[10] = f[3] << 8;
    }
    m[12] = (size / 32) * (fx_scale_x() / 32);
    m[13] = (size / 32) * (fx_scale_y() / 32);
}

/* 0x42e2e0: type 1, a turning sprite (smoke). f24 the texture, f28 the
 * frame (+f2c a step), f30 the size (+f34), f3c..f44 the sprite's
 * offset, f48..f50 the object's angles, f54 the age, f58 the life. */
static void fx_sprite(struct race_effect *e, int slot)
{
    s32 *m = g_part_meshes[slot];
    s32 vx, vz, drag;

    if (e->f54 == 0) {
        sprite_mesh(m, e->f3c, e->f40, e->f44);
        scene_obj_set_mesh(&g_part_objs[slot], m, e->f24, 200, 0, 20, 0);
    }
    sprite_frame(m, e->f38.frames, e->f28, e->f30);
    put(slot, fx(e->x), fx(e->y), fx(e->z), e->f48, e->f4c, e->f50, "TRAN SPRITE");
    vx = e->f10;
    vz = e->f18;
    drag = e->f1c;
    e->x += vx;
    e->y += e->f14;
    e->z += vz;
    e->f10 = mul_fx(drag, vx);
    e->f14 = e->f20 + e->f14;
    e->f18 = mul_fx(vz, drag);
    e->f28 += e->f2c;
    e->f30 += e->f34;
    if (e->f58 <= ++e->f54) {
        kill(e, slot);
    }
}

/* 0x42e860: type 2, a spark: a point of colour f24 / 1024 (+f28 a
 * step), one pixel or two (f2c), f30 the age, f34 the life. */
static void fx_spark(struct race_effect *e, int slot)
{
    s32 *m = g_part_meshes[slot];
    s32 size = e->f2c;

    if (size < 0 || size > 1) {
        size = 0;
    }
    if (e->f30 == 0) {
        m[0] = 1;
        m[1] = 1;
        m[2] = m[3] = m[4] = 0;
        m[5] = size + 1;
        m[6] = 0;
        m[7] = fx(e->f24);
        scene_obj_set_mesh(&g_part_objs[slot], m, 0, 200, 0, 0, 0);
    }
    m[7] = fx(e->f24);
    put(slot, fx(e->x), fx(e->y), fx(e->z), 0, 0, 0, "HANDLE PLOT");
    move_float(e);
    e->f24 += e->f28;
    if (e->f34 <= ++e->f30) {
        kill(e, slot);
    }
}

/* 0x42e5a0: type 4, a sprite that does not turn (a fireball); as type 1
 * with f3c the age and f40 the life. */
static void fx_fireball(struct race_effect *e, int slot)
{
    s32 *m = g_part_meshes[slot];

    if (e->f3c == 0) {
        sprite_mesh(m, 0, 0, 0);
        scene_obj_set_mesh(&g_part_objs[slot], m, e->f24, 200, 0, 20, 0);
    }
    sprite_frame(m, e->f38.frames, e->f28, e->f30);
    put(slot, fx(e->x), fx(e->y), fx(e->z), 0, 0, 0, "TRAN SPRITE");
    move_float(e);
    e->f28 += e->f2c;
    e->f30 += e->f34;
    if (e->f40 <= ++e->f3c) {
        kill(e, slot);
    }
}

/* 0x42ea60: type 8, four points in a spinning square: f24 the colour,
 * f2c the points' size, f30..f38 the angles (+f3c..f44), f48 the age,
 * f4c the life. */
static void fx_star(struct race_effect *e, int slot)
{
    static const s32 verts[12] = { 10, 0, 10, -10, 0, 10, 10, 0, -10, -10, 0, -10 };
    s32 *m = g_part_meshes[slot];
    s32 size = e->f2c, colour;
    int k;

    if (size < 0 || size > 1) {
        size = 0;
    }
    if (e->f48 == 0) {
        m[0] = 4;
        m[1] = 4;
        memcpy(m + 2, verts, sizeof(verts));
        for (k = 0; k < 4; k++) {
            m[14 + 3 * k] = size + 1;
            m[15 + 3 * k] = k;
            m[16 + 3 * k] = fx(e->f24);
        }
        scene_obj_set_mesh(&g_part_objs[slot], m, 0, 200, 0, 0, 0);
    }
    colour = fx(e->f24);
    for (k = 0; k < 4; k++) {
        m[16 + 3 * k] = colour;
    }
    put(slot, fx(e->x), fx(e->y), fx(e->z), fx(e->f30), fx(e->f34), fx(e->f38.i), "SUPER PLOT");
    move_float(e);
    e->f30 += e->f3c;
    e->f34 += e->f40;
    e->f38.i += e->f44;
    e->f24 += e->f28;
    if (e->f4c <= ++e->f48) {
        kill(e, slot);
    }
}

/* A smoke or fire puff trailing behind flying parts. */
static void trail(const struct race_effect *e)
{
    if (e->f44 == 0) {
        if (race_rand() > 0.85) {
            g_effect.x = e->x;
            g_effect.y = e->y;
            g_effect.z = e->z;
            g_effect.type = 1;
            g_effect.f10 = 0;
            g_effect.f14 = (s32)(race_rand() + 1.0);
            g_effect.f18 = 0;
            g_effect.f20 = 0;
            g_effect.f24 = e->f40 + 4;
            g_effect.f28 = 0;
            g_effect.f1c = 0x400;
            g_effect.f2c = 0x199;
            g_effect.f3c = g_effect.f40 = g_effect.f44 = g_effect.f48 = 0;
            g_effect.f4c = g_effect.f50 = g_effect.f54 = 0;
            g_effect.f30 = 0xc800;
            g_effect.f34 = 0x800;
            g_effect.f38.frames = fx_frames(0x498118);
            g_effect.f58 = 0x13;
            race_effect_start(&g_effect);
        }
    } else if (e->f44 == 1) {
        if (race_rand() > 0.85) {
            g_effect.x = e->x;
            g_effect.y = e->y;
            g_effect.z = e->z;
            g_effect.f10 = 0;
            g_effect.type = 1;
            g_effect.f14 = (s32)((race_rand() + 1.0) * 1024.0);
            g_effect.f18 = 0;
            g_effect.f20 = 0;
            g_effect.f28 = 0;
            g_effect.f1c = 0x400;
            g_effect.f24 = 7;
            g_effect.f2c = 0x100;
            g_effect.f3c = g_effect.f40 = g_effect.f44 = g_effect.f48 = 0;
            g_effect.f4c = g_effect.f50 = 0;
            g_effect.f30 = 0xc800;
            g_effect.f54 = 0;
            g_effect.f34 = 0x800;
            g_effect.f38.frames = fx_frames(0x4982a8);
            g_effect.f58 = 0x1c;
            if (race_effect_start(&g_effect) != -1 && race_rand() * 24.0 < 2.0) {
                race_sound_3d(0, 2, 8, 0, 0x4000, 0, 0, (float)fx(e->x), (float)fx(e->z));
            }
        }
    }
}

/* An angle in 1/1024 tenths of a degree, back into 0..3600. */
static s32 wrap_tenths(s32 a)
{
    if (a > 0x384000) {
        a -= 0x384000;
    }
    if (a < 0) {
        a += 0x384000;
    }
    return a;
}

/* 0x42fc80: type 5, a flying part: a two sided triangle of colour f3c
 * tumbling (f24..f2c the angles, +f30..f38), trailing smoke (f44 0,
 * f40 + 4 its texture) or fire (f44 1); f48 the age, f4c the life. */
static void fx_part(struct race_effect *e, int slot)
{
    s32 *m = g_part_meshes[slot];

    if (e->f48 == 0) {
        m[0] = 3;
        m[1] = 2;
        m[2] = (s32)(race_rand() * 5.0 + 8.0);
        m[3] = 0;
        m[4] = 0;
        m[5] = (s32)(race_rand() * -5.0 - 4.0);
        m[6] = 0;
        m[7] = (s32)(race_rand() * -5.0 - 4.0);
        m[8] = (s32)(race_rand() * -5.0 - 4.0);
        m[9] = 0;
        m[10] = (s32)(race_rand() * 5.0 + 4.0);
        m[11] = 15;
        m[12] = 0;
        m[13] = 1;
        m[14] = 2;
        m[15] = e->f3c;
        m[16] = 15;
        m[17] = 0;
        m[18] = 2;
        m[19] = 1;
        m[20] = e->f3c;
        scene_obj_set_mesh(&g_part_objs[slot], m, 0, 200, 0, 0, 0);
    }
    trail(e);
    e->f24 = wrap_tenths(e->f24 + e->f30);
    e->f28 = wrap_tenths(e->f28 + e->f34);
    e->f2c = wrap_tenths(e->f2c + e->f38.i);
    put(slot, fx(e->x), fx(e->y), fx(e->z), fx(e->f24), fx(e->f28), fx(e->f2c), "FLYING PARTS");
    move_float(e);
    if (++e->f48 >= e->f4c) {
        kill(e, slot);
    }
}

/* 0x445f20
 * The palette (768 bytes) moved a fraction f toward (r, g, b), each
 * channel by (target - c) * f truncated, in a byte; entries whose mask
 * word is not 0 stay. Returns a buffer of its own (0x4cd8a8). */
const u8 *race_palette_tint(const u8 *pal, int r, int g, int b, double f, const s32 *mask)
{
    static u8 out[768];
    const int to[3] = { r, g, b };
    int i, k;

    memcpy(out, pal, sizeof(out));
    for (i = 0; i < 256; i++) {
        if (mask != NULL && mask[i] != 0) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            u8 c = out[3 * i + k];

            out[3 * i + k] = (u8)(c + (s32)((double)(to[k] - c) * f));
        }
    }
    return out;
}

/* 0x430300: type 6, lightning: the bolt's mesh at f04..f0c, at a
 * random turn each step, with sparks and smoke where it strikes and the
 * screen flashing; f14 the age, f18 the life. */
static void fx_lightning(struct race_effect *e, int slot)
{
    int k;

    if (e->f14 == 0) {
        scene_obj_set_mesh(&g_part_objs[slot], g_lightning_mesh, 0, 301, 0, 100, 0);
        for (k = 0; k < 8; k++) {
            g_effect.x = e->x;
            g_effect.y = e->y;
            g_effect.type = 2;
            g_effect.z = e->z;
            g_effect.f10 = (s32)((race_rand() * 4.0 - 4.0) * 1024.0);
            g_effect.f14 = (s32)((race_rand() * 3.0 + 3.0) * 1024.0);
            g_effect.f18 = (s32)((race_rand() * 4.0 - 4.0) * 1024.0);
            g_effect.f1c = 0x3e1;
            g_effect.f20 = -0x99;
            g_effect.f24 = (s32)((race_rand() * -5.0 + 15.0) * 1024.0);
            g_effect.f28 = -0x266;
            g_effect.f2c = 1;
            g_effect.f30 = 0;
            g_effect.f34 = 15;
            race_effect_start(&g_effect);
        }
        for (k = 0; k < 4; k++) {
            g_effect.x = e->x;
            g_effect.y = e->y;
            g_effect.z = e->z;
            g_effect.f10 = 0;
            g_effect.type = 1;
            g_effect.f14 = (s32)((race_rand() * 3.0 + 1.0) * 1024.0);
            g_effect.f18 = 0;
            g_effect.f20 = 0;
            g_effect.f28 = 0;
            g_effect.f3c = g_effect.f40 = g_effect.f44 = g_effect.f48 = 0;
            g_effect.f4c = g_effect.f50 = g_effect.f54 = 0;
            g_effect.f1c = 0x400;
            g_effect.f24 = 5;
            g_effect.f2c = 0x99;
            g_effect.f30 = 0x19000;
            g_effect.f34 = 0x1000;
            g_effect.f38.frames = fx_frames(0x498118);
            g_effect.f58 = 0x32;
            race_effect_start(&g_effect);
        }
        r_5285d0 = 1;
    }
    {
        s32 ax = (s32)(race_rand() * 50.0);
        s32 ay = (s32)(race_rand() * 3599.0);

        put(slot, fx(e->x), fx(e->y), fx(e->z), ax, ay, 0, "LIGHTENING");
    }
    if (g_views == 1) {
        const u8 *pal = g_level_col + 8;

        switch (e->f14) {
        case 0: video_set_palette(race_palette_tint(pal, 255, 255, 255, 0.4, NULL)); break;
        case 1: video_set_palette(race_palette_tint(pal, 255, 255, 255, 0.8, NULL)); break;
        case 2: video_set_palette(race_palette_tint(pal, 255, 255, 255, 0.4, NULL)); break;
        case 3: video_set_palette(race_palette_tint(pal, 255, 255, 255, 0.0, NULL)); break;
        case 4:
        case 5: video_set_palette(race_palette_tint(pal, 0x22, 0x22, 0x31, 0.5, NULL)); break;
        default: break;
        }
    }
    if (++e->f14 >= e->f18) {
        kill(e, slot);
        r_5285d0 = 0;
    }
}

/* 0x431d00: type 9, a splash: two faces through the .SHD between the
 * four corners f04..f30, at f3c..f44; f34 the frames (by the original's
 * address), f38 the texture, f48 the frame (+f4c), f50 the age, f54 the
 * life. */
static void fx_splash(struct race_effect *e, int slot)
{
    s32 *m = g_part_meshes[slot];
    const s32 *frames = fx_frames(e->f34);
    const s32 corners[12] = { e->x, e->y, e->z, e->f10, e->f14, e->f18,
                              e->f1c, e->f20, e->f24, e->f28, e->f2c, e->f30 };

    if (e->f50 == 0) {
        m[0] = 4;
        m[1] = 2;
        memcpy(m + 2, corners, sizeof(corners));
        m[14] = 0x13;
        m[15] = 0;
        m[16] = 1;
        m[17] = 2;
        m[24] = e->f38.i;
        m[25] = 0x13;
        m[26] = 0;
        m[27] = 2;
        m[28] = 3;
        m[35] = e->f38.i;
        scene_obj_set_mesh(&g_part_objs[slot], m, e->f38.i, 200, 0, 20, 0);
    }
    put(slot, fx(e->f3c), fx(e->f40), fx(e->f44), 0, 0, 0, "WATER SPLASH");
    e->f48 += e->f4c;
    if (frames != NULL) {
        const s32 *f = frames + 4 * fx(e->f48);

        m[18] = f[0] << 8;
        m[19] = f[3] << 8;
        m[20] = f[0] << 8;
        m[21] = f[1] << 8;
        m[22] = f[2] << 8;
        m[23] = f[1] << 8;
        m[29] = f[0] << 8;
        m[30] = f[3] << 8;
        m[31] = f[2] << 8;
        m[32] = f[1] << 8;
        m[33] = f[2] << 8;
        m[34] = f[3] << 8;
    }
    if (++e->f50 >= e->f54) {
        kill(e, slot);
    }
}

/* An angle in 1/1024 degrees back into 0..360 (unsigned divisions, as
 * the original). */
static s32 wrap_degrees(s32 a)
{
    if (a > 0x59c00) {
        a -= (s32)(((u32)a + 0x3ff) / 0x5a000 * 0x5a000);
    }
    return a;
}

static s32 unwrap_degrees(s32 a)
{
    if (a < 0) {
        a += (s32)((u32)(0x59fff - a) / 0x5a000 * 0x5a000);
    }
    return a;
}

/* 0x432bb0: type 11, the halo round a car put back on the track: a
 * sprite turning about car f04, closing in from 250 to 50; f08..f10 the
 * angles in 1/1024 degrees (+f14..f1c), f20 the age, f24 the life. */
static void fx_halo(struct race_effect *e, int slot)
{
    s32 *m = g_part_meshes[slot];
    const struct car *c = &g_cars[e->x];
    s32 a = e->y, b = e->z, d = e->f10;

    if (e->f20 == 0) {
        sprite_mesh(m, 0, 0, 0);
        m[7] = 0x2100;
        m[8] = 0x5400;
        m[9] = 0x4200;
        m[10] = 0x7500;
        m[12] = fx_scale_x() * 150;
        m[13] = fx_scale_y() * 150;
        scene_obj_set_mesh(&g_part_objs[slot], m, 2, 200, 0, 20, 0);
    }
    m[2] = 250 - e->f20 * 8;
    if (m[2] < 50) {
        m[2] = 50;
    }
    a = unwrap_degrees(wrap_degrees(a));
    b = unwrap_degrees(wrap_degrees(b));
    d = unwrap_degrees(wrap_degrees(d));
    put(slot, (s32)c->x, (s32)c->y, (s32)c->z, fx(a * 10), fx(b * 10), fx(d * 10), "TRAN SPRITE");
    a += e->f14;
    b += e->f18;
    d += e->f1c;
    if (++e->f20 >= e->f24) {
        kill(e, slot);
    }
    e->y = a;
    e->z = b;
    e->f10 = d;
}

/* The car's objects off the screen, or back. The body comes off
 * whatever 0x54f930 says after an explosion (`always`), else as the
 * shadow and car 7's part, only without it. */
static void car_objects(int i, int on, int always)
{
    struct scene_desc *d[7];
    int k, n = 0;

    d[n++] = (!on && always) || r_54f930 == 0 ? &g_car_objs[i] : NULL;
    for (k = 0; k < 4; k++) {
        d[n++] = &g_wheel_objs[4 * i + k];
    }
    d[n++] = r_54f930 == 0 ? &g_shadow_objs[i] : NULL;
    d[n++] = g_drivers[i].car == 7 && r_54f930 == 0 ? &g_car7_objs[i] : NULL;
    for (k = 0; k < n; k++) {
        if (d[k] == NULL) {
            continue;
        }
        if (on && d[k]->in_grid == 0) {
            scene_obj_link(d[k]);
        } else if (!on && d[k]->in_grid != 0) {
            scene_obj_unlink(d[k]);
        }
    }
}

static void stop_voice(s32 handle)
{
    struct sound_voice *v = sound_voice_of(handle);

    if (v != NULL) {
        v->rate = 0;
    }
}

/* A wreck's first step: where the car was, and its skids silenced. The
 * original clears the skid flags of the car being updated
 * (0x5285c0), not the wrecked one's. */
static void wreck_start(struct car *c)
{
    c->saved_x = c->x;
    c->saved_y = c->y;
    c->saved_z = c->z;
    c->saved_heading = c->heading;
    c->f270 = 1;
    if (c->f298 == 1) {
        stop_voice(c->skid_voice_rear);
        g_cars[r_5285c0].f298 = 0;
    }
    if (c->f29c == 1) {
        stop_voice(c->skid_voice_front);
        g_cars[r_5285c0].f29c = 0;
    }
}

/* Ten halos round car i as it comes back, and its sound. */
static void wreck_halos(int i)
{
    const struct car *c = &g_cars[i];
    int k;

    for (k = 0; k < 10; k++) {
        g_effect.type = 11;
        g_effect.x = i;
        g_effect.y = 0;
        g_effect.z = (s32)(race_rand() * 359.0 * 1024.0);
        g_effect.f10 = (s32)(race_rand() * 359.0 * 1024.0);
        g_effect.f14 = 0;
        g_effect.f18 = 0;
        g_effect.f1c = 0x2800;
        g_effect.f20 = 0;
        g_effect.f24 = 0x3c;
        race_effect_start(&g_effect);
    }
    race_sound_3d(0, 4, 3, 0, 0xe000, 0, 0, (float)(s32)c->x, (float)(s32)c->z);
}

/* The middle of the road two pieces on from car c: the mean of its road
 * faces' corners (material below 40), 250 up, and the road's heading
 * there less pi / 2. Returns 0 for a piece with no road, where the
 * original divides by 0. */
static int road_ahead(const struct car *c, s32 *cx, s32 *cy, s32 *cz, double *heading)
{
    s32 seg = c->f364, aseg = seg < 0 ? -seg : seg, plc;
    s32 sx = 0, sy = 0, sz = 0, n = 0;
    double h;
    const struct plc_entry *pe;
    const s32 *mesh;
    const u8 *f;
    int i;

    if (aseg + 2 >= g_track_link_count) {
        h = g_track_links[0].heading - 1.5707963267949;
        plc = g_track_links[0].plc;
    } else if (seg >= 0) {
        h = g_track_links[seg + 2].heading - 1.5707963267949;
        plc = g_track_links[seg + 2].plc;
    } else {
        const struct track_link *l = &g_track_links[aseg + 2];

        h = l->alt_heading - 1.5707963267949;
        if (h < 0.0) {
            h += 6.2831853071796;
        }
        plc = l->alt_plc;
        if (plc == 10000) {
            /* The original reads links[seg + 2] with the segment still
             * negative; before -2 that is outside the table. */
            int at = seg + 2 >= 0 ? seg + 2 : 0;

            if (seg + 2 < 0) {
                log_printf("fx: no piece before the fork at %d\n", seg);
            }
            h = g_track_links[at].heading - 1.5707963267949;
            if (h < 0.0) {
                h += 6.2831853071796;
            }
            plc = g_track_links[at].plc;
        }
        if (plc != -2) {
            goto found;
        }
        {
            int k = 0;

            do {
                k++;
                l++;
            } while (aseg + 2 + k < g_track_link_count && l->alt_plc == -2);
            h = g_track_links[aseg + k + 2].heading - 1.5707963267949;
            plc = g_track_links[aseg + k + 2].plc;
        }
    }
    if (h < 0.0) {
        h += 6.2831853071796;
    }
found:
    pe = plc_at(g_level_plc, plc);
    mesh = g_level_msh + pe->mesh;
    f = (const u8 *)(mesh + 2 + 3 * mesh[0]);
    for (i = 0; i < mesh[1]; i++, f += 44) {
        const s32 *w = (const s32 *)f;

        if (((u32)w[0] & 0xffff0000u) < 0x280000u) {
            const s32 *v0 = mesh + 2 + 3 * w[1], *v1 = mesh + 2 + 3 * w[2], *v2 = mesh + 2 + 3 * w[3];

            sx += v0[0] + pe->x + 0x6400 + v1[0] + pe->x + 0x6400 + v2[0] + pe->x + 0x6400;
            sy += -v0[1] + pe->y - v1[1] + pe->y - v2[1] + pe->y;
            sz += v0[2] + pe->z + 0x6400 + v1[2] + pe->z + 0x6400 + v2[2] + pe->z + 0x6400;
            n += 3;
        }
    }
    if (n == 0) {
        log_printf("fx: piece %d has no road\n", plc);
        return 0;
    }
    *cx = sx / n;
    *cy = sy / n + 250;
    *cz = sz / n;
    if (!(0.0 <= h)) {
        h += 6.2831853071796;
    }
    *heading = h;
    return 1;
}

/* 0x4358e0 */
static double ease(double t)
{
    int i = (s32)(t * 200.0);

    if (i < 0) {
        i = 0;
    }
    if (i > 200) {
        i = 200;
    }
    return g_ease[i] * 0.005;
}

/* The last 40 steps of a wreck: the car eases from where it was to the
 * road ahead (y and the heading in a line). */
static void ease_back(struct car *c, s32 age, s32 life)
{
    s32 cx, cy, cz;
    double h, h2, k, s;

    if (!road_ahead(c, &cx, &cy, &cz, &h)) {
        return;
    }
    k = (double)(age - life + 40);
    s = ease(k * 0.025);
    c->x = s * ((double)cx - c->saved_x) + c->saved_x;
    c->y = ((double)cy - c->saved_y) * k * 0.025 + c->saved_y;
    s = ease(k * 0.025);
    c->z = s * ((double)cz - c->saved_z) + c->saved_z;
    h2 = h - 6.2831853071796;
    if (fabs(h2 - c->saved_heading) < fabs(h - c->saved_heading)) {
        h = h2;
    }
    c->heading = (h - c->saved_heading) * k * 0.025 + c->saved_heading;
    if (c->heading > 3.1415926535898) {
        c->heading -= 6.2831853071796;
    }
    if (c->heading < -3.1415926535898) {
        c->heading += 6.2831853071796;
    }
}

/* Whether another car still racing is within 120 of car i. */
static int crowded(int i)
{
    const struct car *c = &g_cars[i];
    int k, near = 0;

    for (k = 0; k < g_driver_count; k++) {
        const struct car *o = &g_cars[k];

        if (k != i && fabs(o->x - c->x) < 120.0 && fabs(o->y - c->y) < 120.0
            && fabs(o->z - c->z) < 120.0 && o->f354 == 0 && o->f358 == 0 && o->f35c == 0) {
            near = 1;
        }
    }
    return near;
}

/* The end of a wreck: once no other car is near, the car is back on
 * the road (asking 0x435910 to finish it, f350) with its boost full;
 * a finished car not on autopilot waits. Returns the age. */
static s32 wreck_end(struct race_effect *e, int i, s32 age, s32 life, s32 *flag, int place)
{
    struct car *c = &g_cars[i];

    if (++age < life) {
        return age;
    }
    if (crowded(i) || (c->f528 != 0 && c->f604 != 1)) {
        return age - 1;
    }
    e->type = 0;
    c->f350 = 1;
    *flag = 0;
    c->f534 = 100.0;
    car_objects(i, 1, 0);
    if (place) {
        race_place_car(i);
    }
    return age;
}

/* 0x42ed60: type 3, a car blows up and comes back: f04..f0c where, f10
 * the car, f14 the age, f18 the life, f1c 1 for the halo. */
static void fx_explode(struct race_effect *e, int slot)
{
    int i = e->f10, k;
    struct car *c = &g_cars[i];
    s32 age = e->f14, life = e->f18;

    (void)slot;
    c->f534 = 100.0;
    c->f53c = 0;
    if (age == 0) {
        car_objects(i, 0, 1);
        wreck_start(c);
        g_effect.x = e->x;
        g_effect.y = e->y;
        g_effect.z = e->z;
        g_effect.f10 = g_effect.f14 = g_effect.f18 = 0;
        g_effect.f1c = 0x400;
        g_effect.f20 = 0;
        g_effect.f28 = 0;
        g_effect.type = 4;
        g_effect.f24 = 6;
        g_effect.f34 = 0;
        g_effect.f3c = 0;
        g_effect.f2c = 0x266;
        g_effect.f30 = 0x89800;
        g_effect.f38.frames = fx_frames(0x4981a8);
        g_effect.f40 = 0x1a;
        race_effect_start(&g_effect);
        for (k = 0; k < 15; k++) {
            g_effect.x = e->x;
            g_effect.y = e->y;
            g_effect.z = e->z;
            g_effect.type = 5;
            g_effect.f10 = (s32)((race_rand() * 12.0 - 6.0) * 1024.0);
            g_effect.f14 = (s32)((race_rand() * 5.0 + 5.0) * 1024.0);
            g_effect.f18 = (s32)((race_rand() * 12.0 - 6.0) * 1024.0);
            g_effect.f1c = 0x400;
            g_effect.f24 = 0;
            g_effect.f28 = 0;
            g_effect.f20 = -0x166;
            g_effect.f2c = 0;
            g_effect.f30 = (s32)((race_rand() * 100.0 - 50.0) * 1024.0);
            g_effect.f34 = (s32)((race_rand() * 100.0 - 50.0) * 1024.0);
            g_effect.f38.i = (s32)((race_rand() * 100.0 - 50.0) * 1024.0);
            g_effect.f3c = 0;
            g_effect.f48 = 0;
            g_effect.f40 = 1;
            g_effect.f44 = 1;
            g_effect.f4c = 0x3c;
            race_effect_start(&g_effect);
        }
        race_sound_3d(0, 2, 4, 0, 0xe000, 0x55f0, 0, (float)fx(e->x), (float)fx(e->z));
    }
    if ((age & 1) && age <= 20) {
        g_effect.type = 4;
        g_effect.x = (s32)((double)e->x + (race_rand() * 100.0 - 50.0) * 1024.0);
        g_effect.y = (s32)((double)e->y + race_rand() * 20.0 * 1024.0);
        g_effect.z = (s32)((double)e->z + (race_rand() * 100.0 - 50.0) * 1024.0);
        g_effect.f10 = g_effect.f14 = g_effect.f18 = 0;
        g_effect.f20 = 0;
        g_effect.f28 = 0;
        g_effect.f1c = 0x400;
        g_effect.f24 = 6;
        g_effect.f2c = 0x266;
        g_effect.f30 = (s32)((race_rand() * 300.0 + 250.0) * 1024.0);
        g_effect.f34 = 0;
        g_effect.f3c = 0;
        g_effect.f38.frames = fx_frames(0x4981a8);
        g_effect.f40 = 0x1a;
        race_effect_start(&g_effect);
    }
    {
        float r = race_rand();

        if (age * 0.0025 + 0.8 < r) {
            g_effect.type = 1;
            g_effect.x = (s32)(race_rand() * 40.0 + (double)e->x - 20.0);
            g_effect.y = e->y;
            g_effect.z = (s32)(race_rand() * 40.0 + (double)e->z - 20.0);
            g_effect.f10 = (s32)((race_rand() * 2.0 - 1.0) * 1024.0);
            g_effect.f14 = (s32)((race_rand() * 4.0 + 2.0) * 1024.0);
            g_effect.f18 = (s32)((race_rand() * 2.0 - 1.0) * 1024.0);
            g_effect.f1c = 0x400;
            g_effect.f24 = 5;
            g_effect.f2c = 0x66;
            g_effect.f30 = 0x4b000;
            g_effect.f34 = 0x800;
            g_effect.f38.frames = fx_frames(0x498118);
            g_effect.f20 = g_effect.f28 = 0;
            g_effect.f3c = g_effect.f40 = g_effect.f44 = g_effect.f48 = 0;
            g_effect.f4c = g_effect.f50 = g_effect.f54 = 0;
            g_effect.f58 = 0x4f;
            race_effect_start(&g_effect);
        }
    }
    if (age <= 20) {
        /* The camera shakes, less as it goes. */
        g_race_cams[i].f38 = race_rand() * (double)(20 - age) * 25.0;
    }
    if (life - age == 20 && e->f1c == 1) {
        wreck_halos(i);
    }
    if (age > life - 40) {
        ease_back(c, age, life);
    }
    e->f14 = wreck_end(e, i, age, life, &c->f354, 0);
}

/* The water's splashes round a sinking car: twelve quads (four
 * corners each) about it, through the .SHD. */
static const s32 s_sink_splashes[12][12] = {
    { -60, 0, -40, -60, -300, 0, 60, -300, 0, 60, 0, -40 },
    { 60, 0, -40, 60, -300, 0, -60, -300, 0, -60, 0, -40 },
    { -40, 0, 60, 0, -300, 60, 0, -300, -60, -40, 0, -60 },
    { -40, 0, -60, 0, -300, -60, 0, -300, 60, -40, 0, 60 },
    { -60, 0, 40, -60, -300, 0, 60, -300, 0, 60, 0, 40 },
    { 60, 0, 40, 60, -300, 0, -60, -300, 0, -60, 0, 40 },
    { 40, 0, 60, 0, -300, 60, 0, -300, -60, 40, 0, -60 },
    { 40, 0, -60, 0, -300, -60, 0, -300, 60, 40, 0, 60 },
    { -60, 10, 0, -60, 10, 60, 60, 10, 60, 60, 10, 0 },
    { 60, 10, 0, 60, 10, 60, -60, 10, 60, -60, 10, 0 },
    { -60, 10, -60, -60, 10, 0, 60, 10, 0, 60, 10, -60 },
    { 60, 10, -60, 60, 10, 0, -60, 10, 0, -60, 10, -60 },
};

/* 0x4307b0: type 7, a car in the water: splashes, then back on the
 * road; f04..f0c where, f10 the car, f14 the age, f18 the life. */
static void fx_sink(struct race_effect *e, int slot)
{
    int i = e->f10, k;
    struct car *c = &g_cars[i];
    s32 age = e->f14, life = e->f18;

    (void)slot;
    c->f534 = 100.0;
    c->f53c = 0;
    if (age == 0) {
        car_objects(i, 0, 0);
        wreck_start(c);
        for (k = 0; k < 12; k++) {
            const s32 *q = s_sink_splashes[k];

            g_effect.type = 9;
            g_effect.x = q[0];
            g_effect.y = q[1];
            g_effect.z = q[2];
            g_effect.f10 = q[3];
            g_effect.f14 = q[4];
            g_effect.f18 = q[5];
            g_effect.f1c = q[6];
            g_effect.f20 = q[7];
            g_effect.f24 = q[8];
            g_effect.f28 = q[9];
            g_effect.f2c = q[10];
            g_effect.f30 = q[11];
            g_effect.f34 = 0x497f48;
            g_effect.f38.i = 3;
            g_effect.f3c = (s32)(c->x * 1024.0);
            g_effect.f40 = (s32)(c->y * 1024.0);
            g_effect.f44 = (s32)(c->z * 1024.0);
            g_effect.f48 = 0;
            g_effect.f50 = 0;
            g_effect.f4c = 0x166;
            g_effect.f54 = 0x1f;
            race_effect_start(&g_effect);
        }
        race_sound_3d(0, 2, 5, 0, 0x10000, 0x55f0, 0, (float)fx(e->x), (float)fx(e->z));
    }
    if (life - age == 20) {
        wreck_halos(i);
    }
    if (age > life - 40) {
        ease_back(c, age, life);
    }
    e->f14 = wreck_end(e, i, age, life, &c->f358, 1);
}

/* 0x432040: type 10, a car off the track: it tumbles away for 30 steps
 * and goes, then is back on the road; f04..f0c where, f10 the car, f14
 * the age, f18 the life. */
static void fx_fall(struct race_effect *e, int slot)
{
    int i = e->f10;
    struct car *c = &g_cars[i];
    s32 age = e->f14, life = e->f18;

    (void)slot;
    c->f534 = 100.0;
    c->f53c = 0;
    if (age == 0) {
        wreck_start(c);
    }
    if (age <= 30) {
        double k = 30 - age > 10 ? 10 : 30 - age;

        /* The original moves where it started from too. */
        c->saved_x += k;
        c->saved_z += k;
        c->x += k;
        c->y += age < 10 ? 4.0 : -10.0;
        c->z += k;
        c->f108 += 0.2;
        c->f110 += 0.25;
        if (race_rand() == 1.0) {
            race_sound_3d(0, 2, 1, 0, 0x10000, 0x55f0, 0, (float)(s32)c->x, (float)(s32)c->z);
        }
        if (age == 30) {
            car_objects(i, 0, 0);
        }
    }
    if (life - age == 20) {
        wreck_halos(i);
    }
    if (age > life - 40) {
        ease_back(c, age, life);
    }
    e->f14 = wreck_end(e, i, age, life, &c->f35c, 1);
}

/* How loud a sound at (x, z) is from (lx, lz): full at the place,
 * nothing from 1500 away. */
static double nearness(s32 x, s32 z, double lx, double lz)
{
    s32 dx = (s32)fabs((double)x - lx), dz = (s32)fabs((double)z - lz);
    s32 d2 = (s32)((u32)dx * (u32)dx + (u32)dz * (u32)dz);
    double v = d2 != 0 ? (1500.0 - sqrt((double)d2)) * 0.0006666666666666666 : 1.0;

    return 0.0 <= v ? v : 0.0;
}

/* 0x42aa00
 * The track's own sounds (0x553078): each as loud as its placement is
 * near the car watched (or, over the finish, the finish camera), or the
 * second player's car if that is nearer; at three quarters, but the
 * first two on track 4 at full. Without 0x553084 only the sounds of
 * placements the .POS leaves out (-1) play. */
static void race_track_sounds(void)
{
    int n;

    for (n = 0; n < r_563cf0; n++) {
        const struct sound_src *src = &g_sound_srcs[n];
        struct sound_voice *voice = sound_voice_of(src->voice);
        const struct scene_desc *o = &g_world[src->plc];
        double vol;

        if (r_553084 == 0 && g_level_pos[src->plc] != -1) {
            continue;
        }
        if (!(r_54f980 < 120.0)
            && ((g_two_players == 0 && g_cars[0].f528 == 1)
                || (g_two_players == 1 && g_cars[0].f528 == 1 && g_cars[1].f528 == 1))
            && r_563d20 < 3) {
            const float *cam = &g_finish_cams[(g_track * 3 + r_563d20) * 5];

            vol = nearness(o->x, o->z, cam[0], cam[2]);
        } else {
            const struct car *c = &g_cars[g_view_car];

            vol = nearness(o->x, o->z, c->x, c->z);
        }
        if (g_two_players == 1) {
            double v1 = nearness(o->x, o->z, g_cars[1].x, g_cars[1].z);

            if (vol < v1) {
                vol = v1;
            }
        }
        if (voice != NULL) {
            voice->volume = (s32)(vol * 0.75 * 65536.0);
            if (n < 2 && g_track == 4) {
                voice->volume = (s32)(vol * 65536.0);
            }
        }
    }
}

/* The two bodies 0x4292c0 pushes apart (a car or a knocked object):
 * 0x34 bytes in the original. */
struct fx_body {
    double x, z, vx, vz, w;
    float mass, inertia, impulse;
};

/* The contact 0x4295e0 finds (0x563c0c, 0x601690) and the line across
 * it (0x54f90c). */
static float s_contact_x, s_contact_z, s_contact_normal;

/* 0x4461d0 */
static double sign_d(double x)
{
    if (0.0 < x) {
        return 1.0;
    }
    if (x == 0.0) {
        return 0.0;
    }
    return x < 0.0 ? -1.0 : -2.0;
}

/* 0x429a10: the sign of a float by its bits. */
static int sign_f(float f)
{
    u32 b;

    memcpy(&b, &f, sizeof(b));
    if (b > 0x80000000u) {
        return -1;
    }
    return (s32)b > 0 ? 1 : 0;
}

/* 0x4295e0
 * Two convex outlines (n and m points of x, z floats): where their
 * edges cross, at most 8 points, the mean is the contact and the line
 * through the two furthest apart its direction. Floats as the original
 * keeps them. Returns 1 if they cross. */
static int outlines_cross(const float *a, int n, const float *b, int m)
{
    float px[8], pz[8];
    int count = 0, i, j, far_i = 0, far_j = 0;
    float best = 0.0f;
    double sx = 0.0, sz = 0.0;

    for (i = 0; i < n; i++) {
        int i2 = i + 1 < n ? i + 1 : 0;
        float p1x = a[2 * i], p1z = a[2 * i + 1], p2x = a[2 * i2], p2z = a[2 * i2 + 1];
        float dax = p1x - p2x, daz = p1z - p2z;

        for (j = 0; j < m; j++) {
            int j2 = j + 1 < m ? j + 1 : 0;
            float q1x = b[2 * j], q1z = b[2 * j + 1], q2x = b[2 * j2], q2z = b[2 * j2 + 1];
            float e1z = p1z - q1z, e1x = p1x - q1x, e2x = p1x - q2x, e2z = p1z - q2z;
            float dbx, dbz, f2x, f2z, k1, k2, den, num_z_den;
            double d, x, z;

            if ((s32)sign_d((double)e1z * dax - (double)e1x * daz)
                == (s32)sign_d((double)e2z * dax - (double)e2x * daz)) {
                continue;
            }
            dbx = q1x - q2x;
            dbz = q1z - q2z;
            f2x = q1x - p2x;
            f2z = q1z - p2z;
            if ((s32)sign_d(((double)q1z - p1z) * dbx - ((double)q1x - p1x) * dbz)
                == (s32)sign_d((double)f2z * dbx - (double)f2x * dbz)) {
                continue;
            }
            k1 = dbz * dax;
            k2 = dbx * daz;
            d = (double)k1 - k2;
            if (d == 0.0) {
                continue;
            }
            den = (float)d;
            x = ((double)q1x * k1 + (double)e1z * dbx * dax - (double)p1x * k2) / den;
            d = (double)k2 - k1;
            if (d == 0.0) {
                continue;
            }
            num_z_den = (float)d;
            z = ((double)e1x * dbz * daz - (double)p1z * k1 + (double)q1z * k2) / num_z_den;
            if (count >= 8) {
                return 0;
            }
            px[count] = (float)x;
            pz[count] = (float)z;
            count++;
        }
    }
    if (count < 1) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        sx += px[i];
        sz += pz[i];
    }
    s_contact_x = (float)(sx / count);
    s_contact_z = (float)(sz / count);
    for (i = 0; i <= count - 2; i++) {
        for (j = i; j <= count - 1; j++) {
            float dd = (float)(((double)px[i] - px[j]) * ((double)px[i] - px[j])
                               + ((double)pz[i] - pz[j]) * ((double)pz[i] - pz[j]));

            if (dd > best) {
                best = dd;
                far_i = i;
                far_j = j;
            }
        }
    }
    s_contact_normal = (float)atan2((double)pz[far_i] - pz[far_j], (double)px[far_i] - px[far_j]);
    return 1;
}

/* 0x4292c0
 * Two bodies touching at the contact across its line: if their points
 * there close on each other, an impulse at 1.5 times (no bounce but
 * half) turns and pushes each. Both keep the closing speed's whole
 * part as their impulse. Returns 1 if they hit. */
static int bodies_knock(struct fx_body *a, struct fx_body *b)
{
    double n = s_contact_normal, cn = cos(n), sn = sin(n);
    double dza = s_contact_z - a->z, dxa = s_contact_x - a->x;
    double ang_a = atan2(dza, dxa), ra = sqrt(dxa * dxa + dza * dza);
    double dzb = s_contact_z - b->z, dxb = s_contact_x - b->x;
    double ang_b = atan2(dzb, dxb), rb = sqrt(dxb * dxb + dzb * dzb);
    double sa = sin(ang_a), ca = cos(ang_a), sb = sin(ang_b), cb = cos(ang_b);
    double vax, vaz, vbx, vbz, close, ka, kb, jb;
    float msum, ja;
    int side;

    side = sign_f((float)(dza * cn - sn * dxa));
    vax = a->vx - a->w * sa * ra;
    vaz = ca * a->w * ra + a->vz;
    vbx = b->vx - b->w * sb * rb;
    vbz = cb * b->w * rb + b->vz;
    if (side != sign_f((float)((vaz - vbz) * cn - (vax - vbx) * sn))) {
        return 0;
    }
    close = (vbx * sn - vbz * cn) - (vax * sn - vaz * cn);
    a->impulse = (float)abs((s32)close);
    b->impulse = a->impulse;
    ka = cos(n - ang_a) / a->inertia * a->mass * ra;
    kb = cos(n - ang_b) / b->inertia * b->mass * rb;
    msum = b->mass + a->mass;
    ja = (float)((double)b->mass / msum * close * -1.5 / ((sa * sn + ca * cn) * (ra * ka) + 1.0));
    jb = (double)a->mass / msum * close * -1.5 / ((sn * sb + cn * cb) * (rb * kb) + 1.0);
    a->w = ka * ja + a->w;
    a->vx = a->vx - sn * ja;
    a->vz = ja * cn + a->vz;
    b->w = b->w - kb * jb;
    b->vx = sn * jb + b->vx;
    b->vz = b->vz - cn * jb;
    return 1;
}

/* 0x42a8d0
 * A velocity off a plane of normal n: turned so the normal is the x
 * axis, its x turned back if it runs into the plane, and turned back. */
static void bounce(const double nrm[3], double v[3])
{
    double a = atan2(nrm[2], nrm[0]), c1 = cos(-a), s1 = sin(-a);
    double b = atan2(nrm[1], nrm[0] * c1 - nrm[2] * s1);
    double x1 = c1 * v[0] - s1 * v[2], z1 = c1 * v[2] + s1 * v[0];
    double c2 = cos(-b), s2 = sin(-b);
    double x2 = x1 * c2 - s2 * v[1], y2 = c2 * v[1] + x1 * s2;
    double x3, y3;

    if (0.0 < x2) {
        return;
    }
    x3 = cos(b) * -x2 - y2 * sin(b);
    y3 = sin(b) * -x2 + y2 * cos(b);
    v[0] = x3 * cos(a) - sin(a) * z1;
    v[1] = y3;
    v[2] = x3 * sin(a) + z1 * cos(a);
}

/* 0x429a40
 * The trackside objects cars knock over (cones, barrels): one at rest
 * that a car's outline crosses (its own a square of 60) takes an
 * impulse and flies, tumbling, falling 0.4 a step, bouncing off the
 * ground at 0.9 along and 0.6 up until it is slower than 2. Kind 4
 * flashes now and then. */
static void race_knocked_objects(void)
{
    int k, i;

    for (k = 0; k < r_54f954; k++) {
        struct coll_obj *o = &g_coll_objs[k];

        if (o->f08 == 0) {
            for (i = 0; i < g_driver_count; i++) {
                struct car *c = &g_cars[i];
                struct car_phys *p = &g_car_phys[i];
                float car_quad[8], obj_quad[8];
                struct fx_body cb, ob;
                double cs, sn;
                int q;

                if (!(fabs(c->y - o->y) < 50.0) || c->f354 || c->f358 || c->f35c
                    || !(fabs(c->x - o->x) < 150.0) || !(fabs(c->z - o->z) < 150.0)) {
                    continue;
                }
                cs = cos(c->heading);
                sn = sin(c->heading);
                for (q = 0; q < 4; q++) {
                    car_quad[2 * q] = (float)(cs * c->corner_x[q] - sn * c->corner_z[q] + c->x);
                    car_quad[2 * q + 1] = (float)(sn * c->corner_x[q] + cs * c->corner_z[q] + c->z);
                }
                obj_quad[0] = (float)(o->x - 30.0);
                obj_quad[1] = (float)(o->z - 30.0);
                obj_quad[2] = (float)(o->x + 30.0);
                obj_quad[3] = (float)(o->z - 30.0);
                obj_quad[4] = (float)(o->x + 30.0);
                obj_quad[5] = (float)(o->z + 30.0);
                obj_quad[6] = (float)(o->x - 30.0);
                obj_quad[7] = (float)(o->z + 30.0);
                if (!outlines_cross(car_quad, 4, obj_quad, 4)) {
                    continue;
                }
                cb.x = c->x;
                cb.z = c->z;
                cb.vx = c->center_dx;
                cb.vz = c->center_dz;
                cb.w = p->yaw_rate;
                cb.mass = c->f634;
                cb.inertia = c->f638;
                cb.impulse = c->knock;
                ob.x = o->x;
                ob.z = o->z;
                ob.vx = o->f24[9];
                ob.vz = o->f24[10];
                ob.w = o->f24[11];
                ob.mass = o->f84;
                ob.inertia = o->f88;
                ob.impulse = o->impulse;
                if (!bodies_knock(&cb, &ob)) {
                    continue;
                }
                c->x = cb.x;
                c->z = cb.z;
                p->yaw_rate = cb.w;
                c->f634 = cb.mass;
                c->f638 = cb.inertia;
                c->knock = cb.impulse;
                o->x = ob.x;
                o->z = ob.z;
                o->f24[9] = ob.vx;
                o->f24[10] = ob.vz;
                o->f24[11] = ob.w;
                o->f84 = ob.mass;
                o->f88 = ob.inertia;
                o->impulse = ob.impulse;
                o->f24[0] = ob.vx;
                o->f24[1] = race_rand() * 2.0 + 2.0;
                o->f24[2] = ob.vz;
                o->f08 = 1;
                o->f24[6] = o->impulse * 5.0f;
                o->f24[7] = o->impulse * 5.0f;
                o->f24[8] = o->impulse * 5.0f;
                if (cb.impulse > 10.0f) {
                    s32 vol = (s32)((fabs(cb.impulse) - 10.0) * 3000.0);

                    race_sound_3d(0, 2, 1, 0, vol > 0x10000 ? 0x10000 : vol, 0x55f0, 0,
                                  (float)(s32)c->x, (float)(s32)c->z);
                }
            }
        } else {
            const struct srf_hit *h;
            double *a = &o->f24[3];

            o->f24[0] *= 0.99;
            o->f24[1] -= 0.4;
            o->f24[2] *= 0.99;
            for (i = 0; i < 3; i++) {
                a[i] += o->f24[6 + i];
                if (!(0.0 <= a[i])) {
                    a[i] += 3600.0;
                }
                if (a[i] > 3599.0) {
                    a[i] -= 3600.0;
                }
            }
            for (i = 0; i < 3; i++) {
                o->f24[6 + i] *= 0.98;
            }
            o->x += o->f24[0];
            o->y += o->f24[1];
            o->z += o->f24[2];
            if (!(0.0 <= o->x)) {
                o->x = 0.0;
            }
            if (o->x > 51200.0) {
                o->x = 51200.0;
            }
            if (o->y < -10000.0) {
                o->y = -10000.0;
            }
            if (o->y > 10000.0) {
                o->y = 10000.0;
            }
            if (!(0.0 <= o->z)) {
                o->z = 0.0;
            }
            if (o->z > 51200.0) {
                o->z = 51200.0;
            }
            h = srf_lookup((s32)o->x, (s32)o->y + 150, (s32)o->z);
            if (h->type != -1) {
                const s32 *n = h->normal, *v0 = h->v[0], *v1 = h->v[1], *v2 = h->v[2];
                double ground = -((double)v0[1]
                                  + ((double)n[0] * ((double)(v1[0] - v0[0]) - (o->x - v0[0]))
                                     + (double)n[2] * ((double)(v1[2] - v0[2]) - (o->z - v0[2]))
                                     + (double)n[1] * (v1[1] - v0[1]))
                                        / n[1])
                                + 15.0;

                if (ground > o->y) {
                    double speed = sqrt(o->f24[0] * o->f24[0] + o->f24[1] * o->f24[1]
                                        + o->f24[2] * o->f24[2]);

                    if (speed > 2.0) {
                        double a1 = v0[0] - v1[0], b1 = v0[1] - v1[1], c1 = v0[2] - v1[2];
                        double d1 = v2[0] - v1[0], e1 = v2[1] - v1[1], f1 = v2[2] - v1[2];
                        double nrm[3] = { b1 * f1 - c1 * e1, a1 * f1 - c1 * d1, a1 * e1 - b1 * d1 };
                        double vel[3] = { o->f24[0], o->f24[1], o->f24[2] };

                        bounce(nrm, vel);
                        o->f24[0] = vel[0] * 0.9;
                        o->f24[1] = vel[1] * 0.6;
                        o->f24[2] = vel[2] * 0.9;
                        if (o->f24[1] > 20.0) {
                            o->f24[1] = 20.0;
                        }
                        o->y = ground;
                        o->f24[6] = o->f24[7] = o->f24[8] = speed * 5.0;
                    } else {
                        o->f24[0] = o->f24[1] = o->f24[2] = 0.0;
                        o->f24[6] = o->f24[7] = o->f24[8] = 0.0;
                        o->f08 = 0;
                    }
                }
            }
            {
                struct scene_desc *d = &g_world[o->plc];

                d->x = (s32)o->x;
                d->y = (s32)o->y;
                d->z = (s32)o->z;
                d->ax = (s32)a[0];
                d->ay = (s32)a[1];
                d->az = (s32)a[2];
                if (scene_obj_move(d) != 0) {
                    log_printf("FEL VID LI_MOVEOBJECT COLLISION OBJECT\n");
                }
            }
        }
        {
            s32 t = (s32)o->f90;

            if (o->type == 4) {
                s32 period = o->f94 / 5;

                if (period > t) {
                    s32 row = t == 0 ? 0 : 32;

                    if (period - t == 1) {
                        row = 64;
                    }
                    g_effect.type = 1;
                    g_effect.x = (s32)(o->x * 1024.0);
                    g_effect.y = (s32)((o->y + 20.0) * 1024.0);
                    g_effect.z = (s32)(o->z * 1024.0);
                    g_effect.f10 = g_effect.f14 = g_effect.f18 = 0;
                    g_effect.f1c = 0x400;
                    g_effect.f20 = 0;
                    g_effect.f24 = 3;
                    g_effect.f28 = 0;
                    g_effect.f2c = 0;
                    g_effect.f30 = 0x64000;
                    g_effect.f34 = 0;
                    g_effect.f38.frames = fx_frames(0x497ef8 + row);
                    g_effect.f3c = g_effect.f40 = g_effect.f44 = g_effect.f48 = 0;
                    g_effect.f4c = g_effect.f50 = g_effect.f54 = 0;
                    g_effect.f58 = 2;
                    race_effect_start(&g_effect);
                }
                o->f90 += 1.0f;
                if ((float)o->f94 <= o->f90) {
                    o->f90 = 0.0f;
                }
            }
        }
    }
}

/* A puff from an emitter: smoke, steam or fire (type 1, no drag but
 * f1c, frames by the original's address), the rest of the request 0. */
static void emit(s32 x, s32 y, s32 z, s32 vx, s32 vy, s32 vz, s32 drag, s32 gravity, s32 texture,
                 s32 frame, s32 frame_step, s32 size, s32 grow, s32 frames, s32 life)
{
    g_effect.type = 1;
    g_effect.x = x;
    g_effect.y = y;
    g_effect.z = z;
    g_effect.f10 = vx;
    g_effect.f14 = vy;
    g_effect.f18 = vz;
    g_effect.f1c = drag;
    g_effect.f20 = gravity;
    g_effect.f24 = texture;
    g_effect.f28 = frame;
    g_effect.f2c = frame_step;
    g_effect.f30 = size;
    g_effect.f34 = grow;
    g_effect.f38.frames = fx_frames(frames);
    g_effect.f3c = g_effect.f40 = g_effect.f44 = g_effect.f48 = 0;
    g_effect.f4c = g_effect.f50 = g_effect.f54 = 0;
    g_effect.f58 = life;
    race_effect_start(&g_effect);
}

/* 0x42acb0
 * The track's smoking placements (0x563c68), each puffing after a
 * random while by its kind: 0 and 1 chimneys (with 0x563be0), 6 and
 * -46 more of them (and 0x553084), 3 steam (not on placements the .POS
 * keeps in a time trial), 8 and 9 wide plumes. */
static void race_emitters_a(void)
{
    int n;

    for (n = 0; n < r_563d78; n++) {
        struct flare *f = &g_flares[n];
        const struct scene_desc *o = &g_world[f->plc];
        s32 kind = f->kind;

        if ((kind == 0 || kind == 1) && r_563be0 == 1) {
            /* The original also tests kinds 0x165 and -46 here, which
             * cannot reach it. */
            f->f08++;
            if ((double)f->f08 > race_rand() * 3.0 + 2.0) {
                s32 vx = (s32)((race_rand() - 0.5) * 1024.0);
                s32 vy = (s32)((race_rand() * 2.0 + 2.0) * 1024.0);
                s32 vz = (s32)((race_rand() - 0.5) * 1024.0);
                s32 size = (s32)((race_rand() * 100.0 + 100.0) * 1024.0);
                s32 grow = (s32)((race_rand() * 2.0 + 2.0) * 1024.0);

                emit(o->x << 10, o->y << 10, o->z << 10, vx, vy, vz, 0x400, 0, kind == 1 ? 5 : 4, 0,
                     0xa3, size, grow, 0x497ff8, 50);
                f->f08 = 0;
            }
        } else if ((kind == 6 || kind == -46) && r_563be0 == 1 && r_553084 == 1) {
            f->f08++;
            if ((double)f->f08 > race_rand() * 3.0 + 2.0) {
                s32 vx = (s32)((race_rand() - 0.5) * 1024.0);
                s32 vy = (s32)((race_rand() * 2.0 + 1.0) * 1024.0);
                s32 vz = (s32)((race_rand() - 0.5) * 1024.0);
                s32 size = (s32)((race_rand() * 100.0 + 100.0) * 1024.0);
                s32 grow = (s32)((race_rand() * 2.0 + 2.0) * 1024.0);

                emit(o->x << 10, o->y << 10, o->z << 10, vx, vy, vz, 0x400, 0, 4, 0, 0xa3, size, grow,
                     0x498088, 50);
                f->f08 = 0;
            }
        } else if (kind == 2) {
            /* Nothing. */
        } else if (kind == 3) {
            if (g_race_mode != 2 || g_level_pos[f->plc] == -1) {
                f->f08++;
                if ((double)f->f08 > race_rand() * 4.0 + 3.0) {
                    s32 vx = (s32)(race_rand() * 2.0 * 1024.0);
                    s32 vy = (s32)((race_rand() + 3.0) * 1024.0);
                    s32 vz = (s32)((race_rand() * 2.0 - 9.0) * 1024.0);
                    s32 size = (s32)((race_rand() * 100.0 + 100.0) * 1024.0);
                    s32 grow = (s32)((race_rand() * 2.0 + 2.0) * 1024.0);

                    emit(o->x << 10, o->y << 10, o->z << 10, vx, vy, vz, 0x3f5, -0x66, 4, 0xc00, 0x33,
                         size, grow, 0x498088, 100);
                    f->f08 = 0;
                }
            }
        } else if (kind == 8) {
            f->f08++;
            if ((double)f->f08 > race_rand() * 10.0 + 10.0) {
                s32 x = (s32)((race_rand() * 40.0 + (double)o->x) * 1024.0);
                s32 y = (s32)((race_rand() * 10.0 + (double)o->y) * 1024.0);
                s32 z = (s32)((race_rand() * 40.0 + (double)o->z) * 1024.0);
                s32 vy = (s32)((race_rand() + 2.0) * 1024.0);
                s32 size = (s32)((race_rand() * 150.0 + 150.0) * 1024.0);
                s32 grow = (s32)((race_rand() + 2.0) * 1024.0);

                emit(x, y, z, 0, vy, 0, 0x400, 0, 4, 0, 0x35, size, grow, 0x498088, 0x96);
                f->f08 = 0;
            }
        } else if (kind == 9) {
            f->f08++;
            if ((double)f->f08 > race_rand() * 15.0 + 15.0) {
                s32 x = (s32)((race_rand() * 150.0 + (double)o->x - 75.0) * 1024.0);
                s32 y = (s32)((race_rand() * 10.0 + (double)o->y) * 1024.0);
                s32 z = (s32)((race_rand() * 150.0 + (double)o->z - 75.0) * 1024.0);
                s32 vy = (s32)((race_rand() + 1.0) * 1024.0);
                s32 size = (s32)((race_rand() * 150.0 + 150.0) * 1024.0);
                s32 grow = (s32)((race_rand() + 2.0) * 1024.0);

                emit(x, y, z, 0, vy, 0, 0x400, 0, 5, 0, 0x28, size, grow, 0x497ff8, 0x96);
                f->f08 = 0;
            }
        }
    }
}

/* 0x42b5d0
 * The moving placements' smoke (0x525e6c): kind 302 smokes a little at
 * random and now and then throws a part; kind 305 every 40 steps
 * blows eight puffs of steam over 16. */
static void race_emitters_b(void)
{
    int n;

    for (n = 0; n < r_552f78; n++) {
        struct mover *mv = &g_movers[n];

        if (mv->type == 302) {
            const struct scene_desc *o = &g_world[mv->plc];

            mv->phase++;
            if (!((double)mv->phase <= race_rand() + 1.0)) {
                s32 x = (s32)(race_rand() * 150.0 + (double)o->x - 75.0);
                s32 z = (s32)(race_rand() * 150.0 + (double)o->z - 75.0);
                s32 vy = (s32)((race_rand() * 0.5 + 0.5) * 1024.0);
                s32 size = (s32)((race_rand() * 150.0 + 150.0) * 1024.0);
                s32 grow = (s32)((race_rand() * 3.0 + 3.0) * 1024.0);

                emit(x << 10, o->y << 10, z << 10, 0, vy, 0, 0x400, 0, 4, 0, 0x10f, size, grow,
                     0x497ff8, 0x1e);
                mv->phase = 0;
            }
            if ((s32)(race_rand() * 10.0) == 1) {
                s32 x = (s32)(race_rand() * 100.0 + (double)o->x - 50.0);
                s32 y = (s32)(race_rand() * 100.0 + (double)(o->y + 200));
                s32 z = (s32)(race_rand() * 100.0 + (double)o->z - 50.0);

                g_effect.x = x << 10;
                g_effect.y = y << 10;
                g_effect.z = z << 10;
                g_effect.type = 5;
                g_effect.f10 = (s32)((race_rand() * 20.0 - 10.0) * 1024.0);
                g_effect.f14 = (s32)((race_rand() * 5.0 + 5.0) * 1024.0);
                g_effect.f18 = (s32)((race_rand() * 20.0 - 10.0) * 1024.0);
                g_effect.f1c = 0x400;
                g_effect.f20 = -0x1cc;
                g_effect.f24 = 0;
                g_effect.f28 = 0;
                g_effect.f2c = 0;
                g_effect.f30 = (s32)((race_rand() * 100.0 - 50.0) * 1024.0);
                g_effect.f34 = (s32)((race_rand() * 100.0 - 50.0) * 1024.0);
                g_effect.f38.i = (s32)((race_rand() * 100.0 - 50.0) * 1024.0);
                g_effect.f3c = (s32)(race_rand() * 7.0 + 4.0);
                g_effect.f40 = 0;
                g_effect.f44 = 0;
                g_effect.f48 = 0;
                g_effect.f4c = 0x3c;
                race_effect_start(&g_effect);
            }
        }
        if (mv->type == 305) {
            mv->phase++;
            if (mv->phase > 40 && mv->f0c == 0) {
                mv->f0c = 1;
            }
            if (mv->f0c > 0) {
                const struct scene_desc *o = &g_world[mv->plc];
                s32 x = (s32)(race_rand() * 50.0 + (double)o->x - 25.0);
                s32 y = o->y;
                s32 z = (s32)(race_rand() * 50.0 + (double)o->z - 25.0);

                if (!(mv->f0c & 1)) {
                    s32 vx = (s32)((race_rand() * 2.0 - 1.0) * 1024.0);
                    s32 vy, vz, size, grow;

                    /* The original draws a number and throws it away. */
                    race_rand();
                    vy = (s32)((0.0 + 10.0) * 1024.0);
                    vz = (s32)((race_rand() * 2.0 - 1.0) * 1024.0);
                    size = (s32)((race_rand() * 150.0 + 100.0) * 1024.0);
                    grow = (s32)((race_rand() * 3.0 + 3.0) * 1024.0);
                    emit(x << 10, y << 10, z << 10, vx, vy, vz, 0x400, 0, 4, 0, 0x18f, size, grow,
                         0x498088, 0x14);
                }
                if (++mv->f0c > 15) {
                    mv->f0c = 0;
                    mv->phase = 0;
                }
            }
        }
    }
}

/* One puff of the marked car's trail, at its back. */
static void trail_puff(const struct car *c, s32 texture, s32 frame_step, s32 size, s32 grow,
                       s32 frames, s32 spread, s32 life)
{
    g_effect.type = 1;
    g_effect.x = (s32)(c->x * 1024.0);
    g_effect.y = (s32)(c->y * 1024.0);
    g_effect.z = (s32)(c->z * 1024.0);
    g_effect.f10 = (s32)(c->dx * 512.0 + (race_rand() * 1024.0 - 512.0));
    g_effect.f14 = 0x400;
    g_effect.f18 = (s32)((race_rand() * 1024.0 - 512.0) + c->dz * 512.0);
    g_effect.f20 = 0;
    g_effect.f1c = 0x400;
    g_effect.f24 = texture;
    g_effect.f2c = frame_step;
    g_effect.f30 = size;
    g_effect.f34 = grow;
    g_effect.f38.frames = fx_frames(frames);
    g_effect.f28 = 0;
    g_effect.f3c = (s32)(race_rand() * 10.0 + (double)c->min_x);
    g_effect.f40 = 0;
    g_effect.f44 = (s32)(race_rand() * (double)(2 * spread) - (double)spread);
    g_effect.f48 = (s32)(c->f108 * 572.957795130822);
    g_effect.f4c = (s32)(c->heading * 572.957795130822);
    g_effect.f50 = (s32)(c->f110 * 572.957795130822);
    g_effect.f58 = life;
    g_effect.f54 = 0;
    race_effect_start(&g_effect);
}

/* 0x444890: in an elimination race, fire and smoke behind car i, the
 * one marked to go. */
static void race_marked_trail(int i)
{
    const struct car *c = &g_cars[i];
    int k;

    for (k = 0; k < 2; k++) {
        trail_puff(c, 7, 0x200, 0x3e800, -0x1800, 0x4982a8, 15, 14);
    }
    if ((s32)(race_rand() * 2.0) == 0) {
        trail_puff(c, 5, 0x133, 0x25800, 0x1000, 0x498118, 25, 0x19);
    }
}

/* 0x434380
 * Takes a slot for the request: a free one, or the one of the least
 * priority if it is less than the request's. Smoke, sparks and stars
 * (1, 2, 8) are only started near the cars watched, until the players
 * have finished. Returns the slot, or -1. */
int race_effect_start(struct race_effect *e)
{
    int ok = 1, i, min_slot = -1;

    if (e->type == 1 || e->type == 2 || e->type == 8) {
        s32 x = fx(e->x), z = fx(e->z);
        const struct car *v = &g_cars[g_view_car];

        ok = 0;
        if ((abs(x - (s32)v->x) < 3500 && abs(z - (s32)v->z) < 3500)
            || !(r_54f980 < 120.0) || r_5532b0 == 1) {
            ok = 1;
        }
        if (g_two_players == 1) {
            const struct car *w = &g_cars[1];

            if ((abs(x - (s32)w->x) < 2500 && abs(z - (s32)w->z) < 2500) || !(r_54f980 < 120.0)) {
                ok = 1;
            }
        }
    }
    if (!ok) {
        return -1;
    }
    for (i = 0; i < FX_SLOTS && g_effects[i].type != 0; i++) {
    }
    if (i >= FX_SLOTS) {
        s32 want = g_fx_priority[e->type], least = 100;
        int k;

        for (k = 0; k < FX_SLOTS; k++) {
            s32 p = g_fx_priority[g_effects[k].type];

            if (p < least) {
                least = p;
                min_slot = k;
            }
        }
        /* With none below 100 the original takes a slot from stale
         * stack; here there is none. */
        if (want <= least || min_slot < 0) {
            return -1;
        }
        i = min_slot;
    }
    g_effects[i] = *e;
    return i;
}

void race_effect(struct race_effect *e)
{
    race_effect_start(e);
}

/* 0x401f00: the animated textures' next frame (anim_tex.c). */
static void anim_textures(void)
{
    anim_tex_step();
}

/* 0x434190
 * Each step: every effect, the animated textures on the track's clock
 * (0x498480 a frame by track), the knocked objects, the track's sounds
 * and emitters, in an elimination race the trail behind the marked car,
 * and the messages' timers. */
void race_effects_tick(void)
{
    int i;

    gl3d_set_dynamic(&g_part_meshes[0][0], &g_part_meshes[FX_SLOTS - 1][200]);
    gl3d_set_particles(&g_part_meshes[0][0], &g_part_meshes[FX_SLOTS - 1][200]);
    gl3d_set_tables(g_level_shd, g_level_pan);
    for (i = 0; i < FX_SLOTS; i++) {
        struct race_effect *e = &g_effects[i];

        if (!mesh_count_ok(i)) {
            break;
        }
        switch (e->type) {
        case 1: fx_sprite(e, i); break;
        case 2: fx_spark(e, i); break;
        case 3: fx_explode(e, i); break;
        case 4: fx_fireball(e, i); break;
        case 5: fx_part(e, i); break;
        case 6: fx_lightning(e, i); break;
        case 7: fx_sink(e, i); break;
        case 8: fx_star(e, i); break;
        case 9: fx_splash(e, i); break;
        case 10: fx_fall(e, i); break;
        case 11: fx_halo(e, i); break;
        default: break;
        }
    }
    r_563d64 += 1.0f;
    if ((float)g_anim_ticks[g_track] <= r_563d64) {
        float t;

        anim_textures();
        t = (float)g_anim_ticks[g_track];
        if (t <= r_563d64) {
            do {
                r_563d64 -= t;
            } while (!(r_563d64 < t));
        }
    }
    if (r_54f954 > 0) {
        race_knocked_objects();
    }
    if (r_563cf0 > 0) {
        race_track_sounds();
    }
    if (r_563d78 > 0) {
        race_emitters_a();
    }
    if (r_552f78 > 0) {
        race_emitters_b();
    }
    if (g_race_mode == 3) {
        for (i = 0; i < g_driver_count; i++) {
            const struct car *c = &g_cars[i];

            if (c->grid == r_527f24 && !(0.0 <= r_552e48) && c->f354 == 0 && c->f358 == 0
                && c->f35c == 0 && c->f528 == 0) {
                race_marked_trail(i);
            }
        }
    }
    race_age_messages();
}

/* 0x432f20
 * Sparks where car 0x5285c0 scrapes a wall: a corner on a wall (0x5a)
 * at over 100 km/h on the ground starts six steps of them; the last
 * such corner, a random way along its side, gives where. Two a step
 * for a player's car, one for the computer's. */
void race_scrape_sparks(void)
{
    struct car *c = &g_cars[r_5285c0];
    double px = 0.0, pz = 0.0, cs, sn;
    int k, n;

    if ((c->wall_surface[0] == 0x5a || c->wall_surface[1] == 0x5a || c->wall_surface[2] == 0x5a
         || c->wall_surface[3] == 0x5a)
        && !(c->f118 <= 100.0) && c->f270 == 0) {
        c->f33c = 6;
        for (k = 0; k < 4; k++) {
            c->scrape_corner[k] = c->wall_surface[k];
        }
    }
    if (c->f33c > 0) {
        s32 r = (s32)(race_rand() * 20.0);

        cs = cos(c->heading);
        sn = sin(c->heading);
        /* Front corners a way back from the nose, rear ones forward. */
        if (c->scrape_corner[0] == 0x5a) {
            double a = (double)r + c->corner_x[0];

            px = cs * a - c->corner_z[0] * sn + c->x;
            pz = cs * c->corner_z[0] + a * sn + c->z;
        }
        if (c->scrape_corner[1] == 0x5a) {
            double a = (double)r + c->corner_x[1];

            px = a * cs - c->corner_z[1] * sn + c->x;
            pz = cs * c->corner_z[1] + sn * a + c->z;
        }
        if (c->scrape_corner[2] == 0x5a) {
            double a = c->corner_x[2] - (double)r;

            px = cs * a - c->corner_z[2] * sn + c->x;
            pz = cs * c->corner_z[2] + a * sn + c->z;
        }
        if (c->scrape_corner[3] == 0x5a) {
            double a = c->corner_x[3] - (double)r;

            px = a * cs - c->corner_z[3] * sn + c->x;
            pz = cs * c->corner_z[3] + sn * a + c->z;
        }
        n = g_drivers[r_5285c0].control == 0 ? 2 : 1;
        for (k = 0; k < n; k++) {
            double r1 = race_rand() * 2.0 - 1.0, r2 = race_rand() * 2.0 - 1.0;
            double vx = cos(c->heading) * c->f118 * 0.1 + r1;
            double vz = sin(c->heading) * c->f118 * 0.1 + r2;

            g_effect.type = 2;
            g_effect.x = (s32)(px * 1024.0);
            g_effect.y = (s32)(c->y * 1024.0);
            g_effect.z = (s32)(pz * 1024.0);
            g_effect.f10 = (s32)(vx * 1024.0);
            g_effect.f14 = 0x4cc;
            g_effect.f18 = (s32)(vz * 1024.0);
            g_effect.f1c = 0x3e1;
            g_effect.f20 = -0x99;
            g_effect.f24 = (s32)((race_rand() * -5.0 + 15.0) * 1024.0);
            g_effect.f28 = -0x266;
            g_effect.f2c = 1;
            g_effect.f30 = 0;
            g_effect.f34 = 15;
            race_effect_start(&g_effect);
        }
    }
    if (c->f33c > 0) {
        c->f33c--;
    }
}

extern s32 g_weather;                           /* 0x553774: 1 rain, 2 snow, 3 */
extern s32 *g_weather_meshes[2];                /* 0x563c34, 0x563c30 */
extern struct scene_desc *g_weather_objs;       /* 0x553030, 9 a view */
extern s32 g_weather_voices[2];                 /* 0x525e50 */
extern s32 r_563cf4;                            /* the tick the weather last changed */

enum { WEATHER_MESH_DWORDS = 0x9c40 / 4 };

/* The weather's faces anew for n drops: rain two lines a drop (through
 * the .SHD, from colour 0x77 to 0x7b; the table is 1 here for the .SHD,
 * where the original keeps its address), snow a flake sprite a
 * vertex. */
static void weather_faces(s32 *m, int drops)
{
    int k, faces = 2 * drops;

    if (g_weather == 1) {
        s32 *f = m + 2 + 12 * drops;

        for (k = 0; k < faces; k++, f += 6) {
            f[0] = 13;
            f[1] = 2 * k;
            f[2] = 2 * k + 1;
            f[3] = 0x7700;
            f[4] = 0x7b00;
            f[5] = 1;
        }
    } else if (g_weather == 2) {
        s32 *f = m + 2 + 6 * drops;

        for (k = 0; k < faces; k++, f += 9) {
            f[0] = 7;
            f[1] = k;
            f[2] = 0;
            f[3] = 0x7500;
            f[4] = 0x1f00;
            f[5] = 0x9400;
            f[6] = 0;
            f[7] = fx_scale_x() * 100;
            f[8] = fx_scale_y() * 100;
        }
    }
}

static s32 *weather_zone(int v)
{
    return v == 0 ? &r_552f30 : &r_552f34;
}

/* 0x434840
 * The weather each view sees: drops (rain) or flakes (snow) in a mesh
 * of the view's, two more every other tick while the view's car is in
 * the weather's stretch (0x434580's flag) before the finish, two fewer
 * out of it, the sky darkening with them in rain. The mesh's nine
 * objects tile 3 by 3 thousands round the car, falling and drifting;
 * rain sometimes strikes lightning on the road ahead. Its sound
 * follows the drops, and fades in and out with the stretch. */
void race_weather(void)
{
    static double *const sway[2] = { &r_525e30, &r_525e38 };
    s32 *const drift_x[2] = { &r_563d28, &r_563d2c };
    s32 *const drift_y[2] = { &r_563d38, &r_563d3c };
    int v, j, k;

    if (g_weather == 1 || g_weather == 2) {
        gl3d_set_dynamic(g_weather_meshes[0], g_weather_meshes[0] + WEATHER_MESH_DWORDS);
        gl3d_set_dynamic(g_weather_meshes[1], g_weather_meshes[1] + WEATHER_MESH_DWORDS);
        gl3d_set_particles(g_weather_meshes[0], g_weather_meshes[0] + WEATHER_MESH_DWORDS);
        gl3d_set_particles(g_weather_meshes[1], g_weather_meshes[1] + WEATHER_MESH_DWORDS);
    }
    if (g_cars[0].f528 == 1 && !(r_54f980 < 120.0)) {
        if ((g_weather == 1 || g_weather == 2)
            && (g_weather_meshes[0][1] > 0 || g_weather_meshes[1][1] > 0)
            && (g_two_players == 0 || (g_two_players == 1 && g_cars[1].f528 == 1))) {
            /* The players are done: the sky clears. */
            video_set_palette(g_level_col + 8);
            g_weather_meshes[0][1] = 0;
            g_weather_meshes[1][1] = 0;
        }
    } else if (g_weather == 1 || g_weather == 2) {
        for (v = 0; v < g_views; v++) {
            int vc = v == 0 ? g_view_car : 1;
            s32 *m = g_weather_meshes[v];
            const struct car *c = &g_cars[vc];

            if ((double)(r_563cf4 + 2) < g_race_clock) {
                if (*weather_zone(v) == 1 && r_54f980 < 0.0 && (double)m[1] < 100.0) {
                    int i = m[1] / 2;
                    s32 r1, r2, r3;

                    if (g_views == 1 && g_weather == 1) {
                        video_set_palette(race_palette_tint(g_level_col + 8, 0x22, 0x22, 0x31,
                                                            i * 0.02 * 0.5, NULL));
                    }
                    r1 = (s32)(race_rand() * 1000.0);
                    r2 = (s32)(race_rand() * 800.0);
                    r3 = (s32)(race_rand() * 1000.0);
                    if (g_weather == 1) {
                        s32 *p = m + 2 + 12 * i;

                        m[0] += 4;
                        m[1] += 2;
                        p[0] = r1;
                        p[1] = -r2;
                        p[2] = r3;
                        p[3] = r1 + 4;
                        p[4] = 60 - r2;
                        p[5] = r3;
                        p[6] = r1;
                        p[7] = -800 - r2;
                        p[8] = r3;
                        p[9] = r1 + 4;
                        p[10] = -740 - r2;
                        p[11] = r3;
                    } else {
                        s32 *p = m + 2 + 6 * i;

                        m[0] += 2;
                        m[1] += 2;
                        p[0] = r1;
                        p[1] = -r2;
                        p[2] = r3;
                        p[3] = r1;
                        p[4] = -800 - r2;
                        p[5] = r3;
                    }
                    weather_faces(m, i + 1);
                }
                if ((*weather_zone(v) == 0 || 0.0 <= r_54f980) && m[1] > 0) {
                    int i;

                    m[0] -= g_weather == 1 ? 4 : 2;
                    m[1] -= 2;
                    i = m[1] / 2;
                    if (g_views == 1 && g_weather == 1) {
                        video_set_palette(race_palette_tint(g_level_col + 8, 0x22, 0x22, 0x31,
                                                            i * 0.02 * 0.5, NULL));
                    }
                    weather_faces(m, i);
                }
            }
            if (g_weather == 1 && m[0] > 0) {
                *drift_x[v] += 5;
                *drift_y[v] -= 25;
                if ((s32)(race_rand() * 100.0) == 0 && r_54f980 < 0.0) {
                    race_sound_3d(1, 0, 1, 0, 0x10000, 0, 0, (float)(s32)c->rear_x,
                                  (float)(s32)c->rear_z);
                }
            } else if (g_weather == 2 && m[0] > 0) {
                s32 dx, dz, n;
                s32 *p;

                *sway[v] += 0.11781;
                if (*sway[v] > 6.2831853071796) {
                    *sway[v] -= 6.2831853071796;
                }
                *drift_x[v] = (s32)(sin(*sway[v]) * 20.0);
                *drift_y[v] -= 5;
                /* Each flake's top trails the bottom against the car's
                 * travel; as the original, the last writes one flake
                 * past the end, into the faces. */
                dx = (s32)-(cos(c->f100) * (c->f118 * 0.3333333333333333 + 3.0));
                dz = (s32)-((c->f118 * 0.3333333333333333 + 3.0) * sin(c->f100));
                p = m + 2;
                for (n = m[1] / 2; n > 0; n--, p += 6) {
                    if (p + 12 > m + WEATHER_MESH_DWORDS) {
                        break;
                    }
                    p[3] = p[0] + dx;
                    p[5] = p[2] + dz;
                    p[9] = p[0] + dx;
                    p[11] = p[2] + dz;
                }
            }
            if (*drift_y[v] < -800) {
                *drift_y[v] += 800;
                *drift_x[v] = 0;
            }
            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    struct scene_desc *d = &g_weather_objs[9 * v + 3 * j + k];
                    s32 y = (s32)(c->y - 200.0);

                    d->x = *drift_x[v] + ((s32)(c->x * 0.001) + k - 1) * 1000;
                    d->y = *drift_y[v] + y;
                    d->z = ((s32)(c->z * 0.001) + j - 1) * 1000;
                    d->ax = d->ay = d->az = 0;
                    if (scene_obj_move(d) != 0) {
                        log_printf("FEL VID LI_MOVEOBJECT WEATHER\n");
                    }
                }
            }
            {
                struct sound_voice *voice = sound_voice_of(g_weather_voices[v]);

                if (voice != NULL) {
                    voice->volume = (s32)((double)m[0] * 0.01 * 65536.0);
                }
            }
            if (g_weather == 1 && *weather_zone(v) == 1 && r_553084 == 1
                && (s32)(race_rand() * 200.0) == 0) {
                s32 ahead = (s32)(race_rand() * 4.0);
                s32 seg = (c->f364 < 0 ? -c->f364 : c->f364) + ahead;
                s32 plc = g_track_link_count > seg ? g_track_links[seg].plc : g_track_links[ahead].plc;
                const struct plc_entry *pe = plc_at(g_level_plc, plc);
                s32 x0 = pe->x + 0x6400, z0 = pe->z + 0x6400;

                g_effect.type = 6;
                g_effect.x = (s32)((race_rand() * 200.0 + (double)x0 - 100.0) * 1024.0);
                g_effect.y = -pe->y << 10;
                g_effect.z = (s32)((race_rand() * 200.0 + (double)z0 - 100.0) * 1024.0);
                g_effect.f10 = vc;
                g_effect.f14 = 0;
                g_effect.f18 = 6;
                race_effect_start(&g_effect);
                race_sound_3d(1, 0, 2, 0, 0x10000, 0, 0, (float)(s32)c->rear_x,
                              (float)(s32)c->rear_z);
            }
        }
        if ((double)(r_563cf4 + 2) < g_race_clock) {
            r_563cf4 = (s32)g_race_clock;
        }
    }
    if (g_weather == 1 || g_weather == 3) {
        for (v = 0; v < g_views; v++) {
            struct sound_voice *voice = sound_voice_of(g_weather_voices[v]);

            if (voice == NULL) {
                continue;
            }
            if (*weather_zone(v) == 1 && voice->volume < 30000) {
                voice->volume += 2500;
            } else if (*weather_zone(v) == 0 && voice->volume > 0) {
                voice->volume -= 2500;
                if (voice->volume < 0) {
                    voice->volume = 0;
                }
            }
        }
    }
}

/* The surface under the car's rear wheels, for the dust: the first of
 * them that is a road (0..39), else 0. */
static s32 rear_surface(const struct car *c)
{
    if (c->surface[2] >= 0 && c->surface[2] <= 39) {
        return c->surface[2];
    }
    if (c->surface[3] >= 0 && c->surface[3] <= 39) {
        return c->surface[3];
    }
    return 0;
}

/* 0x433f90: dust behind car 0x5285c0 on dirt (4, 14, 24) above 20 km/h,
 * more the faster it goes. */
void race_dust_trail(void)
{
    const struct car *c = &g_cars[r_5285c0];
    s32 surf = rear_surface(c);
    double q;

    if ((surf != 4 && surf != 14 && surf != 24) || c->f118 <= 20.0 || c->f270 != 0) {
        return;
    }
    q = c->f118 * c->f118 * 0.001;
    if (!(race_rand() * 70.0 < q)) {
        return;
    }
    {
        s32 vy = (s32)((race_rand() * 0.5 - 1.0) * 1024.0);
        s32 size = (s32)((race_rand() * 150.0 + 150.0) * 1024.0);
        s32 grow = (s32)((race_rand() * 2.0 + q * 0.125) * 1024.0);

        emit((s32)(c->rear_x * 1024.0), (s32)(c->y * 1024.0), (s32)(c->rear_z * 1024.0), 0, vy, 0,
             0x3f5, 0, 4, 0x800, 0x3d, size, grow, 0x497ff8, 100);
    }
}

/* 0x4338d0: as car 0x5285c0 touches down on a dusty surface, a puff at
 * each side of each axle. */
void race_landing_dust(void)
{
    const struct car *c = &g_cars[r_5285c0];
    double side;
    int k;

    if (c->landing != 1 || g_dusty[rear_surface(c)] != 1) {
        return;
    }
    side = (double)g_wheel_info[4 * g_drivers[r_5285c0].car].dz;
    for (k = 0; k < 4; k++) {
        double a = c->heading + 1.5707963267949;
        double ax = k < 2 ? c->front_x : c->rear_x, az = k < 2 ? c->front_z : c->rear_z;
        double sg = (k & 1) ? -1.0 : 1.0;
        s32 x, z, vx, vz, size, grow;

        /* The original works each out in full; the same maths. */
        if (sg > 0) {
            x = (s32)((cos(a) * side + ax) * 1024.0);
            z = (s32)((side * sin(a) + az) * 1024.0);
        } else {
            x = (s32)((ax - cos(a) * side) * 1024.0);
            z = (s32)((az - side * sin(a)) * 1024.0);
        }
        vx = (s32)((race_rand() * 2.0 - 1.0) * 1024.0);
        vz = (s32)((race_rand() * 2.0 - 1.0) * 1024.0);
        size = (s32)((race_rand() * 50.0 + 50.0) * 1024.0);
        grow = (s32)((race_rand() * 2.0 + 2.0) * 1024.0);
        emit(x, (s32)(c->y * 1024.0), z, vx, -1024, vz, 0x400, 0, 4, 0, 0xcc, size, grow, 0x497ff8,
             0x28);
    }
}

s32 r_4949b0;                                   /* every surface sprays (a debug switch) */

/* 0x4333e0
 * Water or mud (surfaces 6, 16, 26) thrown up by car 0x5285c0's wheels
 * on the ground above 5 km/h: two drops a wheel, with a share of the
 * car's move. The corners run front left, front right, rear left, rear
 * right; their wheels the other way round. */
void race_spray(void)
{
    struct car *c = &g_cars[r_5285c0];
    int model = g_drivers[r_5285c0].car, k, n;
    double ox = 0.0, oz = 0.0;

    if (c->f118 <= 5.0 || c->f270 != 0) {
        return;
    }
    for (k = 0; k < 4; k++) {
        s32 surf = c->surface[k];

        c->spraying = 0;
        if (surf == 6 || surf == 16 || surf == 26 || r_4949b0 == 1) {
            double cs = cos(c->heading), sn = sin(c->heading);
            double wx = g_wheel_dx[4 * model + 3 - k], wz = g_wheel_dz[4 * model + 3 - k];

            ox = cs * wx - sn * wz;
            oz = wz * cs + sn * wx;
            c->spraying = 1;
        }
        if (c->spraying != 1) {
            continue;
        }
        {
            double vx0 = c->dx * 0.2857142857142857, vz0 = c->dz * 0.2857142857142857;
            s32 vy = (s32)((c->f118 * 0.06666666666666667 + 1.0) * 1024.0);

            for (n = 0; n < 2; n++) {
                double r1 = race_rand() * 4.0 - 2.0, r2 = race_rand();

                g_effect.type = 8;
                g_effect.x = (s32)((c->x + ox) * 1024.0);
                g_effect.y = (s32)(c->y * 1024.0);
                g_effect.z = (s32)((c->z + oz) * 1024.0);
                g_effect.f10 = (s32)((r1 + vx0) * 1024.0);
                g_effect.f14 = vy;
                g_effect.f18 = (s32)((r2 * 4.0 - 2.0 + vz0) * 1024.0);
                g_effect.f30 = 0;
                g_effect.f34 = 0;
                g_effect.f38.i = 0;
                g_effect.f1c = 0x3d7;
                g_effect.f20 = -0x99;
                g_effect.f24 = 0x1ec00;
                g_effect.f28 = -0x20;
                g_effect.f2c = 1;
                g_effect.f3c = (s32)(race_rand() * 100.0);
                g_effect.f40 = (s32)(race_rand() * 100.0);
                g_effect.f44 = (s32)(race_rand() * 100.0);
                g_effect.f48 = 0;
                g_effect.f4c = 0x3c;
                race_effect_start(&g_effect);
            }
        }
    }
}

/* A puff out of car 0x5285c0's exhaust at its back, the angles its
 * body's times `turn` (the boost's start has 1024 where the flames
 * have tenths of a degree). */
static void exhaust_puff(const struct car *c, double turn)
{
    s32 vx = (s32)((race_rand() * 4.0 - 2.0) * 1024.0);
    s32 vy = (s32)(race_rand() * 1024.0);
    s32 vz = (s32)((race_rand() * 4.0 - 2.0) * 1024.0);

    g_effect.type = 1;
    g_effect.x = (s32)(c->x * 1024.0);
    g_effect.y = (s32)(c->y * 1024.0);
    g_effect.z = (s32)(c->z * 1024.0);
    g_effect.f10 = vx;
    g_effect.f14 = vy;
    g_effect.f18 = vz;
    g_effect.f20 = 0;
    g_effect.f1c = 0x400;
    g_effect.f24 = 4;
    g_effect.f2c = 0x99;
    g_effect.f30 = 0x32000;
    g_effect.f34 = 0x800;
    g_effect.f38.frames = fx_frames(0x498088);
    g_effect.f28 = 0;
    g_effect.f3c = (s32)(race_rand() * 20.0 + (double)c->min_x);
    g_effect.f40 = 0;
    g_effect.f44 = (s32)(race_rand() * 60.0 - 30.0);
    g_effect.f48 = (s32)(c->f108 * turn);
    g_effect.f4c = (s32)(c->heading * turn);
    g_effect.f50 = (s32)(c->f110 * turn);
    g_effect.f58 = 0x32;
    g_effect.f54 = 0;
    race_effect_start(&g_effect);
}

/* 0x443320's puffs: ten out of the exhaust as the boost starts. */
void race_boost_puffs(void)
{
    const struct car *c = &g_cars[r_5285c0];
    int k;

    for (k = 0; k < 10; k++) {
        exhaust_puff(c, 1024.0);
    }
}

/* One of car 8's smoke puffs, as likely as its boost is full. */
static void boost_smoke(const struct car *c, s32 spread)
{
    if (!(race_rand() * 100.0 < c->f534)) {
        return;
    }
    {
        s32 vx = (s32)(race_rand() * 1024.0 - 512.0 + c->dx * 921.6);
        s32 vz = (s32)(race_rand() * 1024.0 - 512.0 + c->dz * 921.6);

        g_effect.type = 1;
        g_effect.x = (s32)(c->x * 1024.0);
        g_effect.y = (s32)(c->y * 1024.0);
        g_effect.z = (s32)(c->z * 1024.0);
        g_effect.f10 = vx;
        g_effect.f14 = 0x400;
        g_effect.f18 = vz;
        g_effect.f1c = 0x400;
        g_effect.f20 = 0;
        g_effect.f28 = 0x1000;
        g_effect.f34 = 0x1000;
        g_effect.f24 = 5;
        g_effect.f2c = 0x3d;
        g_effect.f30 = 0x19000;
        g_effect.f38.frames = fx_frames(0x498118);
        g_effect.f3c = -25;
        g_effect.f40 = -50;
        g_effect.f44 = spread;
        g_effect.f48 = (s32)(c->f108 * 572.957795130822);
        g_effect.f4c = (s32)(c->heading * 572.957795130822);
        g_effect.f50 = (s32)(c->f110 * 572.957795130822);
        g_effect.f58 = 0x32;
        g_effect.f54 = 0;
        race_effect_start(&g_effect);
    }
}

/* 0x443640
 * The boost burning on car 0x5285c0: two flames at its back each step;
 * in a standing start, ten puffs whenever the count's bits 1 and 3 are
 * set; car 3 a jet that flickers between two runs of frames; car 8
 * smoke as thick as the boost is full. */
void race_boost_flames(void)
{
    struct car *c = &g_cars[r_5285c0];
    int k;

    for (k = 0; k < 2; k++) {
        s32 vx = (s32)(c->dx * 512.0 + (race_rand() * 1024.0 - 512.0));
        s32 vz = (s32)((race_rand() * 1024.0 - 512.0) + c->dz * 512.0);

        g_effect.type = 1;
        g_effect.x = (s32)(c->x * 1024.0);
        g_effect.y = (s32)(c->y * 1024.0);
        g_effect.z = (s32)(c->z * 1024.0);
        g_effect.f10 = vx;
        g_effect.f14 = 0x400;
        g_effect.f18 = vz;
        g_effect.f1c = 0x400;
        g_effect.f20 = 0;
        g_effect.f28 = 0;
        g_effect.f2c = 0;
        g_effect.f34 = 0x400;
        g_effect.f24 = 2;
        g_effect.f30 = 0x25800;
        g_effect.f38.frames = fx_frames(0x498438);
        g_effect.f3c = (s32)(race_rand() * 10.0 + (double)c->min_x);
        g_effect.f40 = 0;
        g_effect.f44 = (s32)(race_rand() * 30.0 - 15.0);
        g_effect.f48 = (s32)(c->f108 * 572.957795130822);
        g_effect.f4c = (s32)(c->heading * 572.957795130822);
        g_effect.f50 = (s32)(c->f110 * 572.957795130822);
        g_effect.f58 = 0x1a;
        g_effect.f54 = 0;
        race_effect_start(&g_effect);
    }
    c->f54c++;
    if (c->f53c == 2 && (c->f54c & 0xa) == 0xa) {
        for (k = 0; k < 10; k++) {
            exhaust_puff(c, 572.957795130822);
        }
    }
    if (g_drivers[r_5285c0].car == 3) {
        s32 frame = (s32)(fmod((double)c->f54c, 8.0) * 0.5);
        int lit = (c->f54c & 8) != 0;

        g_effect.type = 1;
        g_effect.x = (s32)(c->x * 1024.0);
        g_effect.y = (s32)(c->y * 1024.0);
        g_effect.z = (s32)(c->z * 1024.0);
        g_effect.f10 = g_effect.f14 = g_effect.f18 = 0;
        g_effect.f20 = 0;
        g_effect.f24 = 2;
        g_effect.f28 = 0;
        g_effect.f2c = 0;
        g_effect.f34 = 0;
        g_effect.f38.frames = fx_frames((lit ? 0x4983e8 : 0x4983a8) + 16 * frame);
        g_effect.f1c = 0x400;
        g_effect.f30 = 0x4b000;
        g_effect.f40 = -25;
        g_effect.f44 = lit ? 12 : -12;
        g_effect.f3c = 0;
        g_effect.f48 = (s32)(c->f108 * 572.957795130822);
        g_effect.f4c = (s32)(c->heading * 572.957795130822);
        g_effect.f50 = (s32)(c->f110 * 572.957795130822);
        g_effect.f54 = 0;
        g_effect.f58 = 2;
        race_effect_start(&g_effect);
    }
    if (g_drivers[r_5285c0].car == 8) {
        boost_smoke(c, 22);
        boost_smoke(c, -22);
        boost_smoke(c, 22);
        boost_smoke(c, -22);
    }
}

extern struct scene_desc *g_particle_objs;      /* 0x563cec, 700 skid marks */
extern s32 r_5532f0;                            /* the next of them */

enum { SKID_MARKS = 700, SKID_MESH_DWORDS = 49 };

static s32 s_skid_meshes[SKID_MARKS][SKID_MESH_DWORDS];    /* 0x5dfe70 */

/* An axle's skid state: its marks' last edge, where the last mark
 * ended, its flag and its sound. */
struct skid_axle {
    double ax, az, slip;
    s32 *mark_x, *mark_y, *mark_z, *last_x, *last_z, *flag;
    s32 s0, s1;                 /* the surfaces of its two wheels */
};

/* Smoke off a skidding axle: a puff each side, of the wheel's width
 * from the middle. */
static void skid_smoke(const struct car *c, const struct skid_axle *x, double side)
{
    int k;

    for (k = 0; k < 2; k++) {
        double a = c->heading + (k == 0 ? 1.5707963267949 : -1.5707963267949);
        double ox = cos(a) * side, oz = side * sin(a);
        double s = race_rand() * 0.4 + 0.2;

        g_effect.type = 1;
        g_effect.x = (s32)((x->ax + ox) * 1024.0);
        g_effect.y = (s32)(c->y * 1024.0);
        g_effect.z = (s32)((x->az + oz) * 1024.0);
        g_effect.f10 = (s32)(race_rand() * 2048.0);
        g_effect.f14 = 0x400;
        g_effect.f18 = (s32)(race_rand() * 2048.0);
        g_effect.f20 = 0;
        g_effect.f1c = 0x200;
        g_effect.f24 = 4;
        g_effect.f28 = 0;
        g_effect.f2c = (s32)(s * 1024.0);
        g_effect.f30 = (s32)((race_rand() * 100.0 + 50.0) * 1024.0);
        g_effect.f34 = (s32)((race_rand() * 4.0 + 4.0) * 1024.0);
        g_effect.f3c = g_effect.f40 = g_effect.f44 = g_effect.f48 = 0;
        g_effect.f4c = g_effect.f54 = g_effect.f50 = 0;
        g_effect.f38.frames = fx_frames(0x497ff8);
        g_effect.f58 = (s32)(8.0 / s);
        race_effect_start(&g_effect);
    }
}

/* The four ends of a new edge across the axle: the wheels' marks, each
 * the wheel's width across the way the axle went (angle e), about each
 * wheel. */
static void skid_edge(s32 out_x[4], s32 out_z[4], double e, double width, s32 ox, s32 oz, s32 mox,
                      s32 moz, s32 base_x, s32 base_z)
{
    double ce = cos(e), se = sin(e);

    out_x[0] = (s32)(width * ce + ox) + base_x;
    out_z[0] = (s32)(width * se + oz) + base_z;
    out_x[1] = (s32)(ox - width * ce) + base_x;
    out_z[1] = (s32)(oz - width * se) + base_z;
    out_x[2] = (s32)(width * ce + mox) + base_x;
    out_z[2] = (s32)(width * se + moz) + base_z;
    out_x[3] = (s32)(mox - width * ce) + base_x;
    out_z[3] = (s32)(moz - width * se) + base_z;
}

/* A skid mark from the axle's last edge to here: two quads (a wheel
 * each) of flat faces through the .SHD in the surface's colour, on the
 * next of the 700 marks. Then this is the last edge. */
static void skid_mark(struct car *c, struct skid_axle *x, s32 colour, int model)
{
    double cs, sn, w = (double)g_wheel_info[4 * model].dz, width = (double)g_wheel_info[4 * model].top;
    double dx = x->ax - (double)*x->last_x, dz = x->az - (double)*x->last_z, e;
    s32 ox, oz, mox, moz, y0, rx = (s32)x->ax, rz = (s32)x->az, nx[4], nz[4];
    s32 *m;
    struct scene_desc *d;
    int k;

    if (sqrt(dx * dx + dz * dz) <= 30.0 || r_552e58 != 1) {
        return;
    }
    cs = cos(c->heading + 1.5707963267949);
    sn = sin(c->heading + 1.5707963267949);
    ox = (s32)(cs * w);
    oz = (s32)(sn * w);
    mox = (s32)-(cs * w);
    moz = (s32)-(sn * w);
    e = atan2(dz, dx) + 1.5707963267949;
    y0 = (s32)(sin(c->f110) * 20.0 + c->y) - *x->mark_y;
    m = s_skid_meshes[r_5532f0];
    m[0] = 8;
    m[1] = 4;
    skid_edge(nx, nz, e, width, ox, oz, mox, moz, 0, 0);
    /* The new edge about here, the last one about it too. */
    m[2] = nx[0];
    m[3] = 0;
    m[4] = nz[0];
    m[5] = nx[1];
    m[6] = 0;
    m[7] = nz[1];
    m[8] = x->mark_x[0] - rx;
    m[9] = y0;
    m[10] = x->mark_z[0] - rz;
    m[11] = x->mark_x[1] - rx;
    m[12] = y0;
    m[13] = x->mark_z[1] - rz;
    m[14] = nx[2];
    m[15] = 0;
    m[16] = nz[2];
    m[17] = nx[3];
    m[18] = 0;
    m[19] = nz[3];
    m[20] = x->mark_x[2] - rx;
    m[21] = y0;
    m[22] = x->mark_z[2] - rz;
    m[23] = x->mark_x[3] - rx;
    m[24] = y0;
    m[25] = x->mark_z[3] - rz;
    {
        static const s32 faces[4][3] = { { 0, 1, 2 }, { 2, 1, 3 }, { 4, 5, 6 }, { 6, 5, 7 } };

        for (k = 0; k < 4; k++) {
            m[26 + 5 * k] = 20;
            m[27 + 5 * k] = faces[k][0];
            m[28 + 5 * k] = faces[k][1];
            m[29 + 5 * k] = faces[k][2];
            m[30 + 5 * k] = colour;
        }
    }
    d = &g_particle_objs[r_5532f0];
    scene_obj_set_mesh(d, m, 0, 200, 0, 20, 0);
    d->x = rx;
    d->y = (s32)(sin(c->f110) * 20.0 + (double)(s32)c->y);
    d->z = rz;
    d->ax = d->ay = d->az = 0;
    if (d->in_grid) {
        if (scene_obj_move(d) != 0) {
            log_printf("FEL VID LI_MOVEOBJECT SLADD OBJ 1\n");
        }
    } else {
        scene_obj_link(d);
    }
    if (++r_5532f0 >= SKID_MARKS) {
        r_5532f0 = 0;
    }
    *x->mark_y = (s32)(sin(c->f110) * 20.0 + c->y);
    skid_edge(x->mark_x, x->mark_z, e, width, ox, oz, mox, moz, rx, rz);
    *x->last_x = rx;
    *x->last_z = rz;
}

/* A skid starting: the edge where the axle is, across the way it came
 * from the last mark. */
static void skid_start(struct car *c, struct skid_axle *x, int model)
{
    double cs, sn, w = (double)g_wheel_info[4 * model].dz, width = (double)g_wheel_info[4 * model].top;
    s32 ox, oz, mox, moz;
    double e;

    cs = cos(c->heading + 1.5707963267949);
    sn = sin(c->heading + 1.5707963267949);
    ox = (s32)(cs * w);
    oz = (s32)(sn * w);
    mox = (s32)-(cs * w);
    moz = (s32)-(sn * w);
    e = atan2(x->az - (double)*x->last_z, x->ax - (double)*x->last_x);
    *x->flag = 1;
    *x->mark_y = (s32)(sin(c->f110) * 20.0 + c->y);
    e += 1.5707963267949;
    skid_edge(x->mark_x, x->mark_z, e, width, ox, oz, mox, moz, (s32)x->ax, (s32)x->az);
    *x->last_x = (s32)x->ax;
    *x->last_z = (s32)x->az;
}

static int skid_angle(double slip)
{
    return (!(slip <= 0.392699081698725) && slip < 2.748893571891075)
           || (!(slip <= 3.5342917352885252) && slip < 5.890486225480876);
}

/* The rear skid's sound: the surface's (its material, by 9 to go on
 * and by 10 to start), faster with speed. */
static void skid_sound_start(struct car *c, int surf_mod)
{
    s32 s = surf_mod;

    c->skid_voice_rear = race_sound_3d(0, 1, s, -1, g_skid_volume[s],
                                       (s32)(c->f118 * 100.0 + (double)g_skid_rate[s]), 0,
                                       (float)(s32)c->rear_x, (float)(s32)c->rear_z);
}

/* 0x42bc80
 * Skids under car 0x5285c0: an axle sliding (its wheels' travel 22.5
 * to 157.5 degrees off the heading) above 8 km/h, for the rear also
 * while the screech counts down (f2f8), and above 10 for the front, on
 * the ground on a surface that marks: smoke but on sand (5, 15, 25),
 * marks every 30 and, for the rear, the skid sound. */
void race_skids(void)
{
    struct car *c = &g_cars[r_5285c0];
    int model = g_drivers[r_5285c0].car;
    double side = (double)g_wheel_info[4 * model].dz;
    struct skid_axle rear = { c->rear_x, c->rear_z, c->slip, c->rear_mark_x, &c->rear_mark_y,
                              c->rear_mark_z, &c->rear_last_x, &c->rear_last_z, &c->f298,
                              c->surface[2], c->surface[3] };
    struct skid_axle front = { c->front_x, c->front_z, c->front_slip, c->front_mark_x,
                               &c->front_mark_y, c->front_mark_z, &c->front_last_x,
                               &c->front_last_z, &c->f29c, c->surface[0], c->surface[1] };
    s32 colour;

    gl3d_set_dynamic(&s_skid_meshes[0][0], &s_skid_meshes[SKID_MARKS - 1][SKID_MESH_DWORDS]);

    colour = rear.s0 >= 0 && rear.s0 < 40 ? g_skid_colour[rear.s0] : -1;
    if (((skid_angle(c->slip) && !(c->f118 <= 8.0)) || c->skid != 0) && c->f270 == 0
        && colour != -1) {
        if (c->f298 == 1) {
            s32 now, before;

            if (rear.s0 != 5 && rear.s1 != 5 && rear.s0 != 15 && rear.s1 != 15 && rear.s0 != 25
                && rear.s1 != 25 && r_563be0 == 1) {
                skid_smoke(c, &rear, side);
            }
            skid_mark(c, &rear, colour, model);
            now = (s32)fmod((double)c->surface[2], 9.0);
            before = (s32)fmod((double)c->last_surface[2], 9.0);
            if (now == before) {
                struct sound_voice *v = sound_voice_of(c->skid_voice_rear);

                if (v != NULL) {
                    v->rate = (s32)((double)g_skid_rate[now] + c->f118 * 25.0);
                }
            } else {
                stop_voice(c->skid_voice_rear);
                skid_sound_start(c, now);
            }
        } else {
            s32 s;

            skid_start(c, &rear, model);
            s = (s32)fmod((double)c->surface[2], 10.0);
            skid_sound_start(c, s == 9 ? 4 : s);
        }
    } else {
        if (c->f298 == 1) {
            stop_voice(c->skid_voice_rear);
        }
        c->f298 = 0;
    }

    colour = front.s0 >= 0 && front.s0 < 40 ? g_skid_colour[front.s0] : -1;
    if (skid_angle(c->front_slip) && !(c->f118 <= 10.0) && c->f270 == 0 && colour != -1) {
        if (c->f29c == 1) {
            if (front.s0 != 5 && front.s1 != 5 && front.s0 != 15 && front.s1 != 15 && front.s0 != 25
                && front.s1 != 25 && r_563be0 == 1) {
                skid_smoke(c, &front, side);
            }
            skid_mark(c, &front, colour, model);
        } else {
            skid_start(c, &front, model);
        }
    } else {
        c->f29c = 0;
    }
}
