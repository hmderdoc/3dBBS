#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pbview.h"
#include "phonebook.h"
#include "../gfx/termgfx.h"

#define SCREEN_W  320
#define STATUS_H  22
#define LIST_TOP  (STATUS_H + 2)
#define ROW_H     16
#define BTN_H     34
#define BTN_TOP   (240 - BTN_H)
#define LEGEND_H  14                      // KEY line above the buttons
#define LIST_BOT  (BTN_TOP - LEGEND_H)
#define VISIBLE   ((LIST_BOT - LIST_TOP) / ROW_H)

static PbPromptFn promptFn;
static PbConnectFn connectFn;
static int scroll;
static int confirmDel;   // frames left on the "Sure?" delete confirmation
static int pressedBtn = -1;
static int pressedFrames;
static char toast[64];   // "what just changed" line
static int toastFrames;

// Magenta<->cyan pulse (the anaglyph palette) for marking 3D-capable
// boards; one triangle-wave period ~2s. Colors are ABGR like the rest of
// this file.
static u32 animFrame;

static u32 pulse3d(void)
{
	u32 t = animFrame & 127;
	u32 tri = (t < 64 ? t : 127 - t) * 4;   // 0..255 triangle
	u8 r = (u8)(255 - tri);
	u8 g = (u8)tri;
	return 0xFF000000 | (0xFFu << 16) | ((u32)g << 8) | r;
}

static void drawBorder(float x, float y, float w, float h, u32 c)
{
	C2D_DrawRectSolid(x, y, 0, w, 2, c);
	C2D_DrawRectSolid(x, y + h - 2, 0, w, 2, c);
	C2D_DrawRectSolid(x, y, 0, 2, h, c);
	C2D_DrawRectSolid(x + w - 2, y, 0, 2, h, c);
}

static void setToast(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(toast, sizeof(toast), fmt, ap);
	va_end(ap);
	toastFrames = 150;   // ~2.5s
}

typedef struct {
	const char* label;
	float x, w;
} Btn;

// Seven buttons across 320px, so width is spent where it is pressed most:
// DIAL is the common action, DEL must still fit its "SURE?" label.
static Btn buttons[] = {
	{ "DIAL", 0, 66 }, { "EDIT", 66, 46 }, { "USER", 112, 46 },
	{ "PROTO", 158, 52 }, { "SIZE", 210, 44 }, { "ADD", 254, 30 },
	{ "DEL", 284, 36 },
};
#define NBTN (int)(sizeof(buttons) / sizeof(buttons[0]))
enum { BTN_DIAL, BTN_EDIT, BTN_USER, BTN_PROTO, BTN_SIZE, BTN_ADD, BTN_DEL };

// Terminal-size picker: the preset list plus a Custom slot, two per row.
static bool sizeOpen;
#define SZ_COLS   2
#define SZ_CELL_W (SCREEN_W / SZ_COLS)
#define SZ_CELL_H 24
static int sizeCells;    // presets + 1 for Custom, filled in on open
static int sizeHot = -1; // cell under the finger / keyboard highlight
static bool cancelHot;   // finger is on the picker's CANCEL bar

void pbviewInit(PbPromptFn prompt, PbConnectFn connect)
{
	promptFn = prompt;
	connectFn = connect;
}

static void ensureVisible(void)
{
	int sel = pbSelected();
	if (sel < scroll)
		scroll = sel;
	if (sel >= scroll + VISIBLE)
		scroll = sel - VISIBLE + 1;
	if (scroll < 0)
		scroll = 0;
}

// "80x50" -> geometry. Anything unparseable (including the empty string the
// user gets by clearing the field) means "back to the default".
static void applySizeText(int idx, const char* text)
{
	unsigned c = 0, r = 0;
	// The system keyboard makes an uppercase X as easy to type as a
	// lowercase one, so accept either separator.
	if (sscanf(text, "%ux%u", &c, &r) != 2 &&
	    sscanf(text, "%uX%u", &c, &r) != 2)
		c = r = 0;
	pbSetSize(idx, (u16)c, (u16)r);
}

static void sizeText(int idx, char* buf, size_t cap)
{
	u16 c, r;
	pbSizeOf(idx, &c, &r);
	snprintf(buf, cap, "%ux%u", c, r);
}

static void editEntry(int idx)
{
	const PbEntry* e = pbGet(idx);
	char name[32], host[64], port[8], size[12];
	snprintf(name, sizeof(name), "%s", e->name);
	snprintf(host, sizeof(host), "%s", e->host);
	snprintf(port, sizeof(port), "%u", e->port);
	sizeText(idx, size, sizeof(size));
	if (!promptFn("Board name", name, sizeof(name), false))
		return;
	if (!promptFn("Hostname", host, sizeof(host), false))
		return;
	if (!promptFn("Port", port, sizeof(port), false))
		return;
	int p = atoi(port);
	pbSetEntry(idx, name, host, (p > 0 && p < 65536) ? (u16)p : 0);
	// The custom-size override. SIZE cycles the presets; this is where a
	// board wanting something off the list gets it.
	if (promptFn("Terminal size COLSxROWS", size, sizeof(size), false))
		applySizeText(idx, size);
}

static void addEntry(void)
{
	char name[32] = "", host[64] = "", port[8] = "23";
	if (!promptFn("New board name", name, sizeof(name), false))
		return;
	if (!promptFn("Hostname", host, sizeof(host), false))
		return;
	if (!promptFn("Port", port, sizeof(port), false))
		return;
	int p = atoi(port);
	int idx = pbAdd(name, host, (p > 0 && p < 65536) ? (u16)p : 23);
	if (idx >= 0) {
		pbSelect(idx);
		ensureVisible();
	}
}

static void editCreds(int idx)
{
	const PbEntry* e = pbGet(idx);
	char user[32], pass[32] = "";
	snprintf(user, sizeof(user), "%s", e->user);
	if (!promptFn("Username for this board", user, sizeof(user), false))
		return;
	if (!promptFn("Password (plain text on SD)", pass, sizeof(pass), true))
		return;
	pbSetCreds(idx, user, pass);
}

static void pressButton(int i)
{
	int sel = pbSelected();
	if (i != NBTN - 1)
		confirmDel = 0;   // any other action cancels a pending delete
	pressedBtn = i;
	pressedFrames = 10;

	switch (i) {
	case 0: if (connectFn) connectFn(); break;
	case 1:
		editEntry(sel);
		setToast("%s %s:%u", pbGet(sel)->name, pbGet(sel)->host,
		         pbGet(sel)->port);
		break;
	case 2:
		editCreds(sel);
		setToast(pbGet(sel)->user[0] ? "login saved for %s" : "no login for %s",
		         pbGet(sel)->name);
		break;
	case 3: {
		pbToggleProto(sel);
		const PbEntry* e = pbGet(sel);
		setToast("%s -> %s:%u", e->name,
		         e->proto == PROTO_RLOGIN ? "rlogin" :
		         e->proto == PROTO_SSH    ? "ssh"    : "telnet", e->port);
		break;
	}
	case BTN_SIZE:
		sizeCells = pbSizePresetCount() + 1;   // + "Custom..."
		sizeHot = -1;
		sizeOpen = true;
		break;
	case 5: addEntry(); break;
	case 6:
		if (confirmDel > 0) {
			pbDelete(sel);
			confirmDel = 0;
			ensureVisible();
		} else if (pbCount() > 1) {
			confirmDel = 180;   // ~3s to confirm
		}
		break;
	}
}

// Commit a picker cell: the last one is the custom override, the rest are
// presets.
static void chooseSize(int cell)
{
	int sel = pbSelected();
	if (cell == sizeCells - 1) {
		char size[12];
		sizeText(sel, size, sizeof(size));
		if (promptFn("Terminal size COLSxROWS", size, sizeof(size), false))
			applySizeText(sel, size);
	} else {
		u16 c, r;
		pbSizePreset(cell, &c, &r);
		pbSetSize(sel, c, r);
	}
	sizeOpen = false;
	sizeHot = -1;
	char size[12];
	sizeText(sel, size, sizeof(size));
	setToast("%s -> %s", pbGet(sel)->name, size);
}

// Which picker cell a point falls in, or -1.
static int sizeCellAt(int px, int py)
{
	if (py < LIST_TOP || py >= LIST_TOP + ((sizeCells + 1) / SZ_COLS) * SZ_CELL_H)
		return -1;
	int col = px / SZ_CELL_W;
	int row = (py - LIST_TOP) / SZ_CELL_H;
	int cell = row * SZ_COLS + col;
	return (cell >= 0 && cell < sizeCells) ? cell : -1;
}

static int buttonAt(int px, int py)
{
	if (py < BTN_TOP)
		return -1;
	for (int i = 0; i < NBTN; i++) {
		if (px >= buttons[i].x && px < buttons[i].x + buttons[i].w)
			return i;
	}
	return -1;
}

void pbviewUpdate(u32 kDown, u32 kHeld, touchPosition touch)
{
	// Touch is tracked across the whole press and acted on at RELEASE,
	// using the last position seen while the finger was down. Dispatching
	// on the press frame alone means one sample decides everything, which
	// on this panel drops taps; holding also lets a wobbly press land, and
	// lets you slide off a button to cancel it.
	static bool wasTouching;
	static touchPosition held;
	bool touching = (kHeld & KEY_TOUCH) != 0;

	if (confirmDel > 0)
		confirmDel--;
	if (pressedFrames > 0 && --pressedFrames == 0)
		pressedBtn = -1;
	if (toastFrames > 0)
		toastFrames--;
	pbTick();   // deferred SD write, once the tapping stops

	if (sizeOpen) {
		// Modal: the list is covered, so only the picker and cancel react
		if (kDown & KEY_B)     { sizeOpen = false; sizeHot = -1; }
		if (kDown & KEY_DLEFT  && sizeHot > 0)               sizeHot--;
		if (kDown & KEY_DRIGHT && sizeHot < sizeCells - 1)   sizeHot++;
		if (kDown & KEY_DUP    && sizeHot >= SZ_COLS)        sizeHot -= SZ_COLS;
		if (kDown & KEY_DDOWN  && sizeHot + SZ_COLS < sizeCells) sizeHot += SZ_COLS;
		if ((kDown & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) &&
		    sizeHot < 0)
			sizeHot = 0;
		if ((kDown & KEY_A) && sizeHot >= 0)
			chooseSize(sizeHot);

		if (touching) {
			held = touch;
			int cell = sizeCellAt(held.px, held.py);
			if (cell >= 0)
				sizeHot = cell;
			cancelHot = (held.py >= BTN_TOP);
		} else if (wasTouching) {
			cancelHot = false;
			if (held.py >= BTN_TOP) {
				sizeOpen = false;      // the full-width CANCEL bar
				sizeHot = -1;
			} else {
				int cell = sizeCellAt(held.px, held.py);
				if (cell >= 0)
					chooseSize(cell);
			}
		}
		wasTouching = touching;
		return;
	}

	if (kDown & KEY_DUP)   { pbSelectPrev(); ensureVisible(); confirmDel = 0; }
	if (kDown & KEY_DDOWN) { pbSelectNext(); ensureVisible(); confirmDel = 0; }
	if (kDown & KEY_A)     { if (connectFn) connectFn(); }
	if (kDown & KEY_Y)     pressButton(BTN_USER);
	if (kDown & KEY_X)     pressButton(BTN_PROTO);

	if (touching) {
		held = touch;
		// Light the button under the finger straight away; the action
		// itself waits for the release.
		pressedBtn = buttonAt(held.px, held.py);
		pressedFrames = pressedBtn >= 0 ? 2 : 0;
	} else if (wasTouching) {
		pressedFrames = 0;
		int btn = buttonAt(held.px, held.py);
		if (btn >= 0) {
			pressButton(btn);
		} else if (held.py >= LIST_TOP && held.py < LIST_BOT) {
			int row = (held.py - LIST_TOP) / ROW_H + scroll;
			if (row >= 0 && row < pbCount()) {
				if (row == pbSelected() && connectFn)
					connectFn();   // tap the selected entry again to dial
				else
					pbSelect(row);
				confirmDel = 0;
			}
		} else {
			pressedBtn = -1;
		}
	}
	wasTouching = touching;
}

void pbviewRender(const char* status)
{
	animFrame++;
	// Status bar doubles as the "what just happened" line
	C2D_DrawRectSolid(0, 0, 0, SCREEN_W, STATUS_H,
	                  toastFrames > 0 ? 0xFF105010 : 0xFF202030);
	termgfxDrawText(4, 3, 0.75f, toastFrames > 0 ? 0xFF80FF80 : 0xFFCCCCCC,
	                toastFrames > 0 ? toast : status);

	int sel = pbSelected();
	int n = pbCount();
	char line[64];

	if (sizeOpen) {
		u16 curC, curR;
		pbSizeOf(sel, &curC, &curR);
		C2D_DrawRectSolid(0, LIST_TOP, 0, SCREEN_W, LIST_BOT - LIST_TOP,
		                  0xFF101820);
		for (int k = 0; k < sizeCells; k++) {
			float cx = (k % SZ_COLS) * SZ_CELL_W;
			float cy = LIST_TOP + (k / SZ_COLS) * SZ_CELL_H;
			bool custom = (k == sizeCells - 1);
			u16 c = 0, r = 0;
			if (!custom)
				pbSizePreset(k, &c, &r);
			bool current = !custom && c == curC && r == curR;

			if (k == sizeHot)
				C2D_DrawRectSolid(cx + 1, cy + 1, 0, SZ_CELL_W - 2,
				                  SZ_CELL_H - 2, 0xFFC08040);
			else if (current)
				C2D_DrawRectSolid(cx + 1, cy + 1, 0, SZ_CELL_W - 2,
				                  SZ_CELL_H - 2, 0xFF603010);

			if (custom)
				snprintf(line, sizeof(line), "Custom...");
			else
				snprintf(line, sizeof(line), "%ux%u%s", c, r,
				         current ? "  *" : "");
			termgfxDrawText(cx + 10, cy + 4, 0.9f,
			                custom ? 0xFF88AACC : 0xFFFFFFFF, line);
		}
		termgfxDrawText(4, LIST_BOT + 1, 0.7f, 0xFF999999,
		                "terminal size for this board");
		C2D_DrawRectSolid(1, BTN_TOP + 1, 0, SCREEN_W - 2, BTN_H - 2,
		                  cancelHot ? 0xFFC08040 : 0xFF383838);
		float tw = termgfxTextWidth(0.85f, "CANCEL");
		termgfxDrawText((SCREEN_W - tw) / 2,
		                BTN_TOP + (BTN_H - 16 * 0.85f) / 2, 0.85f,
		                0xFFFFFFFF, "CANCEL");
		return;
	}

	for (int i = 0; i < VISIBLE && scroll + i < n; i++) {
		int idx = scroll + i;
		const PbEntry* e = pbGet(idx);
		float y = LIST_TOP + i * ROW_H;
		if (idx == sel)
			C2D_DrawRectSolid(0, y, 0, SCREEN_W, ROW_H, 0xFF603010);

		// Tags fill in from the right; whatever room is left goes to the
		// name/host, truncated to fit rather than overprinting them.
		float rightX = SCREEN_W - 6;

		snprintf(line, sizeof(line), "%s%s",
		         e->proto == PROTO_RLOGIN ? "RLGN" :
		         e->proto == PROTO_SSH    ? " SSH" : "TLNT",
		         e->user[0] ? "*" : " ");   // '*' = credentials stored
		rightX -= termgfxTextWidth(0.85f, line);
		termgfxDrawText(rightX, y + 1, 0.85f,
		                e->proto == PROTO_RLOGIN ? 0xFF66FF66 :
		                e->proto == PROTO_SSH    ? 0xFFFFCC66 : 0xFFAAAAAA,
		                line);

		// 3D-capable boards: animated anaglyph border + tag (see KEY below)
		if (e->flags & PB_FLAG_3D) {
			u32 c = pulse3d();
			drawBorder(0, y, SCREEN_W, ROW_H, c);
			rightX -= 4 + termgfxTextWidth(0.85f, "3D");
			termgfxDrawText(rightX, y + 1, 0.85f, c, "3D");
		}

		// Only non-default geometry is worth the pixels
		if (e->cols && e->rows) {
			snprintf(line, sizeof(line), "%ux%u", e->cols, e->rows);
			rightX -= 4 + termgfxTextWidth(0.85f, line);
			termgfxDrawText(rightX, y + 1, 0.85f, 0xFF88AACC, line);
		}

		snprintf(line, sizeof(line), "%-11.11s %.34s:%u", e->name, e->host,
		         e->port);
		int fits = (int)((rightX - 3 - 4) / termgfxTextWidth(0.85f, "M"));
		if (fits > 0 && fits < (int)strlen(line))
			line[fits] = 0;
		termgfxDrawText(3, y + 1, 0.85f, 0xFFFFFFFF, line);
	}

	if (n > VISIBLE) {
		float frac = (float)VISIBLE / n;
		float barH = (LIST_BOT - LIST_TOP) * frac;
		float barY = LIST_TOP + (LIST_BOT - LIST_TOP) * ((float)scroll / n);
		C2D_DrawRectSolid(SCREEN_W - 3, barY, 0, 3, barH, 0xFF808080);
	}

	// KEY: what the animated border means
	{
		u32 c = pulse3d();
		drawBorder(4, LIST_BOT + 2, 22, LEGEND_H - 4, c);
		termgfxDrawText(6, LIST_BOT + 1, 0.7f, c, "3D");
		termgfxDrawText(30, LIST_BOT + 1, 0.7f, 0xFF999999,
		                "= board sends stereoscopic scenes");
	}

	for (int i = 0; i < NBTN; i++) {
		bool danger = (i == NBTN - 1) && confirmDel > 0;
		bool lit = (i == pressedBtn);
		C2D_DrawRectSolid(buttons[i].x + 1, BTN_TOP + 1, 0,
		                  buttons[i].w - 2, BTN_H - 2,
		                  danger ? 0xFF3030C0 : lit ? 0xFFC08040 : 0xFF383838);
		const char* label = danger ? "SURE?" : buttons[i].label;
		float tw = termgfxTextWidth(0.85f, label);
		termgfxDrawText(buttons[i].x + (buttons[i].w - tw) / 2,
		                BTN_TOP + (BTN_H - 16 * 0.85f) / 2, 0.85f,
		                0xFFFFFFFF, label);
	}
}
