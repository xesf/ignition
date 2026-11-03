/*
 * settings.h - the game settings, saved as ign_win.btz.
 *
 * 0x597 bytes at 0x4bf580 in the original, read and written whole. Menu
 * items point into it, and the code addresses fields by offset, some
 * not aligned; the accessors read and write at an offset.
 */

#ifndef IGN_SETTINGS_H
#define IGN_SETTINGS_H

#include <string.h>

#include "menu/menu.h"
#include "types.h"

enum {
    SETTINGS_VERSION = 0x1e,            /* the word at offset 0 */
    SET_VERSION = 0x000,
    SET_DIFFICULTY = 0x00c,
    SET_PLAYER1_NAME = 0x06c,           /* 15 bytes */
    SET_PLAYER2_NAME = 0x0e4,
    SET_CD_TRACK = 0x0f7,
    SET_CD_MUSIC = 0x0fb,
    SET_SFX_VOLUME = 0x0ff,
    SET_SCREEN_SIZE = 0x103,
    SET_RESOLUTION = 0x10b,
    SET_KEYS = 0x17f,                   /* key bindings, one scan code each */
    SET_LANGUAGE = 0x593,
    /* The port's, past the file's 0x597 bytes, kept in PORT_SETTINGS:
     * 0 the 2D on the GPU, 1 on the CPU. */
    SET_RENDERER = 0x598,
    /* The music's volume, 0 to 10 (the CD's in the original is on or
     * off). */
    SET_MUSIC_VOLUME = 0x59c,
    /* The effects' drawing, in ign_port.cfg: 0 2D (sprites on the 2D
     * layers), 1 3D (sprites in the 3D scene). Read when a race starts. */
    SET_EFFECTS = 0x5a0
};

/* --effects 2d|3d: 0 or 1 overrides the saved effects setting once the
 * port's settings are loaded; -1 keeps it. Defined in frontend.c. */
extern int g_effects_override;

static inline s32 settings_get(int offset)
{
    s32 v;

    memcpy(&v, g_settings + offset, sizeof(v));
    return v;
}

static inline void settings_set(int offset, s32 v)
{
    memcpy(g_settings + offset, &v, sizeof(v));
}

static inline void settings_set8(int offset, u8 v)
{
    g_settings[offset] = v;
}

/* 0x40c810 */
void settings_defaults(void);

#endif /* IGN_SETTINGS_H */
