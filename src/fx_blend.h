/*
 * fx_blend.h - a colour table as the GPU's blending.
 *
 * A colour table gives T[s << 8 | d], the colour index that source s
 * makes over the screen's d. The GPU cannot read the screen, but it can
 * blend: out = C + k * under. Each source row is fitted by least
 * squares over the 256 palette colours to that form, and kept as one
 * premultiplied RGBA texel (C, alpha 1 - k).
 */

#ifndef IGN_FX_BLEND_H
#define IGN_FX_BLEND_H

#include "types.h"

/* The 256 rows of `table` (256 x 256 colour indices, [s << 8 | d]) as
 * `out`, 256 RGBA texels (premultiplied), through `palette` (256 RGB
 * entries of 8 bits). A row that is one colour is that colour, opaque;
 * the identity row is clear. k is kept in 0..1: a row that brightens
 * what is under it adds its colour (alpha 0). A NULL table gives the
 * palette's colours, opaque (0 clear). */
void fx_blend_rows(const u8 *table, const u8 *palette, u8 out[256 * 4]);

#endif /* IGN_FX_BLEND_H */
