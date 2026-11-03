/*
 * frontend/frontend.c - the front end: menu set up, assets and frame
 * (0x4029a0..0x404a60).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "canvas.h"
#include "cd.h"
#include "game.h"
#include "cdp.h"
#include "frontend/frontend.h"
#include "frontend/state.h"
#include "lisa/texture.h"
#include "input.h"
#include "lisa/file.h"
#include "lisa/font.h"
#include "lisa/gfx.h"
#include "lisa/sprite.h"
#include "menu/menu.h"
#include "menu/engine.h"
#include "menu/pages.h"
#include "pic.h"
#include "platform/platform.h"
#include "settings.h"
#include "soft3d/soft3d.h"
#include "sound.h"
#include "startup.h"
#include "stubs.h"
#include "video.h"

#define SETTINGS_FILE "ign_win.btz"

/* The current language's 25 pages (0x4be768); handlers compare the
 * current page with these. */
struct menu_page *g_frontend_pages[MENU_PAGE_TABLE];
/* Each language's row, as 0x404a60 copies it (0x4be8c8). */
struct menu_page *g_frontend_page_rows[MENU_LANGUAGES][MENU_PAGE_TABLE];
/* Each language's page list (0x4beb20). */
struct menu_page *const *g_frontend_lists[MENU_LANGUAGES];

s32 g_menu_mode_index;          /* 0x4be730, into g_menu_modes */
s32 g_menu_sound;               /* 0x4be874 */
s32 g_menu_language;            /* 0x4be60c */
u8 *g_menu_screen;              /* 0x4be840, the 320 x 200 menu buffer */
u8 *g_menu_palette;             /* 0x4be614, menu.col */
u8 *g_menu_table;               /* 0x4be86c, menu.tab */
u8 *g_render_target;            /* 0x498734 */
struct scene *g_menu_camera;             /* 0x4be818, the 3D scene's camera */

/* Pictures without their header (0x34e bytes), as 0x403e80 reads them. */
u8 *g_pic_cars;                 /* 0x4be84c, bilar.pic, 186 wide */
u8 *g_pic_tracks;               /* 0x4be820, trk_spr.pic, 115 wide */
u8 *g_pic_logo;                 /* 0x4beb48, ign_logo.pic */
u8 *g_pic_car_select;           /* 0x4bf560, car_sel.pic */
u8 *g_pic_flags;                /* 0x4be8a8, flaggor.pic */

u8 *g_menu_black;               /* 0x4beb38, 0x300 bytes of 0 */
u8 *g_menu_white;               /* 0x4be580, 0x300 bytes of 0xff */
u8 *g_menu_palette_work;        /* 0x4be720 */
u8 *g_menu_buffer1;             /* 0x4be81c, 0xfb41 bytes */
u8 *g_menu_buffer2;             /* 0x4be8b8, 0xfb41 bytes */

/* The descriptor sprites are cut with (0x4be738), shared with the
 * handlers. */
struct tex_desc g_menu_desc;
struct sprite *g_sprite_logo;           /* 0x4bf558 */
struct sprite *g_sprite_panel;          /* 0x4be824 */
struct sprite *g_sprite_panel2;         /* 0x4be844 */
struct sprite *g_sprite_badges[12];     /* 0x4be7d0 */
struct sprite *g_sprite_car_select;     /* 0x4be800 */
struct sprite *g_sprite_car;            /* 0x4be848 */

s32 g_menu_sound_on;                    /* 0x4be5f0 */
static double s_dt;                     /* 0x4beb40 */
s32 g_menu_volume;                      /* 0x4be590 */
static int s_mode_pending;              /* 0x4be65c */
static int s_load_language = 1;         /* 0x47dc68 */
static int s_sound_warned;              /* 0x47dc4c */
static s32 s_sound_params[3];           /* 0x4be808 */
static double s_last_ticks;             /* 0x4bf568 */

/* The menu's video modes (0x47dc30). */
const s32 g_menu_modes[3][2] = { { 320, 200 }, { 640, 480 }, { 800, 600 } };

/* Reads ign_win.btz over the settings when its version matches. */
static void settings_load(void)
{
    s32 version = 0;

    if (file_exists(SETTINGS_FILE) != 1) {
        return;
    }
    file_read_at(SETTINGS_FILE, &version, 4, 0);
    if (version == SETTINGS_VERSION) {
        file_read_at(SETTINGS_FILE, g_settings, SETTINGS_SIZE, 0);
    }
}

/* The ticks since the last menu frame (0x4beb40). */
double menu_dt(void)
{
    return s_dt;
}

/* The animation the intro plays (0x4be5b8). */
static struct cdp_anim s_intro_anim;
u32 g_intro_sizes[6];       /* the sizes of fe_4be880's files */

int frontend_load_language(void)
{
    return s_load_language;
}

/* The animations draw each frame over the last, so they keep their own
 * picture, put on the menu screen after each frame. */
static u8 s_intro_pixels[320 * 200];

/* The colour about the film in NATIVE: the first film's ground (its
 * first pixel) as it fades, then kept for the films after it as their
 * palettes change. */
static int s_film_rgb[3];

static int film_margin(const u8 *pal, const u8 *pixels, int first_film)
{
    int i, best = 0, least = 1 << 30;

    if (first_film) {
        for (i = 0; i < 3; i++) {
            s_film_rgb[i] = pal[3 * pixels[0] + i];
        }
    }
    for (i = 0; i < 256; i++) {
        int dr = pal[3 * i] - s_film_rgb[0], dg = pal[3 * i + 1] - s_film_rgb[1];
        int db = pal[3 * i + 2] - s_film_rgb[2], d = dr * dr + dg * dg + db * db;

        if (d < least) {
            least = d;
            best = i;
        }
    }
    return best;
}

/* Starts the intro's first animation into `screen` (0x409b4f). */
void frontend_intro_start(void *file, u8 *screen)
{
    (void)screen;
    memset(s_intro_pixels, 0, sizeof(s_intro_pixels));
    s_intro_anim.size = g_intro_sizes[0];
    s_intro_anim.file = file;
    s_intro_anim.screen = s_intro_pixels;
    cdp_open(&s_intro_anim);
}

/* 0x403d40: plays sample `index` of group `group` of the menu's bank
 * at `volume` and `rate`, when the menu has sound. Returns the handle,
 * or -1. */
s32 sound_load(int group, int index, s32 volume, s32 rate)
{
    struct sound_request req;

    if (!g_menu_sound) {
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.group = group;
    req.index = index;
    req.volume = volume;
    req.rate = rate;
    return sound_start(&req);
}

/* 0x403df0
 * Ticks of 1 / 0.036 ms since the last call: 0.01 when no time passed,
 * at most 10.8. */
double menu_ticks(void)
{
    double now = (double)(s32)g_frame_time * 0.036;
    double ticks = now - s_last_ticks;

    s_last_ticks = now;
    if (ticks == 0.0) {
        return 0.01;
    }
    return ticks > 10.8 ? 10.8 : ticks;
}

/* 0x403e70 */
void menu_sound_tick(void)
{
    if (g_menu_sound) {
        sound_tick();
    }
}

/* RESOLUTION offers 320X200, 640X480 and 800X600; the port adds
 * NATIVE, the window's size, as a fourth value. A choice's visuals are
 * its two labels, then two for each value. */
static void add_native_resolution(void)
{
    static struct menu_visual visuals[MENU_LANGUAGES][10];
    static int done;
    int lang, slot, k;

    if (done) {
        return;
    }
    done = 1;
    for (lang = 0; lang < MENU_LANGUAGES; lang++) {
        for (slot = 0; slot < MENU_PAGE_SLOTS; slot++) {
            struct menu_page *p = g_menu_pages[lang][slot];

            if (p == NULL) {
                continue;
            }
            for (k = 0; k < p->count; k++) {
                struct menu_item *it = &p->items[k];

                if (it->value != (s32 *)(g_settings + SET_RESOLUTION) || it->max != 2
                    || it->visuals == visuals[lang]) {
                    continue;
                }
                memcpy(visuals[lang], it->visuals, 8 * sizeof(visuals[lang][0]));
                visuals[lang][8] = visuals[lang][6];
                visuals[lang][9] = visuals[lang][7];
                visuals[lang][8].text = "NATIVE";
                visuals[lang][9].text = "NATIVE";
                it->visuals = visuals[lang];
                it->max = 3;
            }
        }
    }
}

/* The port's settings, which ign_win.btz has no room for (the race
 * writes that file at its own size): the renderer, the music's volume,
 * then the effects. */
#define PORT_SETTINGS "ign_port.cfg"

int g_effects_override = -1;

static void port_settings_save(void)
{
    s32 v[3];

    v[0] = settings_get(SET_RENDERER);
    v[1] = settings_get(SET_MUSIC_VOLUME);
    v[2] = settings_get(SET_EFFECTS);
    file_write(PORT_SETTINGS, v, sizeof(v));
}

/* Moves an item's labels and values down by `dy`. */
static void move_item(struct menu_item *it, int dy)
{
    int v, n = 2 + 2 * (it->max - it->min + 1);

    for (v = 0; v < n; v++) {
        it->visuals[v].y = (s16)(it->visuals[v].y + dy);
        it->visuals[v].text_y = (s16)(it->visuals[v].text_y + dy);
    }
}

/* A port's item for a page: its value, the values' names, and where its
 * visuals come from (`like`, an item of the page with values, whose
 * label and first value's places it takes, at row y). */
struct new_item {
    int offset, min, max;
    const char *label;
    const char *const *names;
    struct menu_visual *visuals;    /* 2 + 2 * (max - min + 1) */
};

/* Puts `n` into page p at index `at`, rows from there on one lower. */
static void insert_item(struct menu_page *p, struct menu_item *items, int at,
                        const struct menu_item *like, const struct new_item *n, int y)
{
    enum { ROW = 19 };
    int k, v, values = n->max - n->min + 1;
    struct menu_item it = *like;

    for (k = 0; k < 2; k++) {
        n->visuals[k] = like->visuals[k];
        n->visuals[k].text = n->label;
    }
    for (v = 0; v < values; v++) {
        for (k = 0; k < 2; k++) {
            n->visuals[2 + 2 * v + k] = like->visuals[2 + k];
            n->visuals[2 + 2 * v + k].text = n->names[v];
        }
    }
    for (k = 0; k < 2 + 2 * values; k++) {
        n->visuals[k].y = n->visuals[k].text_y = (s16)y;
    }
    it.value = (s32 *)(g_settings + n->offset);
    it.min = n->min;
    it.max = n->max;
    it.visuals = n->visuals;
    memcpy(items, p->items, (size_t)at * sizeof(*items));
    items[at] = it;
    memcpy(items + at + 1, p->items + at, (size_t)(p->count - at) * sizeof(*items));
    for (k = at + 1; k <= p->count; k++) {
        if (items[k].value != NULL && items[k].visuals != NULL) {
            move_item(&items[k], ROW);
        }
    }
    p->items = items;
    p->count++;
}

/* The page with an item of g_settings + offset, in a language. */
static struct menu_page *page_with(int lang, int offset, int *index)
{
    int slot, k;

    for (slot = 0; slot < MENU_PAGE_SLOTS; slot++) {
        struct menu_page *p = g_menu_pages[lang][slot];

        for (k = 0; p != NULL && k < p->count; k++) {
            if (p->items[k].value == (s32 *)(g_settings + offset)) {
                *index = k;
                return p;
            }
        }
    }
    return NULL;
}

/* The port's items:
 * - GFX OPTIONS gets RENDERER, then EFFECTS last: HARDWARE draws the 2D
 *   on the GPU, SOFTWARE on the CPU as the original (the 3D on the GPU),
 *   SOFT 3D the 3D on the CPU too, with the original's pipeline
 *   (src/soft3d/). EFFECTS is 2D or 3D (SET_EFFECTS). The page goes up
 *   for the two rows, so that 320 x 200 holds it.
 * - SOUND OPTIONS gets MUSIC VOL. under CD MUSIC, 0 to 10, as SFX
 *   VOLUME, which the original has for the effects alone. */
static void add_port_options(void)
{
    enum { RAISE = 15, ROW = 19, MAX_ITEMS = 16 };
    static const char *const renderer[3] = { "HARDWARE", "SOFTWARE", "SOFT 3D" };
    static const char *const effects[2] = { "2D", "3D" };
    static const char *const levels[11] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"
    };
    static struct menu_item gfx_items[MENU_LANGUAGES][MAX_ITEMS];
    static struct menu_item effects_items[MENU_LANGUAGES][MAX_ITEMS];
    static struct menu_item sound_items[MENU_LANGUAGES][MAX_ITEMS];
    static struct menu_visual gfx_visuals[MENU_LANGUAGES][8];
    static struct menu_visual effects_visuals[MENU_LANGUAGES][6];
    static struct menu_visual music_visuals[MENU_LANGUAGES][24];
    static int done;
    int lang, k;

    if (done) {
        return;
    }
    done = 1;
    for (lang = 0; lang < MENU_LANGUAGES; lang++) {
        struct new_item n;
        struct menu_page *p;
        int at, lowest = -1000;
        const struct menu_item *pair = NULL;

        p = page_with(lang, SET_RESOLUTION, &at);
        if (p != NULL && p->count < MAX_ITEMS) {
            for (k = 0; k < p->count; k++) {
                const struct menu_item *it = &p->items[k];

                if (it->max == 1 && it->visuals != NULL && pair == NULL) {
                    pair = it;
                }
                if (it->visuals != NULL && it->visuals[0].y > lowest) {
                    lowest = it->visuals[0].y;
                }
            }
            if (pair != NULL) {
                int raise = RAISE;

                n.offset = SET_RENDERER;
                n.min = 0;
                n.max = 2;
                n.label = "RENDERER";
                n.names = renderer;
                n.visuals = gfx_visuals[lang];
                insert_item(p, gfx_items[lang], p->count, pair, &n, lowest + ROW);
                if (p->count < MAX_ITEMS) {
                    n.offset = SET_EFFECTS;
                    n.min = 0;
                    n.max = 1;
                    n.label = "EFFECTS";
                    n.names = effects;
                    n.visuals = effects_visuals[lang];
                    insert_item(p, effects_items[lang], p->count, pair, &n, lowest + 2 * ROW);
                    raise += ROW;
                }
                for (k = 0; k < p->count; k++) {
                    if (p->items[k].value != NULL && p->items[k].visuals != NULL) {
                        move_item(&p->items[k], -raise);
                    }
                }
            }
        }
        p = page_with(lang, SET_SFX_VOLUME, &at);
        if (p != NULL && p->count < MAX_ITEMS) {
            const struct menu_item *sfx = &p->items[at];
            int y = sfx->visuals[0].y;

            n.offset = SET_MUSIC_VOLUME;
            n.min = 0;
            n.max = 10;
            n.label = "MUSIC VOL.";
            n.names = levels;
            n.visuals = music_visuals[lang];
            insert_item(p, sound_items[lang], at, sfx, &n, y);
        }
    }
}

/* PORT_SETTINGS, or the defaults: the renderer the resolution's (the
 * GPU for NATIVE, the CPU for the original's sizes; --software starts
 * on the CPU and --soft3d with the 3D on it too, whatever is saved),
 * the music at 10, the effects 3D on the Vita and 2D elsewhere. A file
 * of one or two words (from before the effects, or before the music)
 * keeps the defaults for the words it lacks. --effects overrides the
 * effects. */
static void port_settings_load(int resolution)
{
    s32 v[3];
    int words;

    v[0] = resolution == CANVAS_NATIVE ? 0 : 1;
    v[1] = 10;
    /* 2D everywhere: on the Vita, 3D has not been faster than 2D yet. */
    v[2] = 0;
    if (file_exists(PORT_SETTINGS) == 1) {
        long size = file_size(PORT_SETTINGS);

        words = size >= 3 * (long)sizeof(s32) ? 3 : size >= 2 * (long)sizeof(s32) ? 2 : 1;
        file_read_at(PORT_SETTINGS, v, words * sizeof(s32), 0);
    }
    if (!canvas_hardware() && v[0] == 0) {
        v[0] = 1;
    }
    if (g_soft3d) {
        v[0] = 2;
    }
    if (g_effects_override >= 0) {
        v[2] = g_effects_override;
    }
    v[0] = v[0] < 0 || v[0] > 2 ? 1 : v[0];
    settings_set(SET_RENDERER, v[0]);
    settings_set(SET_MUSIC_VOLUME, v[1] < 0 ? 0 : v[1] > 10 ? 10 : v[1]);
    settings_set(SET_EFFECTS, v[2] < 0 ? 0 : v[2] > 1 ? 1 : v[2]);
    g_soft3d = v[0] == 2;
    canvas_set_hardware(v[0] == 0);
    cd_set_music_volume(settings_get(SET_MUSIC_VOLUME));
}

/* The window for the RESOLUTION option: the original's sizes get a
 * window of that size (320 x 200 shown at 640 x 480); NATIVE gets back
 * the size the window had before. */
static void size_window(int resolution)
{
    static int saved_w, saved_h;
    int w = 0, h = 0;

    if (resolution == CANVAS_NATIVE) {
        if (saved_w > 0) {
            plat_window_size(saved_w, saved_h, &w, &h);
            saved_w = saved_h = 0;
        }
        return;
    }
    plat_window_size(resolution == CANVAS_800X600 ? 800 : 640,
                     resolution == CANVAS_800X600 ? 600 : 480, &w, &h);
    if (saved_w == 0) {
        saved_w = w;
        saved_h = h;
    }
}

/* The internal resolution the RESOLUTION option asks for, from the
 * settings file: the menu reads it before the settings are loaded. */
static int menu_resolution(void)
{
    s32 version = 0, mode = CANVAS_320X200;

    if (file_exists(SETTINGS_FILE) == 1) {
        file_read_at(SETTINGS_FILE, &version, 4, 0);
        if (version == SETTINGS_VERSION) {
            file_read_at(SETTINGS_FILE, &mode, 4, SET_RESOLUTION);
        }
    }
    return mode >= CANVAS_320X200 && mode <= CANVAS_NATIVE ? mode : CANVAS_320X200;
}

/* 0x403da0 */
void menu_present(void)
{
    static s32 last = -1;
    const s32 *mode = g_menu_modes[g_menu_mode_index];
    s32 res = settings_get(SET_RESOLUTION);

    static s32 last_renderer = -1, last_music = -1, last_effects = -1;
    static int last_hardware = -1;
    s32 renderer;

    menu_sound_tick();
    /* The RESOLUTION option takes effect as it changes, and sets the
     * renderer to the resolution's default. */
    if (res >= CANVAS_320X200 && res <= CANVAS_NATIVE && res != last) {
        /* --res fixes the resolution whatever the option says. */
        if (last != -1) {
            canvas_set_resolution(res);
            /* SOFT 3D stays: it races at the original's sizes. */
            if (settings_get(SET_RENDERER) != 2) {
                settings_set(SET_RENDERER, canvas_resolution() != CANVAS_NATIVE);
            }
        }
        size_window(canvas_resolution());
        last = res;
    }
    /* Alt+R switches the renderer too: the option follows it. */
    if (last_hardware != -1 && canvas_hardware() != last_hardware) {
        settings_set(SET_RENDERER, !canvas_hardware());
        last_renderer = -1;
    }
    renderer = settings_get(SET_RENDERER);
    if (renderer != last_renderer) {
        g_soft3d = renderer == 2;
        canvas_set_hardware(renderer == 0);
        if (last_renderer != -1) {
            port_settings_save();
        }
        last_renderer = renderer;
    }
    if (settings_get(SET_MUSIC_VOLUME) != last_music) {
        cd_set_music_volume(settings_get(SET_MUSIC_VOLUME));
        if (last_music != -1) {
            port_settings_save();
        }
        last_music = settings_get(SET_MUSIC_VOLUME);
    }
    if (settings_get(SET_EFFECTS) != last_effects) {
        if (last_effects != -1) {
            port_settings_save();
        }
        last_effects = settings_get(SET_EFFECTS);
    }
    last_hardware = canvas_hardware();
    video_blit(g_menu_screen, mode[0], 0, 0, mode[0], mode[1], 0, 0);
    video_flip();
}

/* 0x403b10 */
static void menu_set_mode(void)
{
    const s32 *mode = g_menu_modes[g_menu_mode_index];
    double one = 1.0;

    if (!s_mode_pending) {
        video_set_palette(g_menu_black);
        menu_sound_tick();
        video_clear();
        video_flip();
    }
    g_video_mode.bpp = 8;
    g_video_mode.flag = 1;
    g_video_mode.width = mode[0];
    g_video_mode.height = mode[1];
    video_set_mode();
    g_menu_camera->center_x = mode[0] / 2;
    g_menu_camera->zoom = one;
    s_mode_pending = 0;
}

/* 0x403bc0
 * The original waits on Return in a loop of input_poll calls when no
 * sound card answers. */
static void menu_sound_init(void)
{
    int r;

    s_sound_params[0] = 5;
    s_sound_params[1] = 100;
    s_sound_params[2] = 2;
    r = sound_open(s_sound_params);
    if (r == 2 && !s_sound_warned) {
        int mini = g_fonts_menu[6];

        canvas_clear(g_menu_screen, 0, 64000);
        g_fonts[mini].align = FONT_ALIGN_CENTER;
        text_draw("NO SOUNDCARD FOUND, PLEASE REFER TO THE MANUAL", mini, 160, 90);
        text_draw("PRESS RETURN TO CONTINUE", mini, 160, 110);
        g_fonts[mini].align = FONT_ALIGN_LEFT;
        menu_present();
        input_poll();
        input_poll();
        input_poll();
        while (input_key_down(0x1c) != 1) {
            input_poll();
        }
        s_sound_warned = 1;
        return;
    }
    if (r == 0) {
        g_menu_sound_on = 0;
        g_menu_sound = 0;
    } else if (r == 1) {
        g_menu_sound_on = 1;
        g_menu_sound = 1;
        sound_load_bank("baltazar\\data\\sound\\", 0);
        g_menu_volume = 0xffff;
        sound_set_volume(0xffff);
    }
}

/* 0x404a60, after its static tables (0x409434 on); the tables are
 * menu_data.c. Returns the root page. */
struct menu_page *menu_setup(void)
{
    int lang, k;

    memcpy(g_frontend_pages, g_menu_page_table[0], sizeof(g_frontend_pages));
    settings_defaults();
    for (k = 0; k < MENU_PAGE_TABLE; k++) {
        settings_set(0x507 + 4 * k, g_frontend_pages[k]->selected);
    }
    memcpy(g_settings + 0x1bf, g_settings_block_1bf, sizeof(g_settings_block_1bf));
    settings_load();

    settings_set(SET_SCREEN_SIZE, 3 - settings_get(0x107));
    if (settings_get(0x587) < settings_get(SET_DIFFICULTY)) {
        settings_set(SET_DIFFICULTY, settings_get(0x587));
    }
    /* Each language's pages take their selection from the settings; the
     * first page starts at its first item, and DIFFICULTY (page 15)
     * offers as many levels as have been reached. */
    for (lang = 0; lang < MENU_LANGUAGES; lang++) {
        for (k = 0; k < MENU_PAGE_TABLE; k++) {
            struct menu_page *p = g_menu_page_table[lang][k];

            g_frontend_page_rows[lang][k] = p;
            p->selected = settings_get(0x507 + 4 * k);
        }
        g_frontend_page_rows[lang][0]->selected = 0;
        g_frontend_page_rows[lang][15]->items[0].max = settings_get(0x587);
    }
    for (lang = 0; lang < MENU_LANGUAGES; lang++) {
        g_frontend_lists[lang] = g_menu_pages[lang];
    }

    g_menu.screen_w = g_menu_modes[g_menu_mode_index][0];
    g_menu.screen_h = g_menu_modes[g_menu_mode_index][1];
    g_menu.root = g_menu_page_table[0][21];
    g_menu.callbacks = g_menu_callbacks;
    g_menu.key_pressed = g_key_pressed;
    g_menu.pages = g_frontend_lists[0];
    g_menu.bindable = g_menu_bindable;
    g_menu.tree = g_menu_tree;
    g_menu.tree_count = MENU_TREE_ENTRIES;
    g_menu.fonts = g_fonts_menu;
    g_menu.scaled = 0;
    g_menu.sound = g_menu_sound;
    g_menu.centre = 1;
    g_menu.keep_selection = 1;
    menu_engine_init();
    return g_menu.root;
}

static u8 *read_pixels(const char *name, u32 size)
{
    u8 *pixels = malloc(size);

    file_read_at(name, pixels, size, PIC_PIXELS);
    return pixels;
}

static struct sprite *cut(u8 *pixels, int w, int h, int stride, s32 hot_x, s32 hot_y)
{
    g_menu_desc.width = w;
    g_menu_desc.height = h;
    g_menu_desc.pixels = pixels;
    g_menu_desc.stride = stride;
    g_menu_desc.hot_x = hot_x;
    g_menu_desc.hot_y = hot_y;
    return sprite_create(&g_menu_desc, NULL);
}

/* 0x403e80
 * Buffers, the menu pictures, the menu tables and the sprites cut from
 * N_SYSGFX.PIC and the menu pictures. The front end's state words set
 * here are named as their users are ported. */
int menu_load_assets(void)
{
    u8 *sysgfx = (u8 *)pic_pixels(g_sysgfx);
    int row, col, n;

    frontend_reset_state();
    g_menu_black = calloc(0x300, 1);
    g_menu_white = malloc(0x300);
    memset(g_menu_white, 0xff, 0x300);
    g_menu_palette_work = malloc(0x300);
    g_menu_buffer1 = calloc(0xfb41, 1);    /* new, uncleared, in the original */
    /* 0xfb41 bytes in the original; a panel on the last row writes its
     * corners past 200 rows, so the port gives it 240. */
    g_menu_buffer2 = calloc(320 * 240, 1);
    /* The background is drawn on the GPU; the page's layer is put
     * together on the CPU and blended over it. */
    canvas_register(g_menu_buffer1, 320, 200);
    g_menu_palette = malloc(0x300);
    file_read_at("baltazar\\data\\menu.col", g_menu_palette, 0x300, 8);
    g_pic_cars = read_pixels("baltazar\\data\\bilar.pic", 0x1e786);
    g_pic_tracks = read_pixels("baltazar\\data\\trk_spr.pic", 0x1374f);
    g_pic_logo = read_pixels("baltazar\\data\\ign_logo.pic", 0x2685);
    g_pic_car_select = read_pixels("baltazar\\data\\car_sel.pic", 0x49b6);
    g_pic_flags = read_pixels("baltazar\\data\\flaggor.pic", 0x978c);
    video_restore();
    menu_setup();
    frontend_reset_after_setup();

    memset(&g_menu_desc, 0, sizeof(g_menu_desc));
    g_sprite_logo = cut(g_pic_logo, 0xad, 0x39, 0xad, 0, 0);
    g_sprite_panel = cut(sysgfx + 0xfb4e - PIC_PIXELS, 0xfd, 0x3a, 0x100, 0x7e80, 0x1d00);
    g_sprite_panel2 = cut(sysgfx + 0x1354e - PIC_PIXELS, 0x7e, 0x6a, 0x100, 0x3f00, 0x3500);
    /* The twelve car badges, four to a row of 44 lines. */
    n = 0;
    for (row = 0; row < 3; row++) {
        for (col = 0; col < 4; col++) {
            g_sprite_badges[n++] = cut(sysgfx + row * 0x2c00 + col * 0x33,
                                       0x33, 0x2c, 0x100, 0x1900, 0x1600);
        }
    }
    g_sprite_car_select = cut(g_pic_car_select, 0xff, 0x4a, 0xff, 0, 0);
    frontend_reset_after_sprites();
    menu_ticks();
    return 1;
}

/* 0x4464f0: out = from + (to - from) * t / 256 for each of the 768
 * palette bytes; t of 256 and up gives `to`. */
void palette_lerp(const u8 *from, const u8 *to, u8 *out, int t)
{
    int i;

    if (t >= 256) {
        memcpy(out, to, 0x300);
        return;
    }
    for (i = 0; i < 0x300; i++) {
        int d = (to[i] - from[i]) * t;

        out[i] = (u8)(from[i] + (d / 256));
    }
}

/* Ten timed lines of debug text (0x47e8d8, 0x4beb50). */
static double s_message_time[10];
static char s_message[10][256];

/* 0x404310: shows `text` for `time` ticks in the first free line. */
void menu_message(const char *text, double time)
{
    int i;

    for (i = 0; i < 10; i++) {
        if (s_message_time[i] == 0.0 || i == 9) {
            break;
        }
    }
    if (i == 10) {
        return;
    }
    s_message_time[i] = time;
    strncpy(s_message[i], text, sizeof(s_message[i]) - 1);
}

/* 0x404380 */
static void menu_messages_draw(void)
{
    int i;

    for (i = 0; i < 10; i++) {
        if (s_message_time[i] > 0.0) {
            s_message_time[i] -= s_dt;
            if (s_message_time[i] < 0.0) {
                s_message_time[i] = 0.0;
            }
            text_draw(s_message[i], g_fonts_menu[0], 0, 16 * i);
        }
    }
}

/* 0x4043f0
 * The iris: rows within `t` x 4 (at 320 wide) of the middle keep a
 * circle of the picture, darkened at its rim through the cone table and
 * MENU.TAB; the rest goes dark. */
static void menu_iris(double t)
{
    const s32 *mode = g_menu_modes[g_menu_mode_index];
    int w = mode[0], h = mode[1];
    int scale = w / 320;
    double radius = t * scale * 4.0;
    int row = (int)(-100.0 * scale);
    double end = 300.0 * scale;
    /* The table row each pixel goes through; MENU.TAB's row 0 leaves
     * the pixel alone. */
    static u8 levels[320 * 200];
    int gpu = canvas_is_gpu(g_menu_screen) && w == 320 && h == 200;

    menu_sound_tick();
    if (gpu) {
        memset(levels, 0, sizeof(levels));
    }
    for (; row < end; row++) {
        double half = h / 2;
        int x = 0, col;

        if (row > half - radius && half + radius > row) {
            double a = asin(((row - (half - radius)) / (radius * 2.0) - 0.5) * 2.0);

            x = (int)(cos(a) * w / h * radius * 0.75);
        }
        if (x < 0) {
            x = 0;
        }
        if (w / 2 + 4 < x || row < 0 || h <= row) {
            continue;
        }
        for (col = 0; col < w; col++) {
            u8 *p = &g_menu_screen[row * w + col];
            u8 level = g_cone_table[x * 800 + 400 + col - w / 2];

            if (gpu) {
                levels[row * w + col] = level;
            } else {
                *p = g_menu_table[level << 8 | *p];
            }
        }
    }
    if (gpu) {
        canvas_remap(g_menu_screen, levels, g_menu_table);
    }
}

/* 0x4029a0 */
int menu_init(void)
{
    const s32 *mode;
    const char *loading;
    int small = g_fonts_menu[7];
    u8 *tab_raw;

    g_menu_mode_index = 0;
    g_menu_palette = malloc(0x300);
    file_read_at("baltazar\\data\\menu.col", g_menu_palette, 0x300, 8);
    g_menu_screen = malloc(0xfb41);
    add_native_resolution();
    add_port_options();
    canvas_set_resolution(menu_resolution());
    port_settings_load(canvas_resolution());
    canvas_register(g_menu_screen, 320, 200);
    g_menu_camera = scene_create();
    s_mode_pending = 1;
    menu_set_mode();
    video_set_palette(g_menu_palette);
    if (s_load_language) {
        s32 version = 0;
        int i;

        if (file_exists(SETTINGS_FILE) == 1
            && (file_read_at(SETTINGS_FILE, &version, 4, 0), version == SETTINGS_VERSION)) {
            file_read_at(SETTINGS_FILE, &g_menu_language, 4, SET_LANGUAGE);
        } else {
            g_menu_language = 0;
        }
        for (i = 0; i < FONT_MAX; i++) {
            g_fonts[i].proportional = 1;
            g_fonts[i].align = FONT_ALIGN_LEFT;
        }
    }
    /* menu.tab goes at a 64 KB boundary in the original, for the
     * rasteriser's addressing. */
    tab_raw = malloc(0x10000);
    file_read_at("baltazar\\data\\menu.tab", tab_raw, 0x10000, 0);
    g_menu_table = tab_raw;
    g_gfx_color_table = tab_raw;

    mode = g_menu_modes[g_menu_mode_index];
    set_target(g_menu_screen, mode[0], mode[0], mode[1], 8);
    canvas_clear(g_menu_screen, 0, 64000);
    loading = ui_text(0x47e0a0 + 200 * g_menu_language);
    text_draw(loading, small, 160 - text_width(loading, small) / 2, 0x5e);
    menu_present();
    raster_set_grid(2000, 2000, 11);
    g_menu_camera->stage_gather = 0;
    g_render_target = g_menu_screen;
    raster_set_screen(mode[0], mode[1], 0);
    menu_sound_init();
    input_poll();
    return menu_load_assets();
}


static s32 s_step;                  /* 0x4be878 */
static double s_step_part;          /* 0x4be578 */
static s32 s_joy_return[2];         /* 0x4be830 */
static s32 s_joy_escape[2];         /* 0x4be838 */
static s32 s_joy_x[2];              /* 0x4be5e0 */
static s32 s_joy_y[2];              /* 0x4be5e8 */

/* A random track of the disc's, or the one chosen; 0 means 7. */
static void cd_play_chosen(void)
{
    s32 t = settings_get(SET_CD_TRACK);

    if (t == -1) {
        s32 r = rand() * g_cd_track_count;

        t = (r + (r < 0 ? 0x7fff : 0)) >> 15;
        t++;
    } else if (t == 0) {
        t = 7;
    }
    cd_play_track(t);
}

/* Joystick moves and buttons become arrow, Return and Escape presses
 * (0x402c7f). Player 1's stick is off on page 5, player 2's on page 4,
 * where each is calibrated. */
static void joysticks_to_keys(void)
{
    int j;

    for (j = 0; j < 2; j++) {
        struct joystick *js = &g_joysticks[j];
        u8 *pressed = g_menu.key_pressed;

        if ((j == 0 && g_menu_view.page == g_frontend_pages[5])
            || (j == 1 && g_menu_view.page == g_frontend_pages[4])) {
            continue;
        }
        if (!js->present || !js->enabled) {
            continue;
        }
        if (js->buttons[0]) {
            if (!s_joy_return[j]) {
                pressed[0x1c] = 1;
                s_joy_return[j] = 1;
            }
        } else {
            s_joy_return[j] = 0;
        }
        if (js->buttons[1]) {
            if (!s_joy_escape[j]) {
                pressed[0x01] = 1;
                s_joy_escape[j] = 1;
            }
        } else {
            s_joy_escape[j] = 0;
        }
        if (abs(js->x) > 25) {
            if (!s_joy_x[j]) {
                pressed[js->x < 0 ? 0xcb : 0xcd] = 1;
                s_joy_x[j] = 1;
            }
        } else {
            s_joy_x[j] = 0;
        }
        if (abs(js->y) > 25) {
            if (!s_joy_y[j]) {
                pressed[js->y < 0 ? 0xc8 : 0xd0] = 1;
                s_joy_y[j] = 1;
            }
        } else {
            s_joy_y[j] = 0;
        }
    }
}

/* The intro's animations, one frame every 2.39 ticks, from 28.8 ticks
 * to 1278 (0x402dce). */
static void intro_step(void)
{
    fe_4be850 += s_step_part;
    /* The film fills the screen's height. */
    canvas_set_fit(g_menu_screen, fe_4be850 > 28.8 && fe_4be850 < 1278.0);
    if (fe_4be850 > 28.8 && fe_4be850 < 1278.0) {
        fe_4be560 += s_step_part;
        if (fe_4be560 >= 2.393103448275862) {
            while (fe_4be560 >= 2.393103448275862) {
                fe_4be560 -= 2.393103448275862;
            }
            menu_sound_tick();
            if (!cdp_next_frame(&s_intro_anim)) {
                if (++fe_4be56c == 6) {
                    fe_4be850 = 1278.0;
                } else {
                    s_intro_anim.file = fe_4be880[fe_4be56c];
                    s_intro_anim.size = g_intro_sizes[fe_4be56c];
                    cdp_open(&s_intro_anim);
                }
            }
            canvas_set_margin(g_menu_screen, film_margin(g_video_palette, s_intro_pixels,
                                                       fe_4be56c == 0 && s_intro_anim.frame <= 2));
            canvas_put(g_menu_screen, s_intro_pixels, 320);
            if (s_intro_anim.frame == 2) {
                cdp_draw(&s_intro_anim);
            }
        }
    }
    if (fe_4be850 > 1312.56) {
        menu_set_delay(0);
    }
    if (fe_4be850 >= 1335.6) {
        fe_4be850 = 0.0;
        video_set_palette(g_menu_palette);
        fe_4be584 = sound_voice_of(fe_4be8b0);
        if (fe_4be584 != NULL) {
            fe_4be584->rate = 0;
        }
        fe_4be584 = sound_voice_of(fe_4be8b4);
        if (fe_4be584 != NULL) {
            fe_4be584->rate = 0;
        }
        sound_stop_all();
        sound_unload(0, 0, 3);
        fe_4be598 = 1079.9;
    }
}

/* Counters that run at 27 a tick and the timers of the screens that
 * use them. */
static void step_timers(void)
{
    struct menu_page *cur = g_menu_view.page;

    if (fe_4be850 == 0.0 && cur != g_frontend_pages[21] && cur != g_frontend_pages[8]) {
        fe_4be598 += s_step_part;
        if (fe_4be598 >= 3780.0) {
            fe_4be598 = 1080.0;
        }
        if (fe_4be598 >= 1080.0) {
            int n = (int)(fe_4be598 - 1080.0);

            if (n % 168 == 0 && n / 168 < 16) {
                fe_4be58c = 1;
            }
        }
    }
    if (fe_4be5a8 != 0.0) {
        fe_4be5a8 += s_step_part;
        if (fe_4be5a8 >= 28.8) {
            fe_4be5a8 = 0.0;
        }
    }
    /* Quit, after the iris closes. */
    if (fe_4be8c0 != 0.0) {
        fe_4be8c0 += s_step_part;
        if (fe_4be8c0 >= 28.8) {
            menu_apply_settings();
            file_write(SETTINGS_FILE, g_settings, SETTINGS_SIZE);
            fe_4be828 = 1;
            fe_4be5a0 = 3;
        }
    }
    /* Start the race, after the iris closes. */
    if (fe_4be8a0 != 0.0) {
        fe_4be8a0 += s_step_part;
        if (fe_4be8a0 >= 28.8) {
            if (fe_net.mode == 2) {
                menu_apply_settings();
            }
            if (fe_net.mode != 2 && fe_net.stage < 1) {
                fe_net.stage = 1;
            }
            if (fe_net.stage == 1) {
                net_send_settings(&fe_net, g_settings);
            }
            file_write(SETTINGS_FILE, g_settings, SETTINGS_SIZE);
            s_load_language = 0;
            fe_4be828 = 1;
            fe_4be5a0 = 2;
        }
    }
    if (fe_4be87c > 0) {
        fe_4be87c += 27;
    }
    if (cur == g_frontend_pages[4] || cur == g_frontend_pages[5]) {
        if (fe_4be87c >= 160 && fe_4be87c < 187) {
            fe_4bf554 = 1;
        } else if (fe_4be87c >= 400) {
            fe_4be87c = 0;
            fe_4be5fc = 0;
        }
    } else {
        fe_4be87c = 0;
        fe_4be5fc = 0;
    }
    if (fe_4be594 > 0) {
        fe_4be594 += 27;
    }
    if (fe_4be594 >= 600) {
        fe_4be594 = 0;
    }
    if (fe_4be588 > 0) {
        fe_4be588 += 27;
    }
    if (fe_4be588 >= 600) {
        fe_4be588 = 0;
    }
    if (fe_4be724 > 0) {
        fe_4be724 += 27;
    }
    if (cur == g_frontend_pages[6]) {
        if (fe_4be724 >= 200 && fe_4be724 < 227) {
            fe_4be728 = 1;
        } else if (fe_4be724 >= 400) {
            fe_4be724 = 0;
        }
    } else {
        fe_4be724 = 0;
    }
    /* CD music: a new track when the last one ended. */
    if (settings_get(SET_CD_MUSIC) != 0
        && (fe_4be850 == 0.0 || fe_4be850 >= 1278.0) && cur != g_frontend_pages[21]) {
        fe_4be608 += 27;
        if (fe_4be608 > 2000) {
            fe_4be608 = 0;
            if (!cd_is_playing()) {
                sound_stop_all();
                cd_read_status();
                if (g_cd_present == 1) {
                    cd_read_tracks();
                    cd_play_chosen();
                }
            }
        }
    }
    if (fe_4bf564 != 0) {
        fe_4bf564 += 27;
        if (fe_4bf564 > 1100) {
            fe_4be58c = 1;
            fe_4bf564 = 0;
        }
    }
}

/* The intro's palette fades, by its clock (0x4033d1). */
static void intro_fades(void)
{
    if (s_intro_anim.palette == NULL) {
        return;
    }
    if (fe_4be850 >= 28.8 && fe_4be850 < 57.6) {
        palette_lerp(g_menu_white, s_intro_anim.palette, g_menu_palette_work,
                     (int)((fe_4be850 - 28.8) * 9.375));
        video_set_palette(g_menu_palette_work);
    } else if (fe_4be850 >= 1249.2 && fe_4be850 < 1278.0) {
        palette_lerp(s_intro_anim.palette, g_menu_white, g_menu_palette_work,
                     (int)((fe_4be850 - 1249.2) * 9.375));
        video_set_palette(g_menu_palette_work);
    }
    if (fe_4be850 < 28.8) {
        palette_lerp(g_menu_black, g_menu_white, g_menu_palette_work,
                     (int)(fe_4be850 * 9.375));
        video_set_palette(g_menu_palette_work);
    } else if (fe_4be850 >= 1306.8) {
        palette_lerp(g_menu_white, g_menu_palette, g_menu_palette_work,
                     (int)((fe_4be850 - 1306.8) * 9.375));
        video_set_palette(g_menu_palette_work);
    } else if (fe_47dc48) {
        fe_47dc48 = 0;
        memset(g_menu_buffer2, 0, 64000);
        canvas_clear(g_menu_screen, 0, 64000);
        fe_4be8b0 = sound_load(0, 3, 0xfde8, 0x5622);
        fe_4be8b4 = sound_load(0, 3, 0xfde8, 0x5622);
    }
}

static double iris_amount(double t)
{
    double v = t * 1.7361111111111112 - 10.0;

    return v < 0.0 ? 0.0 : v;
}

/* 0x402c00
 * Returns 1 while the menu runs, 2 to start a race, 3 to quit. */
int menu_frame(void)
{
    struct menu_page *cur;
    s32 volume;

    if (fe_4be600) {
        return fe_4be5a0;
    }
    video_restore();
    s_dt = menu_ticks();
    joystick_read();
    for (s_step = 0; (s32)s_dt >= s_step; s_step++) {
        s_step_part = (s32)s_dt == s_step ? s_dt - s_step : 1.0;
        joysticks_to_keys();
        input_poll();
        if (fe_4be850 != 0.0) {
            intro_step();
        }
        step_timers();
        if (fe_4be828 || fe_4be600) {
            return fe_4be5a0;
        }
    }
    cur = g_menu_view.page;
    if (fe_4be850 != 0.0) {
        intro_fades();
    }
    if (settings_get(SET_CD_MUSIC) != fe_4be8bc) {
        fe_4be8bc = settings_get(SET_CD_MUSIC);
        cd_set_volume((int)(fe_4be8bc * 25.5));
    }
    if (settings_get(SET_CD_MUSIC) != 0
        && (fe_4be850 == 0.0 || fe_4be850 > 1278.0) && cur != g_frontend_pages[21]
        && settings_get(SET_CD_TRACK) != fe_4be62c) {
        fe_4be62c = settings_get(SET_CD_TRACK);
        sound_stop_all();
        cd_read_status();
        if (g_cd_present == 1) {
            cd_read_tracks();
            cd_play_chosen();
        }
    }
    volume = settings_get(SET_SFX_VOLUME) * 6553;
    if (volume != g_menu_volume) {
        g_menu_volume = volume;
        if (volume == 0) {
            g_menu_sound = 0;
        } else if (g_menu_sound_on) {
            g_menu_sound = 1;
        }
        if (g_menu_sound && g_menu_sound_on) {
            sound_set_volume(volume);
        }
    }
    /* With the language known, the language page passes by itself. */
    if (cur == g_frontend_pages[21] && !s_load_language) {
        g_menu.key_pressed[0x1c] = 1;
    }
    if (fe_4be850 >= 1278.0 && fe_4bf550) {
        int i;

        fe_4bf550 = 0;
        for (i = 0; i < 6; i++) {
            free(fe_4be880[i]);
            fe_4be880[i] = NULL;
        }
        menucar_load();
    }
    if (menu_engine_frame() == -1) {
        menu_cb_4045b0(0, 6);
        return fe_4be5a0;
    }
    cur = g_menu_view.page;
    if (cur == g_frontend_pages[7]) {
        if (fe_4be634) {
            cur->selected = fe_4be634 - 1;
            g_menu.key_pressed[0x1c] = 1;
            menu_engine_frame();
            fe_4be634 = 0;
        } else {
            g_menu.key_pressed[0x01] = 1;
        }
    }
    if (fe_4be5a8 != 0.0) {
        menu_iris(iris_amount(fe_4be5a8));
    }
    if (fe_4be8c0 != 0.0) {
        menu_iris(iris_amount(28.8 - fe_4be8c0));
    }
    if (fe_4be8a0 != 0.0) {
        menu_iris(iris_amount(28.8 - fe_4be8a0));
    }
    menu_messages_draw();
    menu_present();
    fe_4be72c++;
    return fe_4be5a0;
}

static void free_sprite(struct sprite **s)
{
    if (*s != NULL) {
        sprite_create(NULL, *s);
        *s = NULL;
    }
}

static void free_block(void *p)
{
    free(*(void **)p);
    *(void **)p = NULL;
}

/* 0x4038f0
 * Leaves the front end: sound and CD off, a black palette, and every
 * sprite and buffer of the menu freed. The colour table pointed at
 * MENU.TAB, freed here; it goes back to the identity table. */
int menu_exit(void)
{
    int i;

    sound_stop_all();
    cd_stop_play();
    video_set_palette(startup_black_palette());
    menu_engine_free();
    sound_close();
    g_menu_sound = 0;
    menucar_free();
    scene_free();
    free_sprite(&g_sprite_car);
    free_sprite(&g_sprite_track);
    free_sprite(&g_sprite_logo);
    free_sprite(&g_sprite_panel);
    free_sprite(&g_sprite_panel2);
    free_sprite(&g_sprite_car_select);
    for (i = 0; i < 5; i++) {
        free_sprite(&g_sprite_flags[i]);
    }
    for (i = 0; i < 12; i++) {
        free_sprite(&g_sprite_badges[i]);
    }
    free_block(&g_menu_black);
    free_block(&g_menu_white);
    free_block(&g_menu_palette_work);
    canvas_unregister(g_menu_screen);
    canvas_unregister(g_menu_buffer1);
    free_block(&g_menu_screen);
    free_block(&g_menu_buffer1);
    free_block(&g_menu_buffer2);
    free_block(&g_menu_palette);
    free_block(&fe_4be5f8);
    free_block(&fe_4be630);
    free_block(&fe_4be898);
    free_block(&fe_4be804);
    free_block(&fe_4be604);
    free_block(&fe_4be5f4);
    free_block(&fe_4be658);
    free_block(&g_pic_cars);
    free_block(&g_pic_tracks);
    free_block(&g_pic_logo);
    free_block(&g_pic_car_select);
    free_block(&g_pic_flags);
    free_block(&g_menu_table);
    g_gfx_color_table = g_identity_table;
    return 1;
}

/* 0x4045b0: Yes on QUIT?. The iris closes, then the game quits. */
int menu_cb_4045b0(int arg0, int action)
{
    (void)arg0;
    if ((s8)action == MENU_KEY_ACTIVATE) {
        menu_set_delay(800);
        fe_4be8c0 = 0.01;
    }
    return 0;
}

/* 0x4045e0: the start of a race. The iris closes, then the race loads. */
int menu_cb_4045e0(int arg0, int action)
{
    (void)arg0;
    if ((s8)action == MENU_KEY_ACTIVATE) {
        menu_set_delay(800);
        fe_4be8a0 = 0.01;
    }
    return 0;
}

/* 0x404610: NETWORK GAME. The original drops out of full screen and
 * back, twice, around the DirectPlay dialogs; the OpenGL window needs
 * neither. Without a session it goes back a page. */
int menu_cb_404610(int arg0, int action)
{
    (void)arg0;
    net_stop();
    if ((s8)action != MENU_KEY_ACTIVATE) {
        return 0;
    }
    net_connect();
    if (g_net_state == 3 || g_net_state == 4) {
        net_poll_players();
        fe_4be634 = g_net_state == 3 ? 1 : 2;
        fe_net.stage = 0;
    } else {
        g_menu.key_pressed[0x01] = 1;
    }
    s_mode_pending = 1;
    menu_set_mode();
    video_set_palette(g_menu_palette);
    net_reset(&fe_net);
    return 0;
}

/* 0x404740: Yes on RESTORE SETTINGS?. The defaults, keeping how far the
 * player got (0x587), 0x58b and the language. */
int menu_cb_404740(int arg0, int action)
{
    (void)arg0;
    if ((s8)action == MENU_KEY_ACTIVATE) {
        s32 reached = settings_get(0x587);
        s32 flag = settings_get(0x58b);
        s32 lang = settings_get(SET_LANGUAGE);

        settings_defaults();
        settings_set(0x587, reached);
        settings_set(0x58b, flag);
        settings_set(SET_LANGUAGE, lang);
        g_menu.key_pressed[0x01] = 1;
    }
    return 0;
}

/* 0x404790: Yes on RESET GHOST CAR?. Deletes every track's ghost lap. */
int menu_cb_404790(int arg0, int action)
{
    static const char *const ghosts[7] = {
        "GHOSTS\\CAPETHOR.GST", "GHOSTS\\MOOSEJAW.GST", "GHOSTS\\GOLDRUSH.GST",
        "GHOSTS\\SNAKEISL.GST", "GHOSTS\\YODELPEA.GST", "GHOSTS\\TOKYOBUL.GST",
        "GHOSTS\\LOSTRUIN.GST"
    };
    int i;

    (void)arg0;
    if ((s8)action == MENU_KEY_ACTIVATE) {
        for (i = 0; i < 7; i++) {
            file_delete(ghosts[i]);
        }
        g_menu.key_pressed[0x01] = 1;
    }
    return 0;
}

/* 0x404800
 * Turns the menu choices into the race's setup, in the settings: the
 * 25 pages' selections are kept from 0x507, then the race mode, the
 * cars on the grid and their drivers' names are filled in. */
void menu_apply_settings(void)
{
    int i;

    for (i = 0; i < MENU_PAGE_TABLE; i++) {
        settings_set(0x507 + 4 * i, g_frontend_pages[i]->selected);
    }
    /* Two players when page 0 has the second choice and page 2 the first. */
    if (settings_get(0x507) == 1) {
        settings_set(0x004, settings_get(0x50f) ^ 1);
    } else {
        settings_set(0x004, 0);
    }
    /* The race mode, from page 3. */
    if (settings_get(0x513) >= 0 && settings_get(0x513) <= 3) {
        settings_set(0x008, settings_get(0x513));
    }

    /* The grid: the players' cars, then cars 2 to 7. */
    settings_set(0x01c, g_car_order[settings_get(0x040)]);
    if (settings_get(0x004) == 1) {
        settings_set(0x020, g_car_order[settings_get(0x044)]);
    } else {
        settings_set(0x020, 1);
    }
    for (i = 2; i < 8; i++) {
        settings_set(0x01c + 4 * i, i);
    }
    /* Drivers 1 to 7 are named after their cars; player 2 keeps theirs. */
    for (i = 0; i < 7; i++) {
        const char *name = ui_name(0x47e560 + 4 * (u32)settings_get(0x020 + 4 * i));

        memcpy(g_settings + 0x07b + 15 * i, name, strlen(name) + 1);
    }
    if (settings_get(0x004) == 1) {
        const char *name = (const char *)g_settings + SET_PLAYER2_NAME;

        memmove(g_settings + 0x07b, name, strlen(name) + 1);
        settings_set(0x04c, settings_get(0x068));
    } else {
        settings_set(0x04c, 1);
    }
    for (i = 0; i < 6; i++) {
        settings_set(0x050 + 4 * i, 1);
    }

    settings_set(0x15f, fe_net.mode != 2 ? 2 : 0);
    settings_set(0x163, settings_get(0x004) != 1 ? 1 : 0);
    for (i = 0; i < 6; i++) {
        settings_set(0x167 + 4 * i, 1);
    }
    settings_set(0x0f3, settings_get(0x008) == 0 ? 0 : settings_get(0x51f));
    settings_set(0x107, 3 - settings_get(SET_SCREEN_SIZE));
    if (settings_get(0x008) != 2) {
        settings_set(0x153, 6);
    } else {
        settings_set(0x153, settings_get(0x004) == 0 ? 1 : 2);
    }
    for (i = 0; i < 8; i++) {
        settings_set(0x133 + 4 * i, i);
    }
    settings_set(0x197, fe_net.mode);
    settings_set(0x58f, 0);
}
