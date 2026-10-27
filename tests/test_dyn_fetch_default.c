/*
 * rnd.h's capability query is compile-time false away from GLES, so the
 * GXM and other non-GLES renderers need no new backend symbol.
 */

#include <stdio.h>

#include "rnd.h"

int main(void)
{
    if (rnd_dyn_framebuffer_fetch() != 0) {
        fprintf(stderr, "FAIL: non-GLES rnd_dyn_framebuffer_fetch() is nonzero\n");
        return 1;
    }
    puts("ok");
    return 0;
}
