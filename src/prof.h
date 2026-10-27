/*
 * prof.h - time spent in parts of a frame, logged every two seconds
 * when the build defines IGN_PROF (the Vita's does), to find where a
 * slow frame goes on a console.
 */

#ifndef IGN_PROF_H
#define IGN_PROF_H

#include <stdint.h>

enum {
    PROF_PHYSICS, PROF_CARS, PROF_PRESENT, PROF_TICKS, PROF_GATHER, PROF_3D, PROF_HUD,
    PROF_MESHES, PROF_DYNS, PROF_VIEW_END, PROF_VIEW, PROF_LAYERS, PROF_SORT,
    PROF_RESULTS, PROF_ROWS, PROF_PARTS
};

enum {
    PROF_COPY_CALLS, PROF_COPY_PIXELS, PROF_DRAW_CALLS, PROF_UPLOAD_BYTES, PROF_COUNTERS
};

#ifdef IGN_PROF
void prof_begin(int part);
void prof_end(int part);
/* Once a frame. */
void prof_frame(void);
void prof_count(int counter, uint64_t amount);
#else
#define prof_begin(part) ((void)0)
#define prof_end(part) ((void)0)
#define prof_frame() ((void)0)
#define prof_count(counter, amount) ((void)0)
#endif

#endif /* IGN_PROF_H */
