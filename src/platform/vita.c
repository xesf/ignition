/*
 * platform/vita.c - the PlayStation Vita host: the display, the
 * buttons as DirectInput scan codes, the clock and audio.
 *
 * VitaSDK only. The renderer above this is the same as on macOS, on
 * GXM instead of OpenGL (rnd_gxm.c, which owns the display). The
 * screen is 960 x 544, which the NATIVE resolution follows.
 *
 * The game reads a keyboard, so the buttons press keys. Which keys
 * depends on where the game is:
 *
 *   menus   d-pad / left stick  arrows       Cross, Start   Return
 *           Circle, Select      Escape
 *   race    left / right        steer (player 1's keys, g_race_cfg+0x17f)
 *           d-pad up / down     gear up / gear down
 *           R      accelerate   L         brake
 *           Cross  action (Return on the results)   Triangle  look back
 *           Start  Escape (pause)          Select    Return (go on)
 *   paused  Cross  Return        Circle    Escape (back to the race)
 *   Y/N box Cross  Yes           Circle    No
 *
 * The keys follow the player's settings, so a remapped keyboard layout
 * still drives the car. Log output goes to ux0:data/ignition/log.txt.
 */

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <psp2/sysmodule.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>

#include "app.h"
#include "dik.h"
#include "game.h"
#include "menu/engine.h"
#include "platform/platform.h"
#include "race/init.h"
#include "race/race.h"
#include "rnd.h"
#include "video.h"

/* newlib's heap: the game's pools and the decoded data. rnd_gxm.c
 * takes GPU memory from what is left. */
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define USER_DIR "ux0:data/ignition"

struct _reent *__getreent(void);   /* newlib: this thread's streams */

enum { SCREEN_W = 960, SCREEN_H = 544 };

u8 g_plat_keys[256];
int g_plat_run_inactive;

extern s32 g_race_5285c8;       /* game.c: the race's Y/N question is up */

enum { KEY_QUEUE = 64 };

static struct { u8 key, down; } s_key_queue[KEY_QUEUE];
static unsigned s_key_head, s_key_tail;
static u8 s_pad_keys[256];      /* the keys the buttons hold down */
static int s_quit;
static int s_display_ready;
static u64 s_start;

/* ------------------------------------------------------------ log */

/* No console: stdout and stderr go to files, line buffered and
 * unbuffered so a crash keeps what came before it. newlib keeps the
 * standard streams per thread and globally; both are replaced. Runs
 * before main. */
__attribute__((constructor)) static void open_log(void)
{
    static FILE *out, *err;
    struct _reent *r = __getreent();

    sceIoMkdir(USER_DIR, 0777);
    out = fopen(USER_DIR "/log.txt", "w");
    err = fopen(USER_DIR "/err.txt", "w");
    if (out != NULL) {
        setvbuf(out, NULL, _IOLBF, 1024);
        r->_stdout = out;
        _impure_ptr->_stdout = out;
    }
    if (err != NULL) {
        setvbuf(err, NULL, _IONBF, 0);
        r->_stderr = err;
        _impure_ptr->_stderr = err;
    }
}

/* ------------------------------------------------------------ clock */

u32 plat_time_ms(void)
{
    if (s_start == 0) {
        s_start = sceKernelGetProcessTimeWide();
    }
    return (u32)((sceKernelGetProcessTimeWide() - s_start) / 1000u);
}

/* ------------------------------------------------------------ display */

/* The screen is the window: nothing to size. */
void plat_window_size(int width, int height, int *old_width, int *old_height)
{
    (void)width;
    (void)height;
    *old_width = *old_height = 0;
}

int plat_window_open(const char *title, int width, int height, int fullscreen)
{
    (void)title;
    (void)width;
    (void)height;
    (void)fullscreen;
    if (s_display_ready) {
        return 1;
    }
    /* Full clocks: CPU 444 MHz, bus and GPU 222, crossbar 166. The
     * display itself is GXM's, started by gfx_init (rnd_gxm.c). */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    s_display_ready = 1;
    /* The 3D at 720 x 408: a scene's cost goes with its pixels, and the
     * race holds its 36 frames a second more often. */
    g_view_scale = 0.75f;
    return 1;
}

void plat_destroy_window(void)
{
}

void plat_drawable_size(int *width, int *height)
{
    *width = SCREEN_W;
    *height = SCREEN_H;
}

/* Queues the frame for the display, which waits for the vertical
 * blank. */
void plat_present(void)
{
    rnd_present();
}

/* ------------------------------------------------------------ keys */

/* A full queue drops the event, as DirectInput's buffer overflows. */
static void key_event(int dik, int down)
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

enum { SCHEDULED = 256 };

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

static void run_scheduled(void)
{
    u32 now = plat_time_ms();
    int i, n;

    for (i = 0; i < s_scheduled_count; i++) {
        if (s_scheduled[i].state == 0 && now >= s_scheduled[i].ms) {
            key_event(s_scheduled[i].key, 1);
            s_scheduled[i].state = 1;
        } else if (s_scheduled[i].state == 1 && now >= s_scheduled[i].ms + s_scheduled[i].hold) {
            key_event(s_scheduled[i].key, 0);
            s_scheduled[i].state = 2;
        }
    }
    /* Done presses go, so that taps can keep adding them. */
    for (i = 0, n = 0; i < s_scheduled_count; i++) {
        if (s_scheduled[i].state != 2) {
            s_scheduled[n++] = s_scheduled[i];
        }
    }
    s_scheduled_count = n;
}

/* ------------------------------------------------------------ buttons */

enum { STICK_PUSH = 64 };       /* of 128 off the middle */

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

/* The keys the buttons and the left stick press now. */
static void pad_keys(u32 b, int lx, int ly, u8 *keys)
{
    int left = (b & SCE_CTRL_LEFT) || lx < 128 - STICK_PUSH;
    int right = (b & SCE_CTRL_RIGHT) || lx > 128 + STICK_PUSH;
    int up = (b & SCE_CTRL_UP) || ly < 128 - STICK_PUSH;
    int down = (b & SCE_CTRL_DOWN) || ly > 128 + STICK_PUSH;
    int cross = (b & SCE_CTRL_CROSS) != 0, circle = (b & SCE_CTRL_CIRCLE) != 0;
    int triangle = (b & SCE_CTRL_TRIANGLE) != 0;
    int l = (b & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)) != 0;
    int r = (b & (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)) != 0;
    int start = (b & SCE_CTRL_START) != 0, select = (b & SCE_CTRL_SELECT) != 0;

    memset(keys, 0, 256);
    if (g_race_running != 1) {
        want(keys, left, DIK_LEFT);
        want(keys, right, DIK_RIGHT);
        want(keys, up, DIK_UP);
        want(keys, down, DIK_DOWN);
        want(keys, cross || start, DIK_RETURN);
        want(keys, circle || select, DIK_ESCAPE);
        return;
    }
    {
        /* Player 1: left, right, accelerate, brake, gear up, gear down,
         * action, look back. */
        const u8 *k = g_race_cfg + 0x17f;
        int pad_up = (b & SCE_CTRL_UP) != 0, pad_down = (b & SCE_CTRL_DOWN) != 0;

        want(keys, start, DIK_ESCAPE);
        want(keys, select, DIK_RETURN);
        if (g_race_5285c8 == 1) {
            want(keys, cross, yes_key());
            want(keys, circle, DIK_N);
        } else if (r_494990 != 0) {
            /* The pause menu and the boxes. */
            want(keys, up, DIK_UP);
            want(keys, down, DIK_DOWN);
            want(keys, left, DIK_LEFT);
            want(keys, right, DIK_RIGHT);
            want(keys, cross, DIK_RETURN);
            want(keys, circle, DIK_ESCAPE);
        } else {
            /* Steering on the d-pad or the left stick, the triggers for
             * the pedals, the d-pad's up and down for the gears. */
            want(keys, left, k[0]);
            want(keys, right, k[1]);
            want(keys, r, k[2]);
            want(keys, l, k[3]);
            want(keys, pad_up, k[4]);
            want(keys, pad_down, k[5]);
            /* On the results Cross goes on, as Return does. */
            want(keys, cross, r_5530a8 > 0 ? DIK_RETURN : k[6]);
            want(keys, triangle, k[7]);
        }
    }
}

static void poll_pad(void)
{
    SceCtrlData c;
    u8 keys[256];
    int i;

    memset(&c, 0, sizeof(c));
    c.lx = c.ly = 128;
    if (sceCtrlPeekBufferPositiveExt2(0, &c, 1) < 0) {
        return;
    }
    pad_keys(c.buttons, c.lx, c.ly, keys);
    {
        /* L and R together (a press, not held): a screenshot. */
        static int held;
        int both = (c.buttons & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)) != 0
                   && (c.buttons & (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)) != 0;

        if (both && !held) {
            video_request_shot();
        }
        held = both;
    }
    g_plat_sticks[0].present = 1;
    g_plat_sticks[0].x = (c.lx - 128) * 100 / 127;
    g_plat_sticks[0].y = (c.ly - 128) * 100 / 127;
    g_plat_sticks[0].buttons[0] = (c.buttons & (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)) != 0;
    g_plat_sticks[0].buttons[1] = (c.buttons & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)) != 0;
    for (i = 1; i < 256; i++) {
        if (keys[i] != s_pad_keys[i]) {
            s_pad_keys[i] = keys[i];
            key_event(i, keys[i]);
        }
    }
}

/* ------------------------------------------------------------ touch */

/* A tap: a touch let go within TAP_MS, having moved less than TAP_MOVE
 * (in the screen panel's units, 1920 x 1088). */
enum { TAP_MS = 400, TAP_MOVE = 60 };

static struct tap {
    int down;
    int x, y;
    u32 since;
} s_taps[1];

/* Whether port `port` was tapped since the last call, and where. */
static int tapped(int port, int *x, int *y)
{
    SceTouchData d;
    struct tap *t = &s_taps[port];

    memset(&d, 0, sizeof(d));
    if (sceTouchPeek(port, &d, 1) < 0) {
        return 0;
    }
    if (d.reportNum > 0) {
        if (!t->down) {
            t->down = 1;
            t->x = d.report[0].x;
            t->y = d.report[0].y;
            t->since = plat_time_ms();
        } else if (abs(d.report[0].x - t->x) > TAP_MOVE || abs(d.report[0].y - t->y) > TAP_MOVE) {
            t->since = 0;           /* a drag, not a tap */
        }
        return 0;
    }
    if (!t->down) {
        return 0;
    }
    t->down = 0;
    *x = t->x;
    *y = t->y;
    return t->since != 0 && plat_time_ms() - t->since <= TAP_MS;
}

/* In the menus a tap on the screen picks an item, as menu_touch says,
 * with the keys a player would press. */
static void poll_touch(void)
{
    int x, y;

    if (tapped(SCE_TOUCH_PORT_FRONT, &x, &y) && g_race_running != 1) {
        menu_tap(x / 1920.0f, y / 1088.0f);
    }
}

void plat_pump_events(void)
{
    run_scheduled();
    poll_pad();
    poll_touch();
}

/* ------------------------------------------------------------ audio */

enum { AUDIO_FRAMES = 512 };    /* a call's samples, a multiple of 64 */

static plat_audio_fn s_audio_cb;
static void *s_audio_user;
static int s_audio_port = -1;
static volatile int s_audio_run;
static SceUID s_audio_thread = -1;

static int audio_thread(SceSize args, void *argp)
{
    static float mix[AUDIO_FRAMES * 2];
    static s16 out[2][AUDIO_FRAMES * 2];
    int which = 0, i;

    (void)args;
    (void)argp;
    while (s_audio_run) {
        s16 *o = out[which];

        if (s_audio_cb != NULL) {
            s_audio_cb(mix, AUDIO_FRAMES, s_audio_user);
        } else {
            memset(mix, 0, sizeof(mix));
        }
        for (i = 0; i < AUDIO_FRAMES * 2; i++) {
            float v = mix[i] * 32767.0f;

            o[i] = (s16)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
        }
        sceAudioOutOutput(s_audio_port, o);     /* waits for the port */
        which ^= 1;
    }
    return 0;
}

/* The BGM port, which takes the mixer's rate (the main port is 48 kHz
 * only). */
int plat_audio_open(s32 rate, plat_audio_fn cb, void *user)
{
    int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };

    s_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_FRAMES, rate,
                                       SCE_AUDIO_OUT_MODE_STEREO);
    if (s_audio_port < 0) {
        fprintf(stderr, "vita: no audio port (0x%08x)\n", (unsigned)s_audio_port);
        s_audio_port = -1;
        return 0;
    }
    sceAudioOutSetVolume(s_audio_port,
                         (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH
                                                  | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    s_audio_cb = cb;
    s_audio_user = user;
    s_audio_run = 1;
    /* A high priority, so a long frame does not starve it. */
    s_audio_thread = sceKernelCreateThread("ignition audio", audio_thread, 64, 64 * 1024, 0, 0,
                                           NULL);
    if (s_audio_thread < 0 || sceKernelStartThread(s_audio_thread, 0, NULL) < 0) {
        s_audio_run = 0;
        sceAudioOutReleasePort(s_audio_port);
        s_audio_port = -1;
        return 0;
    }
    return 1;
}

void plat_audio_close(void)
{
    if (s_audio_thread >= 0) {
        s_audio_run = 0;
        sceKernelWaitThreadEnd(s_audio_thread, NULL, NULL);
        sceKernelDeleteThread(s_audio_thread);
        s_audio_thread = -1;
    }
    if (s_audio_port >= 0) {
        sceAudioOutReleasePort(s_audio_port);
        s_audio_port = -1;
    }
    s_audio_cb = NULL;
}

/* ------------------------------------------------------------ loop */

void plat_request_quit(void)
{
    s_quit = 1;
}

/* The game is always the active app: the system suspends it when it is
 * not. */
static int run(int (*frame)(void *), void *user)
{
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    while (!s_quit) {
        g_app_active = 1;
        plat_pump_events();
        if (!frame(user)) {
            plat_request_quit();
        }
    }
    fflush(stdout);
    fflush(stderr);
    return 0;
}

static int tick(void *user)
{
    (void)user;
    return app_tick();
}

int plat_run(void)
{
    return run(tick, NULL);
}

int plat_run_with(int (*frame)(void *user), void *user)
{
    return run(frame, user);
}

/* ------------------------------------------------------------ network */

/* UDP for network play on sceNet, the Vita's BSD-like sockets. The
 * net module and its memory are set up the first time. */
static int s_net_sock = -1;

static int net_init(void)
{
    static int s_state;                 /* 0 not tried, 1 up, -1 failed */
    static u8 s_net_memory[256 * 1024];
    SceNetInitParam param;

    if (s_state != 0) {
        return s_state > 0;
    }
    s_state = -1;
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
        return 0;
    }
    param.memory = s_net_memory;
    param.size = sizeof(s_net_memory);
    param.flags = 0;
    sceNetInit(&param);                 /* fails harmlessly when already up */
    s_state = 1;
    return 1;
}

static void to_sockaddr(const struct plat_net_addr *a, SceNetSockaddrIn *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_len = sizeof(*sa);
    sa->sin_family = SCE_NET_AF_INET;
    sa->sin_addr.s_addr = sceNetHtonl(a->ip);
    sa->sin_port = sceNetHtons(a->port);
}

int plat_net_open(u16 port)
{
    struct plat_net_addr any = { 0, 0 };
    SceNetSockaddrIn sa;
    int on = 1;

    plat_net_close();
    if (!net_init()) {
        return 0;
    }
    s_net_sock = sceNetSocket("ignition", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
    if (s_net_sock < 0) {
        s_net_sock = -1;
        return 0;
    }
    sceNetSetsockopt(s_net_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &on, sizeof(on));
    sceNetSetsockopt(s_net_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
    any.port = port;
    to_sockaddr(&any, &sa);
    if (sceNetBind(s_net_sock, (const SceNetSockaddr *)&sa, sizeof(sa)) < 0) {
        plat_net_close();
        return 0;
    }
    return 1;
}

void plat_net_close(void)
{
    if (s_net_sock >= 0) {
        sceNetSocketClose(s_net_sock);
        s_net_sock = -1;
    }
}

int plat_net_send(const struct plat_net_addr *to, const void *buf, int size)
{
    SceNetSockaddrIn sa;

    if (s_net_sock < 0) {
        return 0;
    }
    to_sockaddr(to, &sa);
    return sceNetSendto(s_net_sock, buf, (unsigned)size, 0, (const SceNetSockaddr *)&sa,
                        sizeof(sa)) == size;
}

int plat_net_recv(struct plat_net_addr *from, void *buf, int size)
{
    SceNetSockaddrIn sa;
    unsigned int len = sizeof(sa);
    int n;

    if (s_net_sock < 0) {
        return -1;
    }
    n = sceNetRecvfrom(s_net_sock, buf, (unsigned)size, 0, (SceNetSockaddr *)&sa, &len);
    if (n < 0) {
        return n == (int)SCE_NET_ERROR_EAGAIN ? 0 : -1;
    }
    from->ip = sceNetNtohl(sa.sin_addr.s_addr);
    from->port = sceNetNtohs(sa.sin_port);
    return n;
}

/* No select here: the socket does not block, and a short sleep does. */
void plat_net_wait(u32 ms)
{
    sceKernelDelayThread(ms * 1000);
}

/* Dotted addresses only; the Vita's resolver is not set up. */
int plat_net_resolve(const char *name, u32 *ip)
{
    unsigned int addr;

    if (strcmp(name, "localhost") == 0) {
        *ip = PLAT_NET_LOOPBACK;
        return 1;
    }
    if (sceNetInetPton(SCE_NET_AF_INET, name, &addr) <= 0) {
        return 0;
    }
    *ip = sceNetNtohl(addr);
    return 1;
}
