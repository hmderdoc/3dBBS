#include <3ds.h>
#include <string.h>
#include "ctrlin.h"
#include "../term/keymode.h"

#define STICK_DEAD 40      // circle-pad units before a direction counts
#define TOP_W 400.0f
#define TOP_H 240.0f

static CtrlSendFn sendFn;
static bool held[CI_COUNT];
static u64 heldSince[CI_COUNT];
static u64 lastRepeat[CI_COUNT];
static float ptrX = TOP_W / 2, ptrY = TOP_H / 2;
static bool ptrActive;
static bool clicked;

// 3DS button bit for each bindable input, or 0 for the stick-derived ones,
// which are resolved from the analogue reading instead.
static const u32 keyBit[CI_COUNT] = {
	KEY_A, KEY_B, KEY_X, KEY_Y,
	KEY_L, KEY_R, KEY_ZL, KEY_ZR,
	KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT,
	0, 0, 0, 0,
	0, 0, 0, 0,
};

void ctrlinInit(CtrlSendFn send)
{
	sendFn = send;
	memset(held, 0, sizeof(held));
	ptrX = TOP_W / 2;
	ptrY = TOP_H / 2;
}

static void emit(const Binding* b, KeyEdge edge)
{
	if (!sendFn || (!b->evdev && !b->nbytes))
		return;
	KeyEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.evdev = b->evdev;
	ev.bytes = b->bytes;
	ev.nbytes = b->nbytes;
	// A single-byte printable binding doubles as its own codepoint, which is
	// what the kitty path wants to report.
	if (b->nbytes == 1 && (u8)b->bytes[0] >= 0x20 && (u8)b->bytes[0] < 0x7F)
		ev.codepoint = (u8)b->bytes[0];
	ev.edge = edge;

	u8 out[64];
	int n = keymodeEncode(&ev, out, sizeof(out));
	if (n > 0)
		sendFn(out, n);
}

void ctrlinReleaseAll(void)
{
	const CtrlMap* m = cmGet(cmActive());
	for (int i = 0; i < CI_COUNT; i++) {
		if (held[i]) {
			held[i] = false;
			emit(&m->bind[i], KEY_RELEASE);
		}
	}
}

// Resolve one analogue stick into the four digital directions it stands for.
static void stickDirs(circlePosition p, bool out[4])
{
	out[0] = p.dy >  STICK_DEAD;   // up
	out[1] = p.dy < -STICK_DEAD;   // down
	out[2] = p.dx < -STICK_DEAD;   // left
	out[3] = p.dx >  STICK_DEAD;   // right
}

void ctrlinUpdate(u32 kDown, u32 kUp, u32 kHeld,
                  circlePosition circle, circlePosition cstick)
{
	const CtrlMap* m = cmGet(cmActive());
	u64 now = osGetTime();
	clicked = false;
	ptrActive = false;

	// Sticks in mouse mode drive the pointer and contribute no key edges.
	float mx = 0, my = 0;
	if (m->circle == STICK_MOUSE) {
		mx += circle.dx / 156.0f;
		my -= circle.dy / 156.0f;
		ptrActive = true;
	}
	if (m->cstick == STICK_MOUSE) {
		mx += cstick.dx / 156.0f;
		my -= cstick.dy / 156.0f;
		ptrActive = true;
	}
	if (ptrActive) {
		ptrX += mx * m->mouseSpeed;
		ptrY += my * m->mouseSpeed;
		if (ptrX < 0) ptrX = 0;
		if (ptrY < 0) ptrY = 0;
		if (ptrX > TOP_W - 1) ptrX = TOP_W - 1;
		if (ptrY > TOP_H - 1) ptrY = TOP_H - 1;
	}

	bool cdirs[4] = { false, false, false, false };
	bool sdirs[4] = { false, false, false, false };
	if (m->circle == STICK_DIGITAL) stickDirs(circle, cdirs);
	if (m->cstick == STICK_DIGITAL) stickDirs(cstick, sdirs);

	for (int i = 0; i < CI_COUNT; i++) {
		bool down;
		if (keyBit[i])
			down = (kHeld & keyBit[i]) != 0;
		else if (i >= CI_CUP && i <= CI_CRIGHT)
			down = cdirs[i - CI_CUP];
		else
			down = sdirs[i - CI_SUP];

		const Binding* b = &m->bind[i];

		if (down && !held[i]) {
			held[i] = true;
			heldSince[i] = now;
			lastRepeat[i] = now;
			emit(b, KEY_PRESS);
		} else if (!down && held[i]) {
			held[i] = false;
			emit(b, KEY_RELEASE);
		} else if (down && m->repeatDelayMs) {
			// Auto-repeat is the client's own invention: CTerm sends none on
			// the physical path, and a door holding a key expects to see one
			// press. Only the byte/kitty paths benefit, so repeats are
			// emitted as REPEAT edges and keymode decides what that means.
			u64 since = now - heldSince[i];
			if (since >= m->repeatDelayMs &&
			    now - lastRepeat[i] >= m->repeatRateMs) {
				lastRepeat[i] = now;
				emit(b, KEY_REPEAT);
			}
		}
	}

	// A pointer needs a click, and the only sensible source is a button the
	// user has not bound to anything else.
	if (ptrActive && (kDown & KEY_A) && !m->bind[CI_A].evdev &&
	    !m->bind[CI_A].nbytes)
		clicked = true;
	(void)kUp;
}

bool ctrlinPointer(float* x, float* y)
{
	if (x) *x = ptrX;
	if (y) *y = ptrY;
	return ptrActive;
}

bool ctrlinClicked(void) { return clicked; }
