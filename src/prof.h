/*
 * prof.h - time spent in parts of a frame, logged every two seconds
 * when the build defines IGN_PROF (the Vita's does), to find where a
 * slow frame goes on a console.
 */

#ifndef IGN_PROF_H
#define IGN_PROF_H

enum {
    PROF_PHYSICS, PROF_CARS, PROF_PRESENT, PROF_TICKS, PROF_GATHER, PROF_3D, PROF_HUD,
    PROF_MESHES, PROF_DYNS, PROF_VIEW_END, PROF_VIEW, PROF_LAYERS, PROF_SORT,
    PROF_RESULTS, PROF_ROWS, PROF_P_SCENE, PROF_P_END, PROF_P_PAD, PROF_P_QUEUE, PROF_P_SLOT,
    PROF_P_GFX, PROF_G_SYNC, PROF_G_BEGIN, PROF_G_LAYERS, PROF_H_PAL, PROF_H_3D, PROF_H_TEX, PROF_H_OVER, PROF_H_SNAP, PROF_T_FIN, PROF_T_RING, PROF_T_ALLOC, PROF_T_FILL, PROF_D_DYNS, PROF_D_PART, PROF_PARTS
};

#ifdef IGN_PROF
void prof_begin(int part);
void prof_end(int part);
/* Once a frame. */
void prof_frame(void);
#else
#define prof_begin(part) ((void)0)
#define prof_end(part) ((void)0)
#define prof_frame() ((void)0)
#endif

#endif /* IGN_PROF_H */
