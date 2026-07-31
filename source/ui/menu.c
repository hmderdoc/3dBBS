#include <stdio.h>
#include <string.h>
#include "menu.h"
#include "../gfx/termgfx.h"

// Above the terminal's cell range (0.1..0.95). Draw order alone does not put
// an overlay on top: the depth test does, and a panel at z=0 loses to the
// glyphs of whatever is behind it.
#define UI_Z 0.985f
#include "phonebook.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define ROW_H    30
#define PAD      10

typedef struct {
	const char* label;
	const char* hint;
	MenuAction action;
} Item;

static const Item items[] = {
	{ "Resume",             "close this menu",                   MENU_NONE },
	{ "Controller Mapping", "remap the pad, sticks and buttons", MENU_MAPPING },
	{ "Terminal Size",      "change the grid for this session",  MENU_TERMSIZE },
	{ "Quit",               "leave 3dBBS",                       MENU_QUIT },
};
#define NITEMS (int)(sizeof(items) / sizeof(items[0]))

#define TOP  ((SCREEN_H - (NITEMS * ROW_H + 34)) / 2)
#define LIST (TOP + 30)

static bool open_;
static bool sizing;       // showing the geometry list rather than the menu
static u16 pickCols, pickRows;
static int sel;
static int hot = -1;      // row under the finger
static bool wasTouching;
static touchPosition held;

void menuInit(void)
{
	open_ = false;
	sel = 0;
	hot = -1;
}

bool menuIsOpen(void) { return open_; }
void menuClose(void) { open_ = false; hot = -1; }

void menuToggle(void)
{
	open_ = !open_;
	sizing = false;
	if (open_)
		sel = 0;
	hot = -1;
}

void menuPickedSize(u16* cols, u16* rows)
{
	if (cols) *cols = pickCols;
	if (rows) *rows = pickRows;
}

// The geometry list reuses the phonebook's SyncTERM presets so the menu and
// the dialing directory can never drift apart on what sizes exist.
// Rows are derived, not fixed: with 13 presets a hardcoded 6 left the last
// one outside the hit test and unreachable by touch.
#define SZ_COLS 2
#define SZ_ROWS ((pbSizePresetCount() + SZ_COLS - 1) / SZ_COLS)
#define SZ_CELL_W (SCREEN_W / SZ_COLS)
#define SZ_CELL_H 26
#define SZ_TOP 34

static int sizeCellAt(int px, int py)
{
	if (py < SZ_TOP || py >= SZ_TOP + SZ_ROWS * SZ_CELL_H)
		return -1;
	int c = px / SZ_CELL_W, r = (py - SZ_TOP) / SZ_CELL_H;
	int k = r * SZ_COLS + c;
	return (k >= 0 && k < pbSizePresetCount()) ? k : -1;
}

static int rowAt(int px, int py)
{
	if (px < PAD || px > SCREEN_W - PAD)
		return -1;
	int r = (py - LIST) / ROW_H;
	return (py >= LIST && r >= 0 && r < NITEMS) ? r : -1;
}

MenuAction menuUpdate(u32 kDown, u32 kHeld, touchPosition touch)
{
	MenuAction act = MENU_NONE;
	bool touching = (kHeld & KEY_TOUCH) != 0;

	if (sizing) {
		int n = pbSizePresetCount();
		if (kDown & KEY_B) { sizing = false; return MENU_NONE; }
		if (kDown & KEY_DLEFT  && sel > 0) sel--;
		if (kDown & KEY_DRIGHT && sel < n - 1) sel++;
		if (kDown & KEY_DUP    && sel >= SZ_COLS) sel -= SZ_COLS;
		if (kDown & KEY_DDOWN  && sel + SZ_COLS < n) sel += SZ_COLS;
		int chosen = -1;
		if (kDown & KEY_A) chosen = sel;
		if (touching) {
			held = touch;
			int c = sizeCellAt(held.px, held.py);
			if (c >= 0) sel = c;
			hot = c;
		} else if (wasTouching) {
			chosen = sizeCellAt(held.px, held.py);
			hot = -1;
		}
		wasTouching = touching;
		if (chosen >= 0) {
			pbSizePreset(chosen, &pickCols, &pickRows);
			sizing = false;
			menuClose();
			return MENU_TERMSIZE;
		}
		return MENU_NONE;
	}

	if (kDown & KEY_DUP)   sel = (sel + NITEMS - 1) % NITEMS;
	if (kDown & KEY_DDOWN) sel = (sel + 1) % NITEMS;
	if (kDown & KEY_B)     { menuClose(); return MENU_NONE; }
	if (kDown & KEY_A)     act = items[sel].action;

	// Same press-and-release handling as the phonebook: acting on the single
	// touch-down frame lets one sample decide everything, which drops taps.
	if (touching) {
		held = touch;
		hot = rowAt(held.px, held.py);
		if (hot >= 0)
			sel = hot;
	} else if (wasTouching) {
		int r = rowAt(held.px, held.py);
		if (r >= 0)
			act = items[r].action;
		hot = -1;
	}
	wasTouching = touching;

	if (act == MENU_TERMSIZE) {
		// Not an action yet — show the list and report only once a size is
		// actually chosen.
		sizing = true;
		sel = 0;
		hot = -1;
		return MENU_NONE;
	}
	// Resume is the no-op entry: selecting it just closes.
	if (act == MENU_NONE && (kDown & KEY_A) && items[sel].action == MENU_NONE)
		menuClose();
	else if (act != MENU_NONE)
		menuClose();
	return act;
}

void menuRender(void)
{
	termgfxSetTextDepth(UI_Z);
	if (sizing) {
		C2D_DrawRectSolid(0, 0, UI_Z, SCREEN_W, SCREEN_H, 0xFF101018);
		termgfxDrawText(6, 8, 0.85f, 0xFFCCCCCC, "Terminal Size");
		termgfxDrawText(SCREEN_W - 96, 11, 0.7f, 0xFF808080, "B cancels");
		char lbl[16];
		for (int k = 0; k < pbSizePresetCount(); k++) {
			u16 c, r;
			pbSizePreset(k, &c, &r);
			float x = (k % SZ_COLS) * SZ_CELL_W;
			float y = SZ_TOP + (k / SZ_COLS) * SZ_CELL_H;
			if (k == sel)
				C2D_DrawRectSolid(x + 2, y + 1, UI_Z, SZ_CELL_W - 4,
				                  SZ_CELL_H - 2,
				                  k == hot ? 0xFFC08040 : 0xFF603010);
			snprintf(lbl, sizeof(lbl), "%ux%u", c, r);
			termgfxDrawText(x + 12, y + 5, 0.85f,
			                k == sel ? 0xFFFFFFFF : 0xFFBBBBBB, lbl);
		}
		termgfxSetTextDepth(0.5f);
		return;
	}

	float h = NITEMS * ROW_H + 34;

	// Dim what's behind so the menu reads as modal rather than as more UI
	C2D_DrawRectSolid(0, 0, UI_Z, SCREEN_W, SCREEN_H, 0xFF0A0A10);
	C2D_DrawRectSolid(PAD - 4, TOP - 4, UI_Z, SCREEN_W - 2 * (PAD - 4), h + 8,
	                  0xFF202030);
	C2D_DrawRectSolid(PAD - 2, TOP - 2, UI_Z, SCREEN_W - 2 * (PAD - 2), h + 4,
	                  0xFF101018);

	termgfxDrawText(PAD + 2, TOP + 4, 0.85f, 0xFFCCCCCC, "3dBBS");
	termgfxDrawText(SCREEN_W - PAD - termgfxTextWidth(0.7f, "START closes") - 2,
	                TOP + 7, 0.7f, 0xFF808080, "START closes");

	for (int i = 0; i < NITEMS; i++) {
		float y = LIST + i * ROW_H;
		bool on = (i == sel);
		if (on)
			C2D_DrawRectSolid(PAD, y, UI_Z, SCREEN_W - 2 * PAD, ROW_H - 2,
			                  i == hot ? 0xFFC08040 : 0xFF603010);
		termgfxDrawText(PAD + 8, y + 3, 0.9f,
		                on ? 0xFFFFFFFF : 0xFFAAAAAA, items[i].label);
		termgfxDrawText(PAD + 8, y + 17, 0.6f,
		                on ? 0xFFCCCCCC : 0xFF707070, items[i].hint);
	}
	termgfxSetTextDepth(0.5f);
}
