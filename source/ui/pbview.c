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

// Connect is widest: it's the common action
static Btn buttons[] = {
	{ "DIAL", 0, 74 }, { "EDIT", 74, 58 }, { "USER", 132, 58 },
	{ "PROTO", 190, 66 }, { "ADD", 256, 34 }, { "DEL", 290, 30 },
};
#define NBTN (int)(sizeof(buttons) / sizeof(buttons[0]))

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

static void editEntry(int idx)
{
	const PbEntry* e = pbGet(idx);
	char name[32], host[64], port[8];
	snprintf(name, sizeof(name), "%s", e->name);
	snprintf(host, sizeof(host), "%s", e->host);
	snprintf(port, sizeof(port), "%u", e->port);
	if (!promptFn("Board name", name, sizeof(name), false))
		return;
	if (!promptFn("Hostname", host, sizeof(host), false))
		return;
	if (!promptFn("Port", port, sizeof(port), false))
		return;
	int p = atoi(port);
	pbSetEntry(idx, name, host, (p > 0 && p < 65536) ? (u16)p : 0);
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
		         e->proto == PROTO_RLOGIN ? "rlogin" : "telnet", e->port);
		break;
	}
	case 4: addEntry(); break;
	case 5:
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

void pbviewUpdate(u32 kDown, u32 kHeld, touchPosition touch)
{
	(void)kHeld;
	if (confirmDel > 0)
		confirmDel--;
	if (pressedFrames > 0 && --pressedFrames == 0)
		pressedBtn = -1;
	if (toastFrames > 0)
		toastFrames--;
	pbTick();   // deferred SD write, once the tapping stops

	if (kDown & KEY_DUP)   { pbSelectPrev(); ensureVisible(); confirmDel = 0; }
	if (kDown & KEY_DDOWN) { pbSelectNext(); ensureVisible(); confirmDel = 0; }
	if (kDown & KEY_A)     { if (connectFn) connectFn(); }
	if (kDown & KEY_Y)     pressButton(2);
	if (kDown & KEY_X)     pressButton(3);

	if (!(kDown & KEY_TOUCH))
		return;

	if (touch.py >= BTN_TOP) {
		for (int i = 0; i < NBTN; i++) {
			if (touch.px >= buttons[i].x && touch.px < buttons[i].x + buttons[i].w) {
				pressButton(i);
				return;
			}
		}
		return;
	}
	if (touch.py >= LIST_TOP && touch.py < LIST_BOT) {
		int row = (touch.py - LIST_TOP) / ROW_H + scroll;
		if (row >= 0 && row < pbCount()) {
			if (row == pbSelected() && connectFn)
				connectFn();       // tap the selected entry again to dial
			else
				pbSelect(row);
			confirmDel = 0;
		}
	}
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

	for (int i = 0; i < VISIBLE && scroll + i < n; i++) {
		int idx = scroll + i;
		const PbEntry* e = pbGet(idx);
		float y = LIST_TOP + i * ROW_H;
		if (idx == sel)
			C2D_DrawRectSolid(0, y, 0, SCREEN_W, ROW_H, 0xFF603010);

		snprintf(line, sizeof(line), "%-11.11s %.17s:%u", e->name, e->host,
		         e->port);
		termgfxDrawText(3, y + 1, 0.85f, 0xFFFFFFFF, line);

		// Right-aligned protocol tag + credential marker
		snprintf(line, sizeof(line), "%s%s",
		         e->proto == PROTO_RLOGIN ? "RLGN" : "TLNT",
		         e->user[0] ? "*" : " ");
		float protoX = SCREEN_W - 6 - termgfxTextWidth(0.85f, line);
		termgfxDrawText(protoX, y + 1,
		                0.85f, e->proto == PROTO_RLOGIN ? 0xFF66FF66 : 0xFFAAAAAA,
		                line);

		// 3D-capable boards: animated anaglyph border + tag (see KEY below)
		if (e->flags & PB_FLAG_3D) {
			u32 c = pulse3d();
			drawBorder(0, y, SCREEN_W, ROW_H, c);
			termgfxDrawText(protoX - 4 - termgfxTextWidth(0.85f, "3D"),
			                y + 1, 0.85f, c, "3D");
		}
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
