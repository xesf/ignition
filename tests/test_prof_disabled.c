/*
 * test_prof_disabled.c - the profiler header without IGN_PROF.
 *
 * This is a compile-only check, not linked into an engine build: the
 * counter enum must exist and prof_count must be a no-op macro.
 */

#include "prof.h"

int main(void)
{
    prof_count(PROF_COPY_CALLS, 1);
    prof_count(PROF_COPY_PIXELS, 16);
    prof_count(PROF_DRAW_CALLS, 1);
    prof_count(PROF_UPLOAD_BYTES, 64);
    prof_frame();
    return PROF_COUNTERS == 4 ? 0 : 1;
}
