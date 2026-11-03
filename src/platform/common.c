/*
 * platform/common.c - host code shared by every host but the Vita's.
 * See platform_common.h.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cdimage.h"
#include "menu/engine.h"
#include "compat.h"
#include "dik.h"
#include "game.h"
#include "platform/platform.h"
#include "platform/common.h"
#include "race/race.h"
#include "race/init.h"
#include "vfs.h"

extern s32 g_race_5285c8;       /* game.c: the race's Y/N question is up */

u8 g_plat_keys[256];
int g_plat_run_inactive;

/* ------------------------------------------------------------ keys */

enum { KEY_QUEUE = 64 };

static struct { u8 key, down; } s_key_queue[KEY_QUEUE];
static unsigned s_key_head, s_key_tail;

/* A full queue drops the event, as DirectInput's buffer overflows. */
void plat_key_event(int dik, int down)
{
    if (s_key_head - s_key_tail < KEY_QUEUE) {
        s_key_queue[s_key_head % KEY_QUEUE].key = (u8)dik;
        s_key_queue[s_key_head % KEY_QUEUE].down = (u8)down;
        s_key_head++;
    }
    g_plat_keys[dik] = down ? 0x80 : 0;
}

int plat_next_key_event(int *key, int *down)
{
    if (s_key_tail == s_key_head) {
        return 0;
    }
    *key = s_key_queue[s_key_tail % KEY_QUEUE].key;
    *down = s_key_queue[s_key_tail % KEY_QUEUE].down;
    s_key_tail++;
    return 1;
}

enum { SCHEDULED = 1024 };

static struct { u32 ms, hold; int key; int state; } s_scheduled[SCHEDULED];
static int s_scheduled_count;

void plat_schedule_key(u32 ms, int key, u32 hold)
{
    if (s_scheduled_count < SCHEDULED) {
        s_scheduled[s_scheduled_count].ms = ms;
        s_scheduled[s_scheduled_count].key = key;
        s_scheduled[s_scheduled_count].hold = hold;
        s_scheduled[s_scheduled_count].state = 0;
        s_scheduled_count++;
    }
}

static struct { u32 ms; float fx, fy; } s_taps[64];
static int s_tap_count;

void plat_schedule_tap(u32 ms, float fx, float fy)
{
    if (s_tap_count < 64) {
        s_taps[s_tap_count].ms = ms;
        s_taps[s_tap_count].fx = fx;
        s_taps[s_tap_count].fy = fy;
        s_tap_count++;
    }
}

/* A scheduled key goes down when its time comes and up `hold` ms later. */
void plat_run_scheduled(void)
{
    u32 now = plat_time_ms();
    int i, n;

    for (i = 0; i < s_scheduled_count; i++) {
        if (s_scheduled[i].state == 0 && now >= s_scheduled[i].ms) {
            plat_key_event(s_scheduled[i].key, 1);
            s_scheduled[i].state = 1;
        } else if (s_scheduled[i].state == 1 && now >= s_scheduled[i].ms + s_scheduled[i].hold) {
            plat_key_event(s_scheduled[i].key, 0);
            s_scheduled[i].state = 2;
        }
    }
    for (i = 0, n = 0; i < s_scheduled_count; i++) {
        if (s_scheduled[i].state != 2) {
            s_scheduled[n++] = s_scheduled[i];
        }
    }
    s_scheduled_count = n;
    for (i = 0, n = 0; i < s_tap_count; i++) {
        if (now >= s_taps[i].ms) {
            menu_tap(s_taps[i].fx, s_taps[i].fy);
        } else {
            s_taps[n++] = s_taps[i];
        }
    }
    s_tap_count = n;
}

/* ------------------------------------------------------------ gamepad */

static u8 s_pad_keys[256];

static void want(u8 *keys, int on, int dik)
{
    if (on && dik > 0 && dik < 256) {
        keys[dik] = 1;
    }
}

/* The race's Y key in its language (race/misc.c, yes_key). */
static int yes_key(void)
{
    static const u8 keys[6] = { DIK_Y, DIK_J, DIK_S, DIK_S, DIK_J, DIK_O };
    s32 lang = race_cfg(0x593);

    return lang >= 0 && lang < 6 ? keys[lang] : DIK_Y;
}

/* The keys the buttons press now. The same layout as the Vita's. */
static void pad_keys(u32 b, u8 *keys)
{
    int left = (b & PAD_LEFT) != 0, right = (b & PAD_RIGHT) != 0;
    int up = (b & PAD_UP) != 0, down = (b & PAD_DOWN) != 0;
    int a = (b & PAD_A) != 0, bb = (b & PAD_B) != 0;
    int x = (b & PAD_X) != 0, y = (b & PAD_Y) != 0;
    int l = (b & PAD_L) != 0, r = (b & PAD_R) != 0;
    int start = (b & PAD_START) != 0, select = (b & PAD_SELECT) != 0;

    memset(keys, 0, 256);
    if (g_race_running != 1) {
        want(keys, left, DIK_LEFT);
        want(keys, right, DIK_RIGHT);
        want(keys, up, DIK_UP);
        want(keys, down, DIK_DOWN);
        want(keys, a || start, DIK_RETURN);
        want(keys, bb || select, DIK_ESCAPE);
        return;
    }
    {
        /* Player 1: left, right, accelerate, brake, gear up, gear down,
         * action, look back. */
        const u8 *k = g_race_cfg + 0x17f;

        want(keys, up, DIK_UP);
        want(keys, down, DIK_DOWN);
        want(keys, start, DIK_ESCAPE);
        want(keys, select, DIK_RETURN);
        if (g_race_5285c8 == 1) {
            want(keys, a, yes_key());
            want(keys, bb, DIK_N);
        } else if (r_494990 != 0) {
            want(keys, left, DIK_LEFT);
            want(keys, right, DIK_RIGHT);
            want(keys, a, DIK_RETURN);
            want(keys, bb, DIK_ESCAPE);
        } else {
            want(keys, left, k[0]);
            want(keys, right, k[1]);
            want(keys, a, k[2]);
            want(keys, x, k[3]);
            want(keys, r, k[4]);
            want(keys, l, k[5]);
            want(keys, bb, k[6]);
            want(keys, y, k[7]);
        }
    }
}

void plat_pad_update(u32 buttons)
{
    u8 keys[256];
    int i;

    pad_keys(buttons, keys);
    for (i = 1; i < 256; i++) {
        if (keys[i] != s_pad_keys[i]) {
            s_pad_keys[i] = keys[i];
            plat_key_event(i, keys[i]);
        }
    }
}

/* ------------------------------------------------------------ data */

static int is_dir(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The extracted disc has the game when the menu palette is there. */
static int has_game(const char *dir)
{
    char path[1024];
    FILE *fp;

    if (!is_dir(dir)) {
        return 0;
    }
    vfs_set_root(dir);
    if (!vfs_resolve("baltazar/data/menu.col", path, sizeof(path))) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (fp != NULL) {
        fclose(fp);
    }
    return fp != NULL;
}

/* The first .cue sheet in `dir`, into `out`. */
static int find_cue(const char *dir, char *out, size_t size)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (d == NULL) {
        return 0;
    }
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);

        if (n > 4 && strncmp(e->d_name, "._", 2) != 0
            && strcasecmp(e->d_name + n - 4, ".cue") == 0) {
            snprintf(out, size, "%s/%s", dir, e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* `base`/disc, extracted from a disc image in `base` when needed. */
static int try_base(const char *base, char *out, size_t size)
{
    char cue[1024];

    if (base == NULL || base[0] == '\0') {
        return 0;
    }
    snprintf(out, size, "%s/disc", base);
    if (has_game(out)) {
        return 1;
    }
    if (!find_cue(base, cue, sizeof(cue))) {
        return 0;
    }
    fprintf(stderr, "ignition: extracting %s into %s\n", cue, out);
    if (!is_dir(out)) {
        mkdir(out, 0755);
    }
    if (!cdimage_open(cue, out)) {
        return 0;
    }
    cdimage_extract_all();
    cdimage_close();
    return has_game(out);
}

/* The first folder (in name order) with a disc/ in it, under data/ or
 * one folder down (data/images-disks/<image>/disc): the repository's
 * layout, whatever the disc image's folder is called. */
static void best_disc(const char *dir, int depth, char *best, size_t size)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];

    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (!is_dir(path)) {
            continue;
        }
        {
            char disc[1100];

            snprintf(disc, sizeof(disc), "%s/disc", path);
            if (is_dir(disc)) {
                /* By the image's folder, so that "X" comes before "X (Y)". */
                size_t bl = strlen(best);

                if (best[0] == '\0' || strncmp(path, best, bl > 5 ? bl - 5 : 0) < 0
                    || (strncmp(path, best, bl > 5 ? bl - 5 : 0) == 0
                        && strlen(path) < bl - 5)) {
                    snprintf(best, size, "%s", disc);
                }
                continue;
            }
        }
        if (depth > 0) {
            best_disc(path, depth - 1, best, size);
        }
    }
    closedir(d);
}

static int find_in_repo_data(char *out, size_t n)
{
    char best[1024] = "";

    /* The GOG and Steam releases, copied into data/ as they are. */
    if (has_game("data")) {
        snprintf(out, n, "data");
        return 1;
    }
    best_disc("data", 1, best, sizeof(best));
    if (best[0] == '\0') {
        return 0;
    }
    snprintf(out, n, "%s", best);
    return 1;
}

const char *plat_find_data(const char *fallback)
{
    static char found[1024];
    const char *env = getenv("IGNITION_DATA");
    const char *bases[2];
    int i;

    if (env != NULL && env[0] != '\0') {
        return env;
    }
    if (is_dir(fallback)) {
        return fallback;
    }
    if (find_in_repo_data(found, sizeof(found))) {
        return found;
    }
    bases[0] = plat_host_exe_dir();
    bases[1] = plat_host_pref_dir();
    for (i = 0; i < 2; i++) {
        if (try_base(bases[i], found, sizeof(found))) {
            return found;
        }
    }
    return fallback;
}
