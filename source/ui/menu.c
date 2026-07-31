#include <stdio.h>
#include <string.h>
#include "menu.h"
#include "../gfx/termgfx.h"

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
	{ "Quit",               "leave 3dBBS",                       MENU_QUIT },
};
#define NITEMS (int)(sizeof(items) / sizeof(items[0]))

#define TOP  ((SCREEN_H - (NITEMS * ROW_H + 34)) / 2)
#define LIST (TOP + 30)

static bool open_;
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
	if (open_)
		sel = 0;
	hot = -1;
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

	// Resume is the no-op entry: selecting it just closes.
	if (act == MENU_NONE && (kDown & KEY_A) && items[sel].action == MENU_NONE)
		menuClose();
	else if (act != MENU_NONE)
		menuClose();
	return act;
}

void menuRender(void)
{
	float h = NITEMS * ROW_H + 34;

	// Dim what's behind so the menu reads as modal rather than as more UI
	C2D_DrawRectSolid(0, 0, 0, SCREEN_W, SCREEN_H, 0xC0000000);
	C2D_DrawRectSolid(PAD - 4, TOP - 4, 0, SCREEN_W - 2 * (PAD - 4), h + 8,
	                  0xFF202030);
	C2D_DrawRectSolid(PAD - 2, TOP - 2, 0, SCREEN_W - 2 * (PAD - 2), h + 4,
	                  0xFF101018);

	termgfxDrawText(PAD + 2, TOP + 4, 0.85f, 0xFFCCCCCC, "3dBBS");
	termgfxDrawText(SCREEN_W - PAD - termgfxTextWidth(0.7f, "START closes") - 2,
	                TOP + 7, 0.7f, 0xFF808080, "START closes");

	for (int i = 0; i < NITEMS; i++) {
		float y = LIST + i * ROW_H;
		bool on = (i == sel);
		if (on)
			C2D_DrawRectSolid(PAD, y, 0, SCREEN_W - 2 * PAD, ROW_H - 2,
			                  i == hot ? 0xFFC08040 : 0xFF603010);
		termgfxDrawText(PAD + 8, y + 3, 0.9f,
		                on ? 0xFFFFFFFF : 0xFFAAAAAA, items[i].label);
		termgfxDrawText(PAD + 8, y + 17, 0.6f,
		                on ? 0xFFCCCCCC : 0xFF707070, items[i].hint);
	}
}
