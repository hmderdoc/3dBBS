#include <string.h>
#include "kbd.h"
#include "../gfx/termgfx.h"

#define STATUS_H   24
#define KB_TOP     40
#define ROW_H      40
#define SCREEN_W   320

// Special key codes (positive values are literal bytes to send)
enum { K_SHIFT = -1, K_CTRL = -2, K_UP = -3, K_DOWN = -4, K_LEFT = -5, K_RIGHT = -6 };

typedef struct {
	const char* label;   // NULL: render the (shifted) char glyph
	s16 ch, shiftCh;
	float w;             // width in row-relative units
} KeyDef;

static const KeyDef row0[] = {
	{NULL,'`','~',1}, {NULL,'1','!',1}, {NULL,'2','@',1}, {NULL,'3','#',1},
	{NULL,'4','$',1}, {NULL,'5','%',1}, {NULL,'6','^',1}, {NULL,'7','&',1},
	{NULL,'8','*',1}, {NULL,'9','(',1}, {NULL,'0',')',1}, {NULL,'-','_',1},
	{NULL,'=','+',1}, {"BS",8,8,1.6f},
};
static const KeyDef row1[] = {
	{"TAB",9,9,1.6f},
	{NULL,'q','Q',1}, {NULL,'w','W',1}, {NULL,'e','E',1}, {NULL,'r','R',1},
	{NULL,'t','T',1}, {NULL,'y','Y',1}, {NULL,'u','U',1}, {NULL,'i','I',1},
	{NULL,'o','O',1}, {NULL,'p','P',1}, {NULL,'[','{',1}, {NULL,']','}',1},
	{NULL,'\\','|',1},
};
static const KeyDef row2[] = {
	{"ESC",27,27,1.6f},
	{NULL,'a','A',1}, {NULL,'s','S',1}, {NULL,'d','D',1}, {NULL,'f','F',1},
	{NULL,'g','G',1}, {NULL,'h','H',1}, {NULL,'j','J',1}, {NULL,'k','K',1},
	{NULL,'l','L',1}, {NULL,';',':',1}, {NULL,'\'','"',1}, {"ENT",'\r','\r',1.9f},
};
static const KeyDef row3[] = {
	{"SHFT",K_SHIFT,K_SHIFT,2.0f},
	{NULL,'z','Z',1}, {NULL,'x','X',1}, {NULL,'c','C',1}, {NULL,'v','V',1},
	{NULL,'b','B',1}, {NULL,'n','N',1}, {NULL,'m','M',1}, {NULL,',','<',1},
	{NULL,'.','>',1}, {NULL,'/','?',1}, {"\x18",K_UP,K_UP,1.2f},
};
static const KeyDef row4[] = {
	{"CTRL",K_CTRL,K_CTRL,2.0f}, {"SPACE",' ',' ',6.5f},
	{"\x1B",K_LEFT,K_LEFT,1.2f}, {"\x19",K_DOWN,K_DOWN,1.2f},
	{"\x1A",K_RIGHT,K_RIGHT,1.2f},
};

static const KeyDef* rowDefs[5] = { row0, row1, row2, row3, row4 };
static const int rowCounts[5] = {
	sizeof(row0)/sizeof(*row0), sizeof(row1)/sizeof(*row1),
	sizeof(row2)/sizeof(*row2), sizeof(row3)/sizeof(*row3),
	sizeof(row4)/sizeof(*row4),
};

typedef struct {
	const KeyDef* def;
	float x, y, w, h;
} Key;

#define MAX_KEYS 72
static Key keys[MAX_KEYS];
static int nKeys;

static KbdSendFn sendFn;
static KbdToggleFn toggleFn;
static bool shiftOn, ctrlOn;
static int heldKey = -1;   // index while touch held
static int heldFrames;

void kbdInit(KbdSendFn send, KbdToggleFn toggle)
{
	sendFn = send;
	toggleFn = toggle;
	nKeys = 0;
	for (int r = 0; r < 5; r++) {
		float units = 0;
		for (int i = 0; i < rowCounts[r]; i++)
			units += rowDefs[r][i].w;
		float scale = SCREEN_W / units;
		float x = 0;
		for (int i = 0; i < rowCounts[r]; i++) {
			Key* k = &keys[nKeys++];
			k->def = &rowDefs[r][i];
			k->x = x;
			k->y = KB_TOP + r * ROW_H;
			k->w = rowDefs[r][i].w * scale;
			k->h = ROW_H;
			x += k->w;
		}
	}
}

static void pressKey(const Key* k)
{
	s16 ch = shiftOn ? k->def->shiftCh : k->def->ch;
	switch (ch) {
	case K_SHIFT: shiftOn = !shiftOn; return;
	case K_CTRL:  ctrlOn = !ctrlOn; return;
	case K_UP:    sendFn((const u8*)"\x1B[A", 3); return;
	case K_DOWN:  sendFn((const u8*)"\x1B[B", 3); return;
	case K_RIGHT: sendFn((const u8*)"\x1B[C", 3); return;
	case K_LEFT:  sendFn((const u8*)"\x1B[D", 3); return;
	default: break;
	}
	u8 c = (u8)ch;
	if (ctrlOn) {
		c &= 0x1F;
		ctrlOn = false;
	} else if (shiftOn && k->def->label == NULL) {
		shiftOn = false;  // one-shot shift for character keys
	}
	sendFn(&c, 1);
}

static int hitTest(int px, int py)
{
	for (int i = 0; i < nKeys; i++) {
		Key* k = &keys[i];
		if (px >= k->x && px < k->x + k->w && py >= k->y && py < k->y + k->h)
			return i;
	}
	return -1;
}

void kbdUpdate(u32 kDown, u32 kHeld, touchPosition touch)
{
	if (kDown & KEY_TOUCH) {
		if (touch.py < STATUS_H) {
			if (toggleFn)
				toggleFn();
			heldKey = -1;
			return;
		}
		int i = hitTest(touch.px, touch.py);
		if (i >= 0) {
			pressKey(&keys[i]);
			heldKey = i;
			heldFrames = 0;
		}
	} else if (kHeld & KEY_TOUCH) {
		if (heldKey >= 0) {
			// key repeat: 400ms delay, then ~10Hz
			heldFrames++;
			if (heldFrames > 24 && heldFrames % 6 == 0) {
				s16 ch = keys[heldKey].def->ch;
				if (ch > 0)
					pressKey(&keys[heldKey]);
			}
		}
	} else {
		heldKey = -1;
	}
}

void kbdRender(const char* status, bool connected)
{
	// Status bar
	C2D_DrawRectSolid(0, 0, 0, SCREEN_W, STATUS_H,
	                  connected ? 0xFF203020 : 0xFF202030);
	termgfxDrawChar(4, 4, 1.0f, connected ? 0xFF00FF00 : 0xFF0000FF, 0x07); // status dot
	termgfxDrawText(16, 4, 1.0f, 0xFFCCCCCC, status);

	// Keys
	for (int i = 0; i < nKeys; i++) {
		Key* k = &keys[i];
		s16 ch = shiftOn ? k->def->shiftCh : k->def->ch;
		bool active = (ch == K_SHIFT && shiftOn) || (ch == K_CTRL && ctrlOn);
		bool pressed = (i == heldKey);
		u32 bg = pressed ? 0xFF808080 : active ? 0xFFA06030 : 0xFF383838;
		C2D_DrawRectSolid(k->x + 1, k->y + 1, 0, k->w - 2, k->h - 2, bg);

		if (k->def->label) {
			float ts = strlen(k->def->label) > 1 ? 0.75f : 1.0f;
			float tw = termgfxTextWidth(ts, k->def->label);
			termgfxDrawText(k->x + (k->w - tw) / 2,
			                k->y + (k->h - 16 * ts) / 2, ts, 0xFFCCCCCC,
			                k->def->label);
		} else {
			termgfxDrawChar(k->x + (k->w - 8) / 2, k->y + (k->h - 16) / 2,
			                1.0f, 0xFFFFFFFF, (u8)ch);
		}
	}
}
