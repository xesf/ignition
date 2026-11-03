/*
 * test_fx_blend.c - fx_blend_rows on rows of known shape.
 */

#include <stdio.h>
#include <stdlib.h>

#include "fx_blend.h"

static int s_fail;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        s_fail = 1;
    }
}

int main(void)
{
    static u8 table[65536];
    u8 pal[768], out[1024];
    int d, s;

    for (d = 0; d < 256; d++) {
        pal[3 * d] = (u8)d;
        pal[3 * d + 1] = (u8)(255 - d);
        pal[3 * d + 2] = (u8)((d * 7) & 255);
    }
    for (s = 0; s < 256; s++) {
        for (d = 0; d < 256; d++) {
            table[s << 8 | d] = (u8)d;                  /* the identity */
        }
    }
    for (d = 0; d < 256; d++) {
        table[1 << 8 | d] = 77;                         /* one colour */
        table[2 << 8 | d] = (u8)(d / 2);                /* roughly half of d, over black */
    }
    fx_blend_rows(table, pal, out);
    check(out[3] == 0 && out[0] == 0, "identity row is clear");
    check(out[4] == pal[3 * 77] && out[5] == pal[3 * 77 + 1] && out[7] == 255,
          "one colour row is that colour, opaque");
    check(out[8 + 3] > 40 && out[8 + 3] < 215, "a blend row is translucent");
    fx_blend_rows(NULL, pal, out);
    check(out[3] == 0 && out[4 * 5 + 3] == 255 && out[4 * 5] == pal[15], "NULL table is the palette");
    if (!s_fail) {
        printf("test_fx_blend: ok\n");
    }
    return s_fail;
}
