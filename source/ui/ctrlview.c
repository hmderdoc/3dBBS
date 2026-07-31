#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ctrlview.h"
#include "ctrlmap.h"
#include "../gfx/termgfx.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define TITLE_H  20
#define ROW_H    18
#define BTN_H    28
#define BTN_TOP  (SCREEN_H - BTN_H)
#define LIST_TOP (TITLE_H + 2)
#define LIST_BOT BTN_TOP
#define VISIBLE  ((LIST_BOT - LIST_TOP) / ROW_H)

typedef enum { CV_CLOSED, CV_LIST, CV_EDIT, CV_PICK } CvScreen;

// Rows above the per-input bindings in the edit screen. Keeping them in the
// same scrolling list as the bindings avoids a fourth screen for six values.
enum { OPT_CIRCLE, OPT_CSTICK, OPT_CURSOR, OPT_DELAY, OPT_RATE, OPT_SPEED,
       OPT_COUNT };

static CvPromptFn promptFn;
static CvScreen screen;
static int sel, scroll;
static int editIdx;        // mapping being edited
static int pickInput;      // CtrlInput being bound
static int hot = -1;
static bool wasTouching;
static touchPosition held;
static char toast[48];
static int toastFrames;

static const char* stickName(u8 m)
{
	return m == STICK_MOUSE ? "Mouse" : m == STICK_DIGITAL ? "D-Pad" : "Off";
}

static void setToast(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(toast, sizeof(toast), fmt, ap);
	va_end(ap);
	toastFrames = 140;
}

void ctrlviewInit(CvPromptFn prompt) { promptFn = prompt; }
bool ctrlviewIsOpen(void) { return screen != CV_CLOSED; }

void ctrlviewOpen(void)
{
	screen = CV_LIST;
	sel = cmActive();
	scroll = 0;
	hot = -1;
}

static int rowCount(void)
{
	switch (screen) {
	case CV_LIST: return cmCount();
	case CV_EDIT: return OPT_COUNT + CI_COUNT;
	case CV_PICK: return cmActionCount();
	default:      return 0;
	}
}

static void ensureVisible(void)
{
	int n = rowCount();
	if (sel < 0) sel = 0;
	if (sel >= n) sel = n - 1;
	if (sel < scroll) scroll = sel;
	if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
	if (scroll < 0) scroll = 0;
}

// --- row text -------------------------------------------------------------

static void rowText(int i, char* left, int lcap, char* right, int rcap)
{
	right[0] = 0;
	if (screen == CV_LIST) {
		const CtrlMap* m = cmGet(i);
		snprintf(left, lcap, "%s", m->name);
		snprintf(right, rcap, "%s", i == cmActive() ? "IN USE" : "");
		return;
	}
	if (screen == CV_PICK) {
		snprintf(left, lcap, "%s", cmActionName(i));
		return;
	}
	// CV_EDIT
	CtrlMap* m = cmMutable(editIdx);
	if (i < OPT_COUNT) {
		switch (i) {
		case OPT_CIRCLE:
			snprintf(left, lcap, "Circle Pad");
			snprintf(right, rcap, "%s", stickName(m->circle));
			break;
		case OPT_CSTICK:
			snprintf(left, lcap, "C-Stick");
			snprintf(right, rcap, "%s", stickName(m->cstick));
			break;
		case OPT_CURSOR:
			snprintf(left, lcap, "Show Pointer");
			snprintf(right, rcap, "%s", m->showCursor ? "Yes" : "No");
			break;
		case OPT_DELAY:
			snprintf(left, lcap, "Repeat Delay");
			if (m->repeatDelayMs)
				snprintf(right, rcap, "%ums", m->repeatDelayMs);
			else
				snprintf(right, rcap, "Off");
			break;
		case OPT_RATE:
			snprintf(left, lcap, "Repeat Rate");
			snprintf(right, rcap, "%ums", m->repeatRateMs);
			break;
		case OPT_SPEED:
			snprintf(left, lcap, "Pointer Speed");
			snprintf(right, rcap, "%u", m->mouseSpeed);
			break;
		}
		return;
	}
	int in = i - OPT_COUNT;
	char tmp[24];
	snprintf(left, lcap, "%s", cmInputName((CtrlInput)in));
	snprintf(right, rcap, "%s", cmBindingLabel(&m->bind[in], tmp, sizeof(tmp)));
}

// --- actions --------------------------------------------------------------

static void cycleOption(int i, int dir)
{
	CtrlMap* m = cmMutable(editIdx);
	switch (i) {
	case OPT_CIRCLE: m->circle = (u8)((m->circle + 3 + dir) % 3); break;
	case OPT_CSTICK: m->cstick = (u8)((m->cstick + 3 + dir) % 3); break;
	case OPT_CURSOR: m->showCursor = !m->showCursor; break;
	case OPT_DELAY: {
		// 0 means off, and stepping past the top wraps back to it
		static const u16 steps[] = { 0, 250, 400, 600, 900 };
		int k = 0;
		for (int j = 0; j < 5; j++) if (steps[j] == m->repeatDelayMs) k = j;
		m->repeatDelayMs = steps[(k + 5 + dir) % 5];
		break;
	}
	case OPT_RATE: {
		static const u16 steps[] = { 30, 60, 100, 150, 250 };
		int k = 0;
		for (int j = 0; j < 5; j++) if (steps[j] == m->repeatRateMs) k = j;
		m->repeatRateMs = steps[(k + 5 + dir) % 5];
		break;
	}
	case OPT_SPEED:
		m->mouseSpeed = (u8)(m->mouseSpeed + dir);
		if (m->mouseSpeed < 1) m->mouseSpeed = 12;
		if (m->mouseSpeed > 12) m->mouseSpeed = 1;
		break;
	}
	cmDirty();
}

static void activateRow(int i)
{
	switch (screen) {
	case CV_LIST:
		cmSetActive(i);
		setToast("using %s", cmGet(i)->name);
		break;
	case CV_EDIT:
		if (i < OPT_COUNT) {
			cycleOption(i, 1);
		} else {
			pickInput = i - OPT_COUNT;
			screen = CV_PICK;
			sel = cmActionIndex(&cmMutable(editIdx)->bind[pickInput]);
			if (sel < 0) sel = 0;
			scroll = 0;
			ensureVisible();
		}
		break;
	case CV_PICK: {
		CtrlMap* m = cmMutable(editIdx);
		cmActionAt(i, &m->bind[pickInput]);
		cmDirty();
		setToast("%s = %s", cmInputName((CtrlInput)pickInput),
		         cmActionName(i));
		screen = CV_EDIT;
		sel = OPT_COUNT + pickInput;
		ensureVisible();
		break;
	}
	default:
		break;
	}
}

// Bottom buttons, per screen.
typedef struct { const char* label; float x, w; } Btn;
static Btn listBtns[] = {
	{ "USE", 0, 60 }, { "EDIT", 60, 64 }, { "RENAME", 124, 78 },
	{ "ADD", 202, 56 }, { "DEL", 258, 62 },
};
static Btn editBtns[] = { { "BACK", 0, 80 }, { "DONE", 240, 80 } };
static Btn pickBtns[] = { { "CANCEL", 0, 90 } };

static Btn* btns(int* n)
{
	if (screen == CV_LIST) { *n = 5; return listBtns; }
	if (screen == CV_EDIT) { *n = 2; return editBtns; }
	*n = 1;
	return pickBtns;
}

static void pressBtn(int i)
{
	int n;
	Btn* b = btns(&n);
	const char* lab = b[i].label;

	if (!strcmp(lab, "USE")) {
		cmSetActive(sel);
		setToast("using %s", cmGet(sel)->name);
	} else if (!strcmp(lab, "EDIT")) {
		editIdx = sel;
		screen = CV_EDIT;
		sel = 0;
		scroll = 0;
	} else if (!strcmp(lab, "RENAME")) {
		char nm[CM_NAME_MAX];
		snprintf(nm, sizeof(nm), "%s", cmGet(sel)->name);
		if (promptFn && promptFn("Mapping name", nm, sizeof(nm), false)) {
			snprintf(cmMutable(sel)->name, CM_NAME_MAX, "%s", nm);
			cmDirty();
		}
	} else if (!strcmp(lab, "ADD")) {
		char nm[CM_NAME_MAX] = "";
		if (promptFn && promptFn("New mapping name", nm, sizeof(nm), false)) {
			int k = cmAdd(nm);
			if (k >= 0) { sel = k; ensureVisible(); }
			else setToast("no room for another mapping");
		}
	} else if (!strcmp(lab, "DEL")) {
		if (cmCount() > 1) { cmDelete(sel); ensureVisible(); }
		else setToast("that is the only mapping");
	} else if (!strcmp(lab, "BACK") || !strcmp(lab, "CANCEL")) {
		screen = (screen == CV_PICK) ? CV_EDIT : CV_LIST;
		sel = (screen == CV_EDIT) ? OPT_COUNT + pickInput : editIdx;
		ensureVisible();
	} else if (!strcmp(lab, "DONE")) {
		cmFlush();
		screen = CV_CLOSED;
	}
}

static int btnAt(int px, int py)
{
	if (py < BTN_TOP)
		return -1;
	int n;
	Btn* b = btns(&n);
	for (int i = 0; i < n; i++)
		if (px >= b[i].x && px < b[i].x + b[i].w)
			return i;
	return -1;
}

void ctrlviewUpdate(u32 kDown, u32 kHeld, touchPosition touch)
{
	if (screen == CV_CLOSED)
		return;
	if (toastFrames > 0)
		toastFrames--;
	cmTick();

	bool touching = (kHeld & KEY_TOUCH) != 0;

	if (kDown & KEY_DUP)   { sel--; ensureVisible(); }
	if (kDown & KEY_DDOWN) { sel++; ensureVisible(); }
	// Left/right nudge an option without opening anything, which is the
	// natural gesture for a value that just cycles.
	if (screen == CV_EDIT && sel < OPT_COUNT) {
		if (kDown & KEY_DLEFT)  cycleOption(sel, -1);
		if (kDown & KEY_DRIGHT) cycleOption(sel, 1);
	}
	if (kDown & KEY_A) activateRow(sel);
	if (kDown & KEY_B) {
		if (screen == CV_LIST) { cmFlush(); screen = CV_CLOSED; }
		else if (screen == CV_PICK) { screen = CV_EDIT; ensureVisible(); }
		else { screen = CV_LIST; sel = editIdx; ensureVisible(); }
	}

	if (touching) {
		held = touch;
		hot = btnAt(held.px, held.py);
	} else if (wasTouching) {
		int b = btnAt(held.px, held.py);
		if (b >= 0) {
			pressBtn(b);
		} else if (held.py >= LIST_TOP && held.py < LIST_BOT) {
			int r = (held.py - LIST_TOP) / ROW_H + scroll;
			if (r >= 0 && r < rowCount()) {
				if (r == sel) activateRow(r);
				else { sel = r; ensureVisible(); }
			}
		}
		hot = -1;
	}
	wasTouching = touching;
}

void ctrlviewRender(void)
{
	if (screen == CV_CLOSED)
		return;

	C2D_DrawRectSolid(0, 0, 0, SCREEN_W, SCREEN_H, 0xFF101018);
	C2D_DrawRectSolid(0, 0, 0, SCREEN_W, TITLE_H, 0xFF202030);

	const char* title = screen == CV_LIST ? "Controller Mappings"
	                  : screen == CV_EDIT ? cmGet(editIdx)->name
	                                      : cmInputName((CtrlInput)pickInput);
	termgfxDrawText(4, 3, 0.8f, toastFrames > 0 ? 0xFF80FF80 : 0xFFCCCCCC,
	                toastFrames > 0 ? toast : title);

	int n = rowCount();
	char left[40], right[28];
	for (int i = 0; i < VISIBLE && scroll + i < n; i++) {
		int idx = scroll + i;
		float y = LIST_TOP + i * ROW_H;
		if (idx == sel)
			C2D_DrawRectSolid(0, y, 0, SCREEN_W, ROW_H - 1, 0xFF603010);
		rowText(idx, left, sizeof(left), right, sizeof(right));
		termgfxDrawText(5, y + 2, 0.8f,
		                idx == sel ? 0xFFFFFFFF : 0xFFBBBBBB, left);
		if (right[0]) {
			float w = termgfxTextWidth(0.8f, right);
			termgfxDrawText(SCREEN_W - 6 - w, y + 2, 0.8f,
			                idx == sel ? 0xFF88DDFF : 0xFF7799AA, right);
		}
	}

	if (n > VISIBLE) {
		float frac = (float)VISIBLE / n;
		float bh = (LIST_BOT - LIST_TOP) * frac;
		float by = LIST_TOP + (LIST_BOT - LIST_TOP) * ((float)scroll / n);
		C2D_DrawRectSolid(SCREEN_W - 3, by, 0, 3, bh, 0xFF808080);
	}

	int nb;
	Btn* b = btns(&nb);
	for (int i = 0; i < nb; i++) {
		C2D_DrawRectSolid(b[i].x + 1, BTN_TOP + 1, 0, b[i].w - 2, BTN_H - 2,
		                  i == hot ? 0xFFC08040 : 0xFF383838);
		float tw = termgfxTextWidth(0.8f, b[i].label);
		termgfxDrawText(b[i].x + (b[i].w - tw) / 2, BTN_TOP + 7, 0.8f,
		                0xFFFFFFFF, b[i].label);
	}
}
