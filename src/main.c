/*
 * main.c - entry point. WinMain 0x4120a0.
 *
 *   ignition [--data DIR] [--view FILE.PIC] [--text TEXT] [--press MS:KEY]...
 *            [--background] [--shot MS:FILE.TGA] [--cue IMAGE.CUE] [--res N]
 *            [--software] [--soft3d] [--effects 2d|3d] [--host]
 *            [--connect HOST[:PORT]]
 *
 * --data is the folder with the game files, the disc root as extracted
 * by `make extract`. --view shows one picture through the renderer, with
 * its own palette. --text runs the game's start up, then draws TEXT the
 * way the front end draws "LOADING" (0x402b24): small.lft, centred on
 * x 160 at y 94 of the cleared 320 x 200 buffer, with menu.col. Both
 * exit on Escape. --press presses a key (DirectInput scan code, hex) MS
 * milliseconds after start up, for scripted runs; --background keeps the
 * game running while its window is not active; --shot writes the screen
 * to a TGA file at a time. --cue names the disc image for CD music;
 * without it the .cue next to the data folder plays. --res sets the
 * internal resolution: 0 320 x 200, 1 640 x 480, 2 800 x 600, 3 the
 * window's. --software starts with the 2D drawn on the CPU, as the
 * original did (Alt+R switches). --soft3d draws the 3D on the CPU too,
 * with the original's pipeline and rasteriser (src/soft3d/), at the
 * screen's resolution. --effects 2d|3d sets EFFECTS over the saved
 * setting, for scripted comparisons (not saved). --host and --connect
 * choose what NETWORK GAME does (net_set_option). --version prints this
 * port's version.
 *
 * Without --data the host looks for the game (plat_find_data in
 * platform/common.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "net.h"
#include "soft3d/soft3d.h"
#include "canvas.h"
#include "cd.h"
#include "dik.h"
#include "lisa/file.h"
#include "lisa/font.h"
#include "lisa/lisa.h"
#include "lisa/log.h"
#include "lisa/mem.h"
#include "pic.h"
#include "platform/platform.h"
#ifndef __vita__
#include "platform/common.h"
#endif
#include "settings.h"
#include "startup.h"
#include "vfs.h"
#include "race/debug.h"
#include "race/init.h"
#include "version.h"
#include "video.h"

#ifdef __vita__
/* The game's files (the GOG or Steam release, copied as it is, with
 * game.ins and MUSIC/ in it), or else an extracted disc in disc/ with its
 * .cue and .bin beside it. */
#define DEFAULT_DATA "ux0:data/ignition/disc"
#define GAME_DATA "ux0:data/ignition"
#else
#define DEFAULT_DATA "data/Ignition/disc"
#endif

static int view_frame(void *user)
{
    (void)user;
    if (g_plat_keys[DIK_ESCAPE]) {
        return 0;
    }
    video_present();
    return 1;
}

static int view(const char *name)
{
    const u8 *pic;
    int y, w, h;

    mem_init();
    pic = file_load(name);
    if (pic == NULL || pic_u16(pic, 4) != PIC_MAGIC) {
        fprintf(stderr, "%s: not a picture (error 0x%x)\n", name, g_file_error);
        return 1;
    }
    w = pic_width(pic);
    h = pic_height(pic);
    printf("%s: %d x %d\n", name, w, h);
    if (!video_create_window()) {
        return 1;
    }
    g_video_mode.width = w;
    g_video_mode.height = h;
    g_video_mode.bpp = 8;
    g_video_mode.flag = 1;
    if (!video_set_mode()) {
        return 1;
    }
    for (y = 0; y < h; y++) {
        memcpy(g_video_screen + y * w, pic_pixels(pic) + y * w, (size_t)w);
    }
    memcpy(g_video_palette, pic_palette(pic), sizeof(g_video_palette));
    return plat_run_with(view_frame, NULL);
}

static int text_frame(void *user)
{
    (void)user;
    if (g_plat_keys[DIK_ESCAPE]) {
        return 0;
    }
    memcpy(g_video_screen, g_back_buffer, 320 * 200);
    video_present();
    return 1;
}

static int text(const char *s)
{
    int font;

    lisa_init();
    if (!video_create_window()) {
        return 1;
    }
    game_video_init();
    if (file_read_at("baltazar\\data\\menu.col", g_video_palette, 0x300, 8) != 1) {
        return 1;
    }
    font = g_fonts_menu[7];
    memset(g_back_buffer, 0, sizeof(g_back_buffer));
    text_draw(s, font, 160 - text_width(s, font) / 2, 94);
    return plat_run_with(text_frame, NULL);
}

int main(int argc, char **argv)
{
    const char *data = NULL;
    const char *view_name = NULL;
    const char *text_arg = NULL;
    int i;

#ifdef __vita__
    {
        /* The Vita has no command line: ux0:data/ignition/shots.txt lists
         * screenshots to take, a line "MS name.tga" each (MS from the start). */
        FILE *fp = fopen(GAME_DATA "/shots.txt", "r");
        char line[256];

        while (fp != NULL && fgets(line, sizeof(line), fp) != NULL) {
            unsigned ms;
            char name[128], path[256];

            if (sscanf(line, "%u %127s", &ms, name) == 2) {
                snprintf(path, sizeof(path), GAME_DATA "/%s", name);
                video_schedule_shot(ms, path);
            }
        }
        if (fp != NULL) {
            fclose(fp);
        }
    }
#endif
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data = argv[++i];
        } else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            view_name = argv[++i];
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            unsigned ms;
            char path[1024];

            if (sscanf(argv[++i], "%u:%1023s", &ms, path) == 2) {
                video_schedule_shot(ms, path);
            }
        } else if (strcmp(argv[i], "--cue") == 0 && i + 1 < argc) {
            cd_set_image(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0) {
            r_4949b4 = 1;
        } else if (strcmp(argv[i], "--software") == 0) {
            canvas_set_hardware(0);
        } else if (strcmp(argv[i], "--soft3d") == 0) {
            g_soft3d = 1;
            canvas_set_hardware(0);
        } else if (strcmp(argv[i], "--effects") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];

            if (strcmp(mode, "2d") == 0) {
                g_effects_override = 0;
            } else if (strcmp(mode, "3d") == 0) {
                g_effects_override = 1;
            } else {
                fprintf(stderr, "--effects takes 2d or 3d\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            canvas_force_resolution(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--autopilot") == 0) {
            g_race_autopilot = 1;
        } else if (strcmp(argv[i], "--background") == 0) {
            g_plat_run_inactive = 1;
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            unsigned ms, key, hold = 60;

            /* ms:key in hex, then how long it is held (60 ms). */
            if (sscanf(argv[++i], "%u:%x:%u", &ms, &key, &hold) >= 2) {
                plat_schedule_key(ms, (int)key, hold);
            }
        } else if (strcmp(argv[i], "--tap") == 0 && i + 1 < argc) {
            unsigned ms;
            float fx, fy;

            /* ms:x:y, x and y shares of the window. */
            if (sscanf(argv[++i], "%u:%f:%f", &ms, &fx, &fy) == 3) {
#ifndef __vita__
                plat_schedule_tap(ms, fx, fy);
#endif
            }
        } else if (strcmp(argv[i], "--host") == 0) {
            net_set_option(1, NULL);
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            net_set_option(0, argv[++i]);
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            text_arg = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("ignition %s\n", IGN_VERSION);
            return 0;
        } else {
            fprintf(stderr, "usage: %s [--data DIR] [--view FILE.PIC] [--text TEXT]\n", argv[0]);
            return 1;
        }
    }
    if (data == NULL) {
#ifdef __vita__
        FILE *probe = fopen(GAME_DATA "/BALTAZAR/DATA/MENU.COL", "rb");

        if (probe != NULL) {
            fclose(probe);
            data = GAME_DATA;
        } else {
            data = DEFAULT_DATA;
        }
#else
        data = plat_find_data(DEFAULT_DATA);
#endif
    }
    fprintf(stderr, "ignition %s, data: %s\n", IGN_VERSION, data);
    vfs_set_root(data);
    if (view_name != NULL) {
        return view(view_name);
    }
    if (text_arg != NULL) {
        return text(text_arg);
    }
    if (!app_startup()) {
        return 0;
    }
    return plat_run();
}
