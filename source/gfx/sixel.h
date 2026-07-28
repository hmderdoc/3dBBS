#ifndef SIXEL_H
#define SIXEL_H

#include <3ds/types.h>
#include <stdbool.h>

#define SIXEL_MAX_W 1024
#define SIXEL_MAX_H 1024

// Decode a sixel stream (DCS payload: "P1;P2;P3q<data>", the DCS/q header
// included). On success returns a malloc'd RGBA8888 buffer (row-major,
// u32 per pixel as r|g<<8|b<<16|a<<24) — caller frees.
bool sixelDecode(const u8* data, int len, u32** rgba, int* w, int* h);

// Scroll-band geometry for overlay images (pure math, host-testable).
// Shifts an image with a band scroll of dPix and grows the clip amounts for
// rows that exited the band. Returns true when fully consumed (free it).
bool sixelShiftClip(int* yPix, int h, int* clipTop, int* clipBot,
                    int dPix, int bandTop, int bandBot);

#endif
