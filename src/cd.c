/*
 * cd.c - CD audio, as the game drives it through MCI (0x457b10..
 * 0x4581b0), played from the disc image.
 *
 * The game asks MCI for the disc's length, its number of tracks and
 * where each track starts, in frames of 1/75 s, and plays a track from
 * its start for its length. Here the answers come from the CUE sheet and
 * the tracks play through the mixer. MCI's CD volume is only on or off;
 * so is this one.
 */

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "cd.h"
#include "cdimage.h"
#include "stubs.h"
#include "vfs.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"

s32 g_cd_present;               /* 0x50ef4c */
s32 g_cd_track_count;           /* 0x50f070 */
/* Track starts from track 1 (0x50f074, index 0 for track 1), and the
 * disc's end after the last; the original stores the starts after its
 * count, so start[t] is at 0x50f070 + 4 t. */
s32 g_cd_start[101];
/* The disc's length, then each track's length from track 1 (0x50f2cc). */
s32 g_cd_tracks[101];

static char s_cue[1024];
static int s_open;
static int s_on = 1;
static int s_playing_track;

/* The track being streamed: its file, read up to the mixer's ring. */
static FILE *s_fp;
static stb_vorbis *s_ogg;       /* or a decoder, for Ogg Vorbis (GOG, Steam) */
static u32 s_left;              /* frames still to read */
static int s_swap;              /* big endian samples */
static struct cd_stream *s_stream;

static void stream_close(void)
{
    if (s_ogg != NULL) {
        stb_vorbis_close(s_ogg);
        s_ogg = NULL;
    }
    if (s_fp != NULL) {
        fclose(s_fp);
        s_fp = NULL;
    }
    s_stream = NULL;
    s_left = 0;
}

/* Tops up the mixer's ring from the track's file; the main loop calls it
 * every pass. */
void cd_pump(void)
{
    enum { CHUNK = 4096 };
    static s16 buf[CHUNK * 2];

    while (s_stream != NULL && s_left > 0) {
        u32 room = audio_cd_room(s_stream), n = s_left < CHUNK ? s_left : CHUNK;
        size_t got;

        if (room < n) {
            return;
        }
        if (s_ogg != NULL) {
            /* Decoded to 16 bit stereo; the track ends where it does. */
            int frames = stb_vorbis_get_samples_short_interleaved(s_ogg, 2, buf, (int)n * 2);

            got = frames > 0 ? (size_t)frames : 0;
            if (got == 0) {
                s_left = 0;
                break;
            }
        } else {
            got = fread(buf, 4, n, s_fp);
        }
        if (s_swap) {
            u8 *b = (u8 *)buf;
            size_t k;

            for (k = 0; k < got * 4; k += 2) {
                u8 t = b[k];

                b[k] = b[k + 1];
                b[k + 1] = t;
            }
        }
        audio_cd_write(s_stream, buf, (u32)got);
        s_left = got < n && s_ogg == NULL ? 0 : s_left - (u32)got;
    }
    if (s_stream != NULL) {
        audio_cd_end(s_stream);
        stream_close();
    }
}

/* Finds a .cue next to the data folder (the image's folder holds the
 * extracted disc as disc/). */
static void find_cue(void)
{
    char dir[1024];
    char *slash;
    DIR *d;
    struct dirent *e;

    if (s_cue[0] != '\0') {
        return;
    }
    /* The GOG and Steam releases keep the sheet (game.ins) in the game's
     * own folder, with its music beside it. */
    snprintf(dir, sizeof(dir), "%s/game.ins", vfs_root());
    if (access(dir, R_OK) == 0) {
        snprintf(s_cue, sizeof(s_cue), "%s", dir);
        return;
    }
    snprintf(dir, sizeof(dir), "%s", vfs_root());
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    d = opendir(dir);
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);

        /* "._" files are macOS's notes on a copied file, not sheets. */
        if (n > 4 && strncmp(e->d_name, "._", 2) != 0
            && (strcasecmp(e->d_name + n - 4, ".cue") == 0
                || strcasecmp(e->d_name + n - 4, ".ins") == 0)) {
            snprintf(s_cue, sizeof(s_cue), "%s/%s", dir, e->d_name);
            break;
        }
    }
    closedir(d);
}

void cd_set_image(const char *cue)
{
    snprintf(s_cue, sizeof(s_cue), "%s", cue);
}

/* 0x458040 */
int cd_open(void)
{
    find_cue();
    if (s_open) {
        return 1;
    }
    if (s_cue[0] == '\0') {
        fprintf(stderr, "cd: no .cue next to %s: no CD music\n", vfs_root());
        return 0;
    }
    if (!cdimage_open(s_cue, vfs_root())) {
        fprintf(stderr, "cd: %s does not open: no CD music\n", s_cue);
        return 0;
    }
    if (!audio_init()) {
        fprintf(stderr, "cd: no audio output\n");
    }
    s_open = 1;
    s_on = 1;
    return 1;
}

/* 0x4580a0 */
int cd_close(void)
{
    stream_close();
    audio_cd_stop();
    cdimage_close();
    s_open = 0;
    return 1;
}

/* 0x457b10 */
int cd_is_playing(void)
{
    return s_open && audio_cd_playing();
}

/* 0x457b50
 * The original also reads where the disc is playing; the game only uses
 * whether a disc is there. */
int cd_read_status(void)
{
    g_cd_present = s_open && cdimage_track_count() > 0;
    return g_cd_present;
}

/* 0x457cf0: 0 switches CD audio off, 1..255 on. */
int cd_set_volume(int volume)
{
    if (volume < 0 || volume > 0xff) {
        return 0;
    }
    s_on = volume != 0;
    audio_cd_mute(!s_on);
    return s_open;
}

/* 0x457da0
 * Track 1 is the data track; as the original does, its start is moved
 * to track 2's, so only audio is ever played. */
int cd_read_tracks(void)
{
    int n, t;

    if (!s_open) {
        return 0;
    }
    n = cdimage_track_count();
    if (n > 100) {
        n = 100;
    }
    g_cd_track_count = n;
    g_cd_tracks[0] = cdimage_disc_frames();
    for (t = 1; t <= n; t++) {
        g_cd_start[t - 1] = cdimage_track_start(cdimage_track_number(t - 1));
    }
    g_cd_start[n] = g_cd_start[0] + g_cd_tracks[0];
    for (t = 1; t <= n; t++) {
        g_cd_tracks[t] = g_cd_start[t] - g_cd_start[t - 1];
    }
    if (n > 1) {
        g_cd_start[0] = g_cd_start[1];
    }
    return 1;
}

/* 0x457ed0 */
int cd_play_track(int track)
{
    const char *path;
    long offset;
    u32 frames;
    int big;

    if (track > g_cd_track_count || track < 1 || !s_open
        || !cdimage_audio_track(track, &path, &offset, &frames, &big)) {
        return 0;
    }
    stream_close();
    {
        size_t len = strlen(path);

        if (len > 4 && strcasecmp(path + len - 4, ".ogg") == 0) {
            int err = 0;
            stb_vorbis_info info;

            s_ogg = stb_vorbis_open_filename(path, &err, NULL);
            if (s_ogg == NULL) {
                fprintf(stderr, "cd: track %d: cannot decode %s (%d)\n", track, path, err);
                return 0;
            }
            info = stb_vorbis_get_info(s_ogg);
            if ((int)info.sample_rate != AUDIO_RATE) {
                fprintf(stderr, "cd: track %d: %u Hz, not %d\n", track, info.sample_rate,
                        AUDIO_RATE);
            }
            offset = 0;
        }
    }
    if (s_ogg == NULL) {
        s_fp = fopen(path, "rb");
    }
    if ((s_ogg == NULL && (s_fp == NULL || fseek(s_fp, offset, SEEK_SET) != 0))) {
        fprintf(stderr, "cd: track %d: cannot read %s\n", track, path);
        stream_close();
        return 0;
    }
    s_stream = audio_cd_open();
    if (s_stream == NULL) {
        fprintf(stderr, "cd: track %d: no audio output\n", track);
        stream_close();
        return 0;
    }
    fprintf(stderr, "cd: track %d, %u:%02u\n", track, frames / AUDIO_RATE / 60,
            frames / AUDIO_RATE % 60);
    s_left = frames;
    s_swap = big;
    s_playing_track = track;
    audio_cd_mute(!s_on);
    cd_pump();
    return 1;
}

/* 0x457fc0 */
int cd_stop_play(void)
{
    stream_close();
    audio_cd_stop();
    s_playing_track = 0;
    return 1;
}

void cd_set_music_volume(int level)
{
    float v = (level < 0 ? 0 : level > 10 ? 10 : level) / 10.0f;

    /* Squared, so that the steps sound even. */
    audio_cd_gain(v * v);
}
