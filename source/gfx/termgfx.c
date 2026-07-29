#include <string.h>
#include "termgfx.h"
#include "../term/palette.h"
#include "font_cp437_t3x.h"

#define OPAQUE_BLACK 0xFF000000u

#define GLYPH_W 8
#define GLYPH_H 16
#define SCREEN_W 400.0f
#define SCREEN_H 240.0f

// citro2d silently drops draws once its per-frame vertex buffer is full,
// which corrupts whatever is drawn last (the bottom screen) and, because
// backgrounds and glyphs are separate passes, can leave colour blocks with
// no text. Counting the failures turns that from a baffling visual glitch
// into a number on the overlay.
static u32 dropped;

static C2D_SpriteSheet sheet;
static C3D_Tex* fontTex;
static Tex3DS_SubTexture glyphSub[256];

u32 termgfxDropped(void) { return dropped; }

u32 termgfxPalette(u8 idx)
{
	return palAnsi(idx);
}

bool termgfxInit(void)
{
	sheet = C2D_SpriteSheetLoadFromMem(font_cp437_t3x, font_cp437_t3x_size);
	if (!sheet)
		return false;
	C2D_Image base = C2D_SpriteSheetGetImage(sheet, 0);
	fontTex = base.tex;
	C3D_TexSetFilter(fontTex, GPU_LINEAR, GPU_LINEAR);

	float tw = fontTex->width, th = fontTex->height;
	for (int g = 0; g < 256; g++) {
		int gx = (g % 16) * GLYPH_W;
		int gy = (g / 16) * GLYPH_H;
		glyphSub[g].width = GLYPH_W;
		glyphSub[g].height = GLYPH_H;
		glyphSub[g].left = gx / tw;
		glyphSub[g].right = (gx + GLYPH_W) / tw;
		glyphSub[g].top = 1.0f - gy / th;
		glyphSub[g].bottom = 1.0f - (gy + GLYPH_H) / th;
	}
	return true;
}

void termgfxExit(void)
{
	if (sheet) {
		C2D_SpriteSheetFree(sheet);
		sheet = NULL;
	}
}

void termgfxDrawChar(float x, float y, float scale, u32 color, u8 ch)
{
	C2D_Image img = { fontTex, &glyphSub[ch] };
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, color, 1.0f);
	C2D_DrawImageAt(img, x, y, 0.5f, &tint, scale, scale);
}

void termgfxDrawText(float x, float y, float scale, u32 color, const char* s)
{
	for (; *s; s++) {
		termgfxDrawChar(x, y, scale, color, (u8)*s);
		x += GLYPH_W * scale;
	}
}

float termgfxTextWidth(float scale, const char* s)
{
	return strlen(s) * GLYPH_W * scale;
}

void termgfxFitView(const Terminal* t, int screenW, int screenH, TermView* v)
{
	float gridW = t->cols * GLYPH_W;
	float gridH = t->rows * GLYPH_H;
	// Each axis fills independently. Clamping to 1.0 keeps small grids from
	// being blown up past the font atlas's native size; anything larger
	// than the screen (every 132-column mode) uses the whole width.
	float sx = screenW / gridW;
	float sy = screenH / gridH;
	if (sx > 1.0f) sx = 1.0f;
	if (sy > 1.0f) sy = 1.0f;
	v->sx = sx;
	v->sy = sy;
	v->ox = (screenW - gridW * sx) / 2.0f;
	v->oy = (screenH - gridH * sy) / 2.0f;
	v->screenW = screenW;
	v->screenH = screenH;
}

void termgfxSpanView(const Terminal* t, int screenW, int screenH, float scale,
                     float yOffPix, TermView* v)
{
	float gridW = t->cols * GLYPH_W;
	v->sx = v->sy = scale;   // span mode is width-fit, so cells stay square
	v->ox = (screenW - gridW * scale) / 2.0f;
	v->oy = -yOffPix;
	v->screenW = screenW;
	v->screenH = screenH;
}

bool termgfxCellAt(const Terminal* t, const TermView* v, int px, int py,
                   int* col, int* row)
{
	float cw = GLYPH_W * v->sx, chh = GLYPH_H * v->sy;
	int x = (int)((px - v->ox) / cw);
	int y = (int)((py - v->oy) / chh);
	if (px < v->ox || x < 0 || x >= t->cols || y < 0 || y >= t->rows)
		return false;
	*col = x;
	*row = y;
	return true;
}

// One rects-then-glyphs sweep over rows [y0,y1). layerMask selects which
// text layers draw (-1 = all); xShift displaces the whole pass (stereo
// disparity for one eye); z orders passes for the GPU depth test so nearer
// layers occlude deeper ones regardless of submission order.
static void drawCells(const Terminal* t, const TermView* v, int y0, int y1,
                      bool blinkPhase, int layerMask, float xShift, float z)
{
	float cw = GLYPH_W * v->sx, chh = GLYPH_H * v->sy;

	// Two passes: all solid rects, then all textured glyphs. Interleaving
	// them costs a GPU pipeline-state switch per cell, which overflows the
	// command buffer on dense screens (libctru svcBreaks in GPUCMD_Add).
	// Backgrounds go out as merged horizontal runs, not one quad per cell.
	// A cell-at-a-time pass makes a full-screen colour field the single
	// largest consumer of the frame's vertex budget — thousands of quads
	// for what is really a handful of bands — and dense screens are
	// exactly where running out does visible damage.
	for (int y = y0; y < y1; y++) {
		const TermCell* row = &t->cells[y * t->cols];
		float py = v->oy + y * chh;
		int x = 0;
		while (x < t->cols) {
			bool inLayer = (layerMask < 0 || row[x].layer == layerMask);
			if (!inLayer || row[x].bg == OPAQUE_BLACK) {
				x++;
				continue;
			}
			u32 bg = row[x].bg;
			int start = x;
			while (x < t->cols && row[x].bg == bg &&
			       (layerMask < 0 || row[x].layer == layerMask))
				x++;
			if (!C2D_DrawRectSolid(v->ox + xShift + start * cw, py, z,
			                       (x - start) * cw, chh, bg))
				dropped++;
		}
	}

	C2D_ImageTint tint;
	for (int y = y0; y < y1; y++) {
		const TermCell* row = &t->cells[y * t->cols];
		float py = v->oy + y * chh;
		for (int x = 0; x < t->cols; x++) {
			const TermCell* c = &row[x];
			u8 ch = c->ch;
			if (layerMask >= 0 && c->layer != layerMask)
				continue;
			if (ch == ' ' || ch == 0)
				continue;
			if (c->blink && blinkPhase)
				continue;
			C2D_Image img = { fontTex, &glyphSub[ch] };
			C2D_PlainImageTint(&tint, c->fg, 1.0f);
			if (!C2D_DrawImageAt(img, v->ox + xShift + x * cw, py,
			                     z + 0.02f, &tint, v->sx, v->sy))
				dropped++;
		}
	}
}

void termgfxRenderTermView(const Terminal* t, u32 frame, const TermView* v,
                           const float* layerShiftPx)
{
	float cw = GLYPH_W * v->sx, chh = GLYPH_H * v->sy;
	bool blinkPhase = (frame / 32) & 1;

	// Cull rows outside this view's screen
	int y0 = (int)((0 - v->oy) / chh);
	if (y0 < 0) y0 = 0;
	int y1 = (int)((v->screenH - v->oy) / chh) + 1;
	if (y1 > t->rows) y1 = t->rows;

	// Which layers are actually on screen, and do any of them shift?
	bool present[TERM_TEXT_LAYERS] = { false };
	bool anyShift = false;
	int nPresent = 0;
	for (int y = y0; y < y1; y++) {
		const TermCell* row = &t->cells[y * t->cols];
		for (int x = 0; x < t->cols; x++) {
			u8 l = row[x].layer;
			if (l < TERM_TEXT_LAYERS && !present[l]) {
				present[l] = true;
				nPresent++;
				if (layerShiftPx && layerShiftPx[l] != 0.0f)
					anyShift = true;
			}
		}
	}

	if (!anyShift || nPresent <= 1) {
		// Flat path (bottom screen, slider at zero, or single-layer
		// screens): exactly the classic single-sweep draw.
		float shift = 0.0f;
		if (layerShiftPx) {
			for (int l = 0; l < TERM_TEXT_LAYERS; l++)
				if (present[l]) { shift = layerShiftPx[l]; break; }
		}
		drawCells(t, v, y0, y1, blinkPhase, -1, nPresent == 1 ? shift : 0.0f, 0.1f);
	} else {
		// Deep-to-near: sort present layers by depth descending (index
		// ascending on ties). <=16 entries, insertion sort.
		int order[TERM_TEXT_LAYERS];
		int n = 0;
		for (int l = 0; l < TERM_TEXT_LAYERS; l++)
			if (present[l]) order[n++] = l;
		for (int i = 1; i < n; i++) {
			int l = order[i], j = i;
			while (j > 0 && t->layerDepth[order[j - 1]] < t->layerDepth[l]) {
				order[j] = order[j - 1];
				j--;
			}
			order[j] = l;
		}
		for (int i = 0; i < n; i++) {
			int l = order[i];
			drawCells(t, v, y0, y1, blinkPhase, l,
			          layerShiftPx[l], 0.1f + 0.045f * i);
		}
	}

	if (t->cursorVisible && blinkPhase && t->cy >= y0 && t->cy < y1) {
		float px = v->ox + t->cx * cw;
		float py = v->oy + t->cy * chh;
		C2D_DrawRectSolid(px, py + chh * 0.85f, 0.95f, cw, chh * 0.15f,
		                  palAnsi(7));
	}
}
