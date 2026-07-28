#include <string.h>
#include "termgfx.h"
#include "../term/palette.h"
#include "font_cp437_t3x.h"

#define OPAQUE_BLACK 0xFF000000u

#define GLYPH_W 8
#define GLYPH_H 16
#define SCREEN_W 400.0f
#define SCREEN_H 240.0f

static C2D_SpriteSheet sheet;
static C3D_Tex* fontTex;
static Tex3DS_SubTexture glyphSub[256];

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
	float sx = screenW / gridW;
	float sy = screenH / gridH;
	float s = sx < sy ? sx : sy;
	if (s > 1.0f) s = 1.0f;
	v->scale = s;
	v->ox = (screenW - gridW * s) / 2.0f;
	v->oy = (screenH - gridH * s) / 2.0f;
	v->screenW = screenW;
	v->screenH = screenH;
}

void termgfxSpanView(const Terminal* t, int screenW, int screenH, float scale,
                     float yOffPix, TermView* v)
{
	float gridW = t->cols * GLYPH_W;
	v->scale = scale;
	v->ox = (screenW - gridW * scale) / 2.0f;
	v->oy = -yOffPix;
	v->screenW = screenW;
	v->screenH = screenH;
}

bool termgfxCellAt(const Terminal* t, const TermView* v, int px, int py,
                   int* col, int* row)
{
	float cw = GLYPH_W * v->scale, chh = GLYPH_H * v->scale;
	int x = (int)((px - v->ox) / cw);
	int y = (int)((py - v->oy) / chh);
	if (px < v->ox || x < 0 || x >= t->cols || y < 0 || y >= t->rows)
		return false;
	*col = x;
	*row = y;
	return true;
}

void termgfxRenderTermView(const Terminal* t, u32 frame, const TermView* v)
{
	float cw = GLYPH_W * v->scale, chh = GLYPH_H * v->scale;
	bool blinkPhase = (frame / 32) & 1;

	// Cull rows outside this view's screen
	int y0 = (int)((0 - v->oy) / chh);
	if (y0 < 0) y0 = 0;
	int y1 = (int)((v->screenH - v->oy) / chh) + 1;
	if (y1 > t->rows) y1 = t->rows;

	// Two passes: all solid rects, then all textured glyphs. Interleaving
	// them costs a GPU pipeline-state switch per cell, which overflows the
	// command buffer on dense screens (libctru svcBreaks in GPUCMD_Add).
	for (int y = y0; y < y1; y++) {
		const TermCell* row = &t->cells[y * t->cols];
		float py = v->oy + y * chh;
		for (int x = 0; x < t->cols; x++) {
			if (row[x].bg != OPAQUE_BLACK)
				C2D_DrawRectSolid(v->ox + x * cw, py, 0.0f, cw, chh,
				                  row[x].bg);
		}
	}

	C2D_ImageTint tint;
	for (int y = y0; y < y1; y++) {
		const TermCell* row = &t->cells[y * t->cols];
		float py = v->oy + y * chh;
		for (int x = 0; x < t->cols; x++) {
			const TermCell* c = &row[x];
			u8 ch = c->ch;
			if (ch == ' ' || ch == 0)
				continue;
			if (c->blink && blinkPhase)
				continue;
			C2D_Image img = { fontTex, &glyphSub[ch] };
			C2D_PlainImageTint(&tint, c->fg, 1.0f);
			C2D_DrawImageAt(img, v->ox + x * cw, py, 0.5f, &tint,
			                v->scale, v->scale);
		}
	}

	if (t->cursorVisible && blinkPhase && t->cy >= y0 && t->cy < y1) {
		float px = v->ox + t->cx * cw;
		float py = v->oy + t->cy * chh;
		C2D_DrawRectSolid(px, py + chh * 0.85f, 0.75f, cw, chh * 0.15f,
		                  palAnsi(7));
	}
}
