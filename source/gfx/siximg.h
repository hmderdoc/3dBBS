#ifndef SIXIMG_H
#define SIXIMG_H

#include <3ds/types.h>
#include "termgfx.h"

// On-screen sixel image registry. Decode happens on the APC worker; textures
// are created on the main thread (siximgPoll). Images anchor to the grid
// cell where the cursor stood and follow scrolls/clears.

void siximgInit(void);
void siximgExit(void);

// Worker thread: submit a decoded RGBA image (takes ownership of rgba)
void siximgSubmit(u32* rgba, int w, int h, int cellX, int cellY);

// Main thread, once per frame: turn pending submissions into textures
void siximgPoll(void);
// Main thread, inside a C2D scene: draw all images mapped through the view
void siximgDraw(const TermView* v);

// Grid coupling (main thread). Only images inside the scrolled band move —
// DECSTBM region scrolls must not drag graphics parked outside the region.
void siximgScroll(int rows, int topRow, int botRow);
void siximgClearAll(void);
// Text/erase overwrote cells [x0..x1] of row: destroy images under them
void siximgOverwrite(int x0, int x1, int row);

// Telemetry: pipeline counters + last decoded image size
void siximgDebugStats(u32* submitted, u32* textured, u32* texFail,
                      int* lastW, int* lastH);
void siximgDebugLive(int* live, u32* clears);

#endif
