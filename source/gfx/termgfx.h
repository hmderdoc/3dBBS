#ifndef TERMGFX_H
#define TERMGFX_H

#include <citro2d.h>
#include "../term/termbuf.h"

// CP437 font-atlas text rendering (terminal grid + UI text).
// All draw calls must happen inside an active C2D scene.

bool termgfxInit(void);
void termgfxExit(void);

u32 termgfxPalette(u8 idx);  // 16-color VGA palette -> RGBA

// Cumulative count of terminal draws citro2d refused because its per-frame
// vertex buffer was full. Anything but zero means C2D_Init is undersized
// for the current grid: the tail of the frame — the bottom screen — is
// being dropped, and a run that ends between the background and glyph
// passes leaves colour blocks with no text.
u32 termgfxDropped(void);

// A view maps the terminal grid onto (part of) one screen. The axes scale
// independently: a text mode's character cell is not square (VGA 132x60 is
// far narrower per column than 80x25), so forcing one scale on both axes
// would letterbox wide modes into a fraction of the screen rather than
// showing them the way the mode is meant to look.
typedef struct {
	float sx, sy;
	float ox, oy;        // pixel offset; oy negative shows a lower grid slice
	int screenW, screenH;
} TermView;

// Whole grid scaled to fill the screen and centered (neither axis past 1.0)
void termgfxFitView(const Terminal* t, int screenW, int screenH, TermView* v);
// Fixed scale, horizontally centered, vertical start at yOffPix screen pixels
// into the scaled grid (tall mode: top screen 0, bottom screen 240)
void termgfxSpanView(const Terminal* t, int screenW, int screenH, float scale,
                     float yOffPix, TermView* v);

// Render into the current C2D scene. frame drives blink and cursor phase.
// layerShiftPx: per-text-layer horizontal disparity in screen pixels for
// the eye being drawn (scene3dTextShifts), or NULL for a flat draw (bottom
// screen, 3D slider at zero). Layers render deep-to-near so nearer text
// and backgrounds occlude deeper ones.
void termgfxRenderTermView(const Terminal* t, u32 frame, const TermView* v,
                           const float* layerShiftPx);

// Map a screen pixel to a grid cell; false if outside the grid
bool termgfxCellAt(const Terminal* t, const TermView* v, int px, int py,
                   int* col, int* row);

// Depth for UI text drawn by termgfxDrawChar/Text. Defaults to 0.5, which
// sits inside the terminal's own 0.1..0.95 cell range — fine for status
// text on an otherwise empty screen, wrong for anything meant to cover the
// terminal. Modal overlays raise it and restore it afterwards.
void termgfxSetTextDepth(float z);
float termgfxTextDepth(void);

// UI text helpers (x,y in pixels, glyphs are 8x16 * scale)
void termgfxDrawChar(float x, float y, float scale, u32 color, u8 ch);
void termgfxDrawText(float x, float y, float scale, u32 color, const char* s);
float termgfxTextWidth(float scale, const char* s);

#endif
