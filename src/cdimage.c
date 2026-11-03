/*
 * cdimage.c - game data and CD audio read from a BIN/CUE disc image.
 *
 * A CUE sheet is text: FILE names a data file (BINARY, MOTOROLA for big
 * endian audio, or WAVE), TRACK gives the number and the sector layout
 * (MODE1/2048, MODE1/2352, MODE2/2352, AUDIO), and INDEX 01 the start of
 * the track as minutes:seconds:frames, 75 frames a second, from the start
 * of its file. A track runs to the next track of the same file or to the
 * end of the file.
 *
 * In a 2352 byte MODE1 sector the 2048 bytes of user data start at 16; in
 * MODE2 form 1 at 24. The file system is ISO 9660: the primary volume
 * descriptor in sector 16, directory records with the extent at +2, the
 * size at +10, the flags at +25 (bit 1 a directory) and the name length
 * at +32, then the name with its ";1" version.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cdimage.h"
#include "compat.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"

enum {
    MAX_FILES = 99,
    MAX_TRACKS = 99,
    MAX_ENTRIES = 4096,
    SECTOR_DATA = 2048,
    SECTOR_RAW = 2352
};

struct cue_file {
    char path[1024];
    int wave;               /* WAVE: samples after the RIFF header */
    int ogg;                /* Ogg Vorbis: size is the decoded 16 bit stereo's */
    int big_endian;         /* MOTOROLA */
    long data_start;        /* where the sectors start */
    long size;
};

struct cue_track {
    int number;
    int file;
    int audio;
    int sector_size;        /* 2048 or 2352 */
    int user_offset;        /* user data within a sector */
    long start;             /* INDEX 01, in sectors from the file start */
    long index0;            /* INDEX 00, or -1 */
    long length;            /* sectors */
};

struct iso_entry {
    char path[256];         /* upper case, '/' separated */
    u32 extent, size;
    int dir;
};

static struct cue_file s_files[MAX_FILES];
static int s_file_count;
static struct cue_track s_tracks[MAX_TRACKS];
static int s_track_count;
static struct iso_entry *s_entries;
static int s_entry_count;
static char s_root[256];        /* the game folder on the disc ("" = root) */
static char s_cache[1024];
static int s_open;
static const struct cue_track *s_data;

/* ---- CUE --------------------------------------------------------------- */

static long msf_to_sectors(const char *msf)
{
    int m = 0, s = 0, f = 0;

    if (sscanf(msf, "%d:%d:%d", &m, &s, &f) != 3) {
        return -1;
    }
    return ((long)m * 60 + s) * 75 + f;
}

static void dir_of(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        snprintf(out, size, ".");
    } else {
        snprintf(out, size, "%.*s", (int)(slash - path), path);
    }
}

/* A RIFF file: the samples start after the "data" chunk header. */
static long wave_data_start(FILE *fp)
{
    u8 header[12], chunk[8];
    long at = 12;

    if (fseek(fp, 0, SEEK_SET) != 0 || fread(header, 1, 12, fp) != 12
        || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return 0;
    }
    while (fseek(fp, at, SEEK_SET) == 0 && fread(chunk, 1, 8, fp) == 8) {
        u32 len = (u32)chunk[4] | ((u32)chunk[5] << 8) | ((u32)chunk[6] << 16)
                | ((u32)chunk[7] << 24);
        if (memcmp(chunk, "data", 4) == 0) {
            return at + 8;
        }
        at += 8 + (long)len + (long)(len & 1);
    }
    return 0;
}

static int add_file(const char *cue_dir, const char *name, const char *type)
{
    struct cue_file *f;
    FILE *fp;

    if (s_file_count >= MAX_FILES) {
        return 0;
    }
    f = &s_files[s_file_count];
    memset(f, 0, sizeof(*f));
    snprintf(f->path, sizeof(f->path), "%s/%s", cue_dir, name);
    /* Sheets from Windows name files with backslashes. */
    {
        char *c;

        for (c = f->path; *c != '\0'; c++) {
            if (*c == '\\') {
                *c = '/';
            }
        }
    }
    f->wave = strcasecmp(type, "WAVE") == 0;
    f->big_endian = strcasecmp(type, "MOTOROLA") == 0;
    {
        size_t n = strlen(f->path);

        f->ogg = n > 4 && strcasecmp(f->path + n - 4, ".ogg") == 0;
    }
    fp = fopen(f->path, "rb");
    if (fp == NULL && strcasecmp(type, "BINARY") == 0) {
        /* The data track's image (the GOG release's game.gog, 400 MB) is
         * not needed when the game's files are there as they are. */
        fprintf(stderr, "cdimage: %s is missing: only its audio tracks\n", f->path);
        s_file_count++;
        return 1;
    }
    if (fp == NULL) {
        fprintf(stderr, "cdimage: %s is missing\n", f->path);
        return 0;
    }
    if (f->ogg) {
        int err = 0;
        stb_vorbis *v;

        fclose(fp);
        v = stb_vorbis_open_filename(f->path, &err, NULL);
        if (v == NULL) {
            fprintf(stderr, "cdimage: %s does not decode (%d)\n", f->path, err);
            return 0;
        }
        /* As 16 bit stereo, the sectors the track would take on a disc. */
        f->size = (long)stb_vorbis_stream_length_in_samples(v) * 4;
        stb_vorbis_close(v);
        s_file_count++;
        return 1;
    }
    if (f->wave) {
        f->data_start = wave_data_start(fp);
    }
    fseek(fp, 0, SEEK_END);
    f->size = ftell(fp) - f->data_start;
    fclose(fp);
    s_file_count++;
    return 1;
}

static int parse_cue(const char *cue_path)
{
    char cue_dir[1024], line[1024];
    FILE *fp = fopen(cue_path, "r");
    int i;

    if (fp == NULL) {
        return 0;
    }
    dir_of(cue_path, cue_dir, sizeof(cue_dir));
    s_file_count = 0;
    s_track_count = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        char word[32];

        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (sscanf(p, "%31s", word) != 1) {
            continue;
        }
        if (strcasecmp(word, "FILE") == 0) {
            char name[512], type[32] = "BINARY";
            char *q = p + 4;

            while (isspace((unsigned char)*q)) {
                q++;
            }
            if (*q == '"') {
                char *end = strchr(q + 1, '"');
                if (end == NULL) {
                    continue;
                }
                snprintf(name, sizeof(name), "%.*s", (int)(end - q - 1), q + 1);
                sscanf(end + 1, "%31s", type);
            } else if (sscanf(q, "%511s %31s", name, type) < 1) {
                continue;
            }
            if (!add_file(cue_dir, name, type)) {
                fclose(fp);
                return 0;
            }
        } else if (strcasecmp(word, "TRACK") == 0 && s_file_count > 0
                   && s_track_count < MAX_TRACKS) {
            struct cue_track *t = &s_tracks[s_track_count];
            char mode[32] = "";

            memset(t, 0, sizeof(*t));
            sscanf(p + 5, "%d %31s", &t->number, mode);
            t->file = s_file_count - 1;
            t->index0 = -1;
            t->start = -1;
            t->audio = strcasecmp(mode, "AUDIO") == 0;
            t->sector_size = strstr(mode, "2048") != NULL ? SECTOR_DATA : SECTOR_RAW;
            t->user_offset = t->sector_size == SECTOR_DATA ? 0
                           : strncasecmp(mode, "MODE2", 5) == 0 ? 24 : 16;
            s_track_count++;
        } else if (strcasecmp(word, "INDEX") == 0 && s_track_count > 0) {
            struct cue_track *t = &s_tracks[s_track_count - 1];
            int index = -1;
            char msf[32] = "";

            sscanf(p + 5, "%d %31s", &index, msf);
            if (index == 1) {
                t->start = msf_to_sectors(msf);
            } else if (index == 0) {
                t->index0 = msf_to_sectors(msf);
            }
        }
    }
    fclose(fp);

    /* Each track runs to the next one in its file, or to the file's end. */
    for (i = 0; i < s_track_count; i++) {
        struct cue_track *t = &s_tracks[i];
        long end = s_files[t->file].size / t->sector_size;

        if (t->start < 0) {
            return 0;
        }
        if (i + 1 < s_track_count && s_tracks[i + 1].file == t->file) {
            const struct cue_track *n = &s_tracks[i + 1];
            end = n->index0 >= 0 ? n->index0 : n->start;
        }
        t->length = end - t->start;
    }
    return s_track_count > 0;
}

/* ---- ISO 9660 ---------------------------------------------------------- */

static FILE *s_sector_fp;

static int read_sector(u32 lba, u8 *out)
{
    const struct cue_file *f = &s_files[s_data->file];
    FILE *fp;
    long at;
    int ok;

    if ((long)lba >= s_data->length) {
        return 0;
    }
    at = f->data_start + (s_data->start + (long)lba) * s_data->sector_size
       + s_data->user_offset;
    /* The data track's file stays open until cdimage_close: extracting
     * the disc reads it a sector at a time. */
    if (s_sector_fp == NULL) {
        s_sector_fp = fopen(f->path, "rb");
    }
    fp = s_sector_fp;
    if (fp == NULL) {
        return 0;
    }
    ok = fseek(fp, at, SEEK_SET) == 0 && fread(out, 1, SECTOR_DATA, fp) == SECTOR_DATA;
    return ok;
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void walk_dir(u32 extent, u32 size, const char *prefix, int depth)
{
    u8 sector[SECTOR_DATA];
    u32 done;

    if (depth > 8) {
        return;
    }
    for (done = 0; done < size; done += SECTOR_DATA) {
        u32 at = 0;

        if (!read_sector(extent + done / SECTOR_DATA, sector)) {
            return;
        }
        while (at < SECTOR_DATA && sector[at] != 0 && s_entry_count < MAX_ENTRIES) {
            const u8 *rec = sector + at;
            int len = rec[0], name_len = rec[32];
            struct iso_entry *e;
            int k;

            if (len < 34 || at + (u32)len > SECTOR_DATA) {
                break;
            }
            at += (u32)len;
            if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) {
                continue;   /* "." and ".." */
            }
            e = &s_entries[s_entry_count++];
            snprintf(e->path, sizeof(e->path), "%s%s", prefix, prefix[0] ? "/" : "");
            k = (int)strlen(e->path);
            for (int j = 0; j < name_len && k < (int)sizeof(e->path) - 1; j++) {
                char c = (char)rec[33 + j];
                if (c == ';') {
                    break;
                }
                e->path[k++] = (char)toupper((unsigned char)c);
            }
            e->path[k] = '\0';
            if (k > 0 && e->path[k - 1] == '.') {
                e->path[k - 1] = '\0';   /* "NAME." for a file without extension */
            }
            e->extent = rd32(rec + 2);
            e->size = rd32(rec + 10);
            e->dir = (rec[25] & 2) != 0;
            if (e->dir) {
                char sub[256];
                snprintf(sub, sizeof(sub), "%s", e->path);
                walk_dir(e->extent, e->size, sub, depth + 1);
            }
        }
    }
}

static const struct iso_entry *find_entry(const char *path)
{
    int i;

    for (i = 0; i < s_entry_count; i++) {
        if (!s_entries[i].dir && strcasecmp(s_entries[i].path, path) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

/* Ignition keeps its files at the disc root. */
static void find_root(void)
{
    s_root[0] = '\0';
}

/* ---- public ------------------------------------------------------------ */

int cdimage_open(const char *cue_path, const char *cache_dir)
{
    u8 pvd[SECTOR_DATA];
    int i;

    cdimage_close();
    if (!parse_cue(cue_path)) {
        return 0;
    }
    for (i = 0; i < s_track_count && s_tracks[i].audio; i++) {
    }
    if (i == s_track_count) {
        return 0;
    }
    s_data = &s_tracks[i];
    s_entries = calloc(MAX_ENTRIES, sizeof(*s_entries));
    if (s_entries == NULL) {
        return 0;
    }
    s_entry_count = 0;
    if (!read_sector(16, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        /* No image of the data (the GOG release's is optional): the audio
         * tracks still play, and the game's files are read as they are. */
        fprintf(stderr, "cdimage: %s has no ISO 9660 data track\n", cue_path);
    } else {
        walk_dir(rd32(pvd + 156 + 2), rd32(pvd + 156 + 10), "", 0);
        find_root();
    }
    snprintf(s_cache, sizeof(s_cache), "%s", cache_dir);
    s_open = 1;
    fprintf(stderr, "cdimage: %s, %d tracks, %d files\n",
            cue_path, s_track_count, s_entry_count);
    return 1;
}

void cdimage_close(void)
{
    if (s_sector_fp != NULL) {
        fclose(s_sector_fp);
        s_sector_fp = NULL;
    }
    free(s_entries);
    s_entries = NULL;
    s_entry_count = 0;
    s_open = 0;
}

int cdimage_is_open(void)
{
    return s_open;
}

static void make_dirs(const char *path)
{
    char tmp[1024];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* A disc path inside the game folder, as it is laid out on disk. */
static const char *local_path(const char *path)
{
    size_t n = strlen(s_root);

    if (n > 0 && strncasecmp(path, s_root, n) == 0 && path[n] == '/') {
        return path + n + 1;
    }
    return path;
}

int cdimage_resolve(const char *name, char *out, size_t out_size)
{
    char path[512];
    const struct iso_entry *e;
    struct stat st;
    FILE *fp;
    u8 sector[SECTOR_DATA];
    u32 done;

    if (!s_open) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s%s%s", s_root, s_root[0] ? "/" : "", name);
    e = find_entry(path);
    if (e == NULL) {
        int i;

        /* A folder: made in the cache. */
        for (i = 0; i < s_entry_count; i++) {
            if (s_entries[i].dir && strcasecmp(s_entries[i].path, path) == 0) {
                snprintf(out, out_size, "%s/%s/", s_cache, local_path(s_entries[i].path));
                make_dirs(out);
                out[strlen(out) - 1] = '\0';
                return 1;
            }
        }
        return 0;
    }
    if (snprintf(out, out_size, "%s/%s", s_cache, local_path(e->path)) >= (int)out_size) {
        return 0;
    }
    if (stat(out, &st) == 0 && (u32)st.st_size == e->size) {
        return 1;
    }
    make_dirs(out);
    fp = fopen(out, "wb");
    if (fp == NULL) {
        return 0;
    }
    for (done = 0; done < e->size; done += SECTOR_DATA) {
        u32 n = e->size - done < SECTOR_DATA ? e->size - done : SECTOR_DATA;
        if (!read_sector(e->extent + done / SECTOR_DATA, sector)
            || fwrite(sector, 1, n, fp) != n) {
            fclose(fp);
            remove(out);
            return 0;
        }
    }
    fclose(fp);
    return 1;
}

int cdimage_audio_track(int track, const char **path, long *offset, u32 *frames,
                        int *big_endian)
{
    const struct cue_track *t = NULL;
    const struct cue_file *f;
    int i;

    for (i = 0; s_open && i < s_track_count; i++) {
        if (s_tracks[i].number == track && s_tracks[i].audio) {
            t = &s_tracks[i];
        }
    }
    if (t == NULL || t->length <= 0) {
        return 0;
    }
    f = &s_files[t->file];
    *path = f->path;
    *offset = f->data_start + t->start * SECTOR_RAW;
    *frames = (u32)((size_t)t->length * SECTOR_RAW / 4);
    *big_endian = f->big_endian;
    return 1;
}

int cdimage_extract_all(void)
{
    char out[1024];
    size_t root_len = strlen(s_root);
    int i, count = 0;

    for (i = 0; s_open && i < s_entry_count; i++) {
        const char *path = s_entries[i].path;

        if (s_entries[i].dir) {
            continue;
        }
        if (root_len > 0) {
            if (strncasecmp(path, s_root, root_len) != 0 || path[root_len] != '/') {
                continue;
            }
            path += root_len + 1;
        }
        if (cdimage_resolve(path, out, sizeof(out))) {
            count++;
        }
    }
    return count;
}

void *cdimage_read_file(const char *name, u32 *size)
{
    char path[512];
    const struct iso_entry *e;
    u8 *data, sector[SECTOR_DATA];
    u32 done;

    if (!s_open) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s%s%s", s_root, s_root[0] ? "/" : "", name);
    e = find_entry(path);
    if (e == NULL || (data = malloc(e->size ? e->size : 1)) == NULL) {
        return NULL;
    }
    for (done = 0; done < e->size; done += SECTOR_DATA) {
        u32 n = e->size - done < SECTOR_DATA ? e->size - done : SECTOR_DATA;
        if (!read_sector(e->extent + done / SECTOR_DATA, sector)) {
            free(data);
            return NULL;
        }
        memcpy(data + done, sector, n);
    }
    *size = e->size;
    return data;
}

int cdimage_track_count(void)
{
    return s_open ? s_track_count : 0;
}

int cdimage_track_number(int i)
{
    return i >= 0 && i < s_track_count ? s_tracks[i].number : 0;
}

int cdimage_track_is_audio(int track)
{
    int i;

    for (i = 0; s_open && i < s_track_count; i++) {
        if (s_tracks[i].number == track) {
            return s_tracks[i].audio;
        }
    }
    return 0;
}

/* A file's sectors come after all the earlier files' sectors. */
static long file_base(int file)
{
    long base = 0;
    int f;

    for (f = 0; f < file; f++) {
        int i;
        int size = SECTOR_RAW;

        for (i = 0; i < s_track_count; i++) {
            if (s_tracks[i].file == f) {
                size = s_tracks[i].sector_size;
                break;
            }
        }
        base += s_files[f].size / size;
    }
    return base;
}

s32 cdimage_track_start(int track)
{
    int i;

    for (i = 0; s_open && i < s_track_count; i++) {
        if (s_tracks[i].number == track) {
            return (s32)(file_base(s_tracks[i].file) + s_tracks[i].start);
        }
    }
    return 0;
}

s32 cdimage_disc_frames(void)
{
    if (!s_open || s_track_count == 0) {
        return 0;
    }
    return (s32)file_base(s_file_count);
}
