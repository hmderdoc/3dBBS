#ifndef TERMGFX_H
#define TERMGFX_H

#include <citro2d.h>
#include "../term/termbuf.h"

// CP437 font-atlas text rendering (terminal grid + UI text).
// All draw calls must happen inside an active C2D scene.

bool termgfxInit(void);
void termgfxExit(void);

u32 termgfxPalette(u8 idx);  // 16-color VGA palette -> RGBA

// A view maps the terminal grid onto (part of) one screen.
typedef struct {
	float scale;
	float ox, oy;        // pixel offset; oy negative shows a lower grid slice
	int screenW, screenH;
} TermView;

// Whole grid scaled to fit and centered (max scale 1.0)
void termgfxFitView(const Terminal* t, int screenW, int screenH, TermView* v);
// Fixed scale, horizontally centered, vertical start at yOffPix screen pixels
// into the scaled grid (tall mode: top screen 0, bottom screen 240)
void termgfxSpanView(const Terminal* t, int screenW, int screenH, float scale,
                     float yOffPix, TermView* v);

// Render into the current C2D scene. frame drives blink and cursor phase.
void termgfxRenderTermView(const Terminal* t, u32 frame, const TermView* v);

// Map a screen pixel to a grid cell; false if outside the grid
bool termgfxCellAt(const Terminal* t, const TermView* v, int px, int py,
                   int* col, int* row);

// UI text helpers (x,y in pixels, glyphs are 8x16 * scale)
void termgfxDrawChar(float x, float y, float scale, u32 color, u8 ch);
void termgfxDrawText(float x, float y, float scale, u32 color, const char* s);
float termgfxTextWidth(float scale, const char* s);

#endif
