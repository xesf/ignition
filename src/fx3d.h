/*
 * fx3d.h - the effects' drawing: 3D in the scene (EFFECTS 3D) or the
 * 2D layers. The 3D is used with HARDWARE and SOFTWARE, which draw the
 * 3D on the GPU; SOFT 3D always draws the 2D effects.
 */

#ifndef IGN_FX3D_H
#define IGN_FX3D_H

#include "settings.h"
#include "soft3d/soft3d.h"

static inline int fx3d_enabled(void)
{
    return settings_get(SET_EFFECTS) == 1 && !g_soft3d;
}

#endif /* IGN_FX3D_H */
