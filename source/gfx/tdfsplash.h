#ifndef TDFSPLASH_H
#define TDFSPLASH_H

#include <citro2d.h>

// Pre-login splash: the product name rendered in TheDraw fonts, several
// copies drifting independently through 3D space at real stereo depth.
//
// The banners are baked cell grids (source/gfx/tdf_splash_data.c), so this
// is a CP437 glyph draw like the terminal's — the only new work is the
// projection. Nothing here touches the TDF format; see
// assets/gen_tdf_splash.py.

void tdfSplashInit(void);
void tdfSplashUpdate(void);        // advance motion; call once per frame
void tdfSplashRender(float iod);   // draw into the active C2D scene
                                   // (iod < 0 left eye, > 0 right, 0 flat)

#endif
