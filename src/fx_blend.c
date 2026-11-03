/*
 * fx_blend.c - a colour table as the GPU's blending (fx_blend.h).
 */

#include <math.h>

#include "fx_blend.h"

static u8 clamp8(double v)
{
    return (u8)(v < 0.0 ? 0.0 : v > 255.0 ? 255.0 : v + 0.5);
}

void fx_blend_rows(const u8 *table, const u8 *palette, u8 out[256 * 4])
{
    double mean[3] = { 0, 0, 0 }, var = 0;
    int s, d, c;

    for (d = 0; d < 256; d++) {
        for (c = 0; c < 3; c++) {
            mean[c] += palette[3 * d + c] / 256.0;
        }
    }
    for (d = 0; d < 256; d++) {
        for (c = 0; c < 3; c++) {
            double p = palette[3 * d + c] - mean[c];

            var += p * p;
        }
    }
    for (s = 0; s < 256; s++) {
        u8 *o = out + 4 * s;

        if (table == NULL) {
            for (c = 0; c < 3; c++) {
                o[c] = s == 0 ? 0 : palette[3 * s + c];
            }
            o[3] = s == 0 ? 0 : 255;
            continue;
        }
        {
            const u8 *row = table + (s << 8);
            double om[3] = { 0, 0, 0 }, cov = 0, k;
            int same = 1, ident = 1;

            for (d = 0; d < 256; d++) {
                same = same && row[d] == row[0];
                ident = ident && row[d] == d;
                for (c = 0; c < 3; c++) {
                    om[c] += palette[3 * row[d] + c] / 256.0;
                }
            }
            if (ident) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            if (same) {
                for (c = 0; c < 3; c++) {
                    o[c] = palette[3 * row[0] + c];
                }
                o[3] = 255;
                continue;
            }
            for (d = 0; d < 256; d++) {
                for (c = 0; c < 3; c++) {
                    cov += (palette[3 * d + c] - mean[c])
                           * (palette[3 * row[d] + c] - om[c]);
                }
            }
            k = var > 0 ? cov / var : 0;
            k = k < 0 ? 0 : k > 1 ? 1 : k;
            for (c = 0; c < 3; c++) {
                o[c] = clamp8(om[c] - k * mean[c]);
            }
            o[3] = clamp8((1.0 - k) * 255.0);
        }
    }
}
