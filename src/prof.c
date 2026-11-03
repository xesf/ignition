/*
 * prof.c - see prof.h.
 */

#include "prof.h"

#ifdef IGN_PROF

#include <stdio.h>
#include <time.h>

static const char *const s_names[PROF_PARTS] = {
    "physics", "cars", "present", "ticks", "gather", "3d", "hud", "3d-meshes", "3d-faces",
    "3d-end", "view", "3d-layers", "3d-sort", "results", "rows", "p-scene", "p-end", "p-pad", "p-queue", "p-slot", "p-gfx", "g-sync", "g-begin", "g-layers", "h-pal", "h-3d", "h-tex", "h-over", "h-snap", "t-fin", "t-ring", "t-alloc", "t-fill", "d-dyns", "d-part"
};

static double s_start[PROF_PARTS], s_total[PROF_PARTS], s_since;
static int s_frames;

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

void prof_begin(int part)
{
    s_start[part] = now_ms();
}

void prof_end(int part)
{
    s_total[part] += now_ms() - s_start[part];
}

void prof_frame(void)
{
    double t = now_ms();
    int i;

    s_frames++;
    if (s_since == 0.0) {
        s_since = t;
    } else if (t - s_since >= 2000.0) {
        printf("prof: a frame %.1f ms:", (t - s_since) / s_frames);
        for (i = 0; i < PROF_PARTS; i++) {
            printf(" %s %.1f", s_names[i], s_total[i] / s_frames);
            s_total[i] = 0.0;
        }
        printf("\n");
        fflush(stdout);
        s_frames = 0;
        s_since = t;
    }
}

#else
typedef int prof_unused;
#endif
