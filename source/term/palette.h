#ifndef PALETTE_H
#define PALETTE_H

#include <3ds/types.h>

// Shared color tables, as u32 ABGR (C2D_Color32 layout).
// 0-15: VGA text colors in ANSI order; 16-231: xterm 6x6x6 cube;
// 232-255: xterm grayscale ramp.
u32 palAnsi(int idx);

// Pack r,g,b into the same layout
static inline u32 palRGB(u8 r, u8 g, u8 b)
{
	return 0xFF000000u | ((u32)b << 16) | ((u32)g << 8) | r;
}

#endif
