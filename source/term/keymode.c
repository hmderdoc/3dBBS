#include <stdio.h>
#include <string.h>
#include "keymode.h"

static bool physical;     // CSI = 1 h
static bool suppress;     // CSI = 2 h

// kitty keeps a stack of flag sets so nested programs can push their own and
// pop back to whatever the shell had. Depth is bounded; overflow drops the
// oldest rather than refusing, since a door that pushes without popping must
// not wedge the keyboard permanently.
#define KITTY_DEPTH 8
static u32 kittyStack[KITTY_DEPTH];
static int kittyTop;      // 0 = nothing pushed

// kitty flag bits we act on
#define KF_DISAMBIGUATE 1
#define KF_EVENT_TYPES  2
#define KF_REPORT_ALL   8

void keymodeReset(void)
{
	physical = false;
	suppress = false;
	kittyTop = 0;
}

void keymodeSetPhysical(bool on)
{
	physical = on;
	// "Disabling this mode ... clears CTerm's remembered physical-key state"
	// and emits no release reports, so anything tracking held keys upstream
	// is expected to drop them itself.
}

void keymodeSetSuppress(bool on) { suppress = on; }

void keymodePushKitty(u32 flags)
{
	if (kittyTop >= KITTY_DEPTH) {
		memmove(&kittyStack[0], &kittyStack[1],
		        sizeof(kittyStack[0]) * (KITTY_DEPTH - 1));
		kittyTop = KITTY_DEPTH - 1;
	}
	kittyStack[kittyTop++] = flags;
}

void keymodePopKitty(void)
{
	if (kittyTop > 0)
		kittyTop--;
}

void keymodeSetKitty(u32 flags, int mode)
{
	u32 cur = keymodeKittyFlags();
	u32 next;
	switch (mode) {
	case 2:  next = cur | flags; break;    // set given bits
	case 3:  next = cur & ~flags; break;   // clear given bits
	default: next = flags; break;          // 1 (or absent): replace all
	}
	if (kittyTop == 0)
		kittyStack[kittyTop++] = next;
	else
		kittyStack[kittyTop - 1] = next;
}

u32 keymodeKittyFlags(void)
{
	return kittyTop > 0 ? kittyStack[kittyTop - 1] : 0;
}

bool keymodePhysical(void) { return physical; }
bool keymodeSuppress(void) { return suppress; }

int keymodeEncode(const KeyEvent* ev, u8* out, int cap)
{
	int n = 0;
	if (!ev || cap < 32)
		return 0;

	// Physical reports come first and are independent of translation: the
	// spec is explicit that they describe key identity *before* normal
	// character translation and do not replace it unless suppression is on.
	if (physical && ev->evdev) {
		n += snprintf((char*)out + n, cap - n, "\x1B[=%u%c",
		              (unsigned)ev->evdev,
		              ev->edge == KEY_RELEASE ? 'k' : 'K');
		// A repeat is a fresh press edge as far as evdev reporting goes;
		// CTerm itself sends no auto-repeat, so a held key is one press.
		if (ev->edge == KEY_REPEAT)
			return n;
	}

	if (suppress)
		return n;   // far end wants the physical stream only

	u32 kf = keymodeKittyFlags();
	if (kf) {
		// CSI unicode ; modifiers : event-type u. Modifiers are encoded as
		// the bitmask plus one; the event type is only meaningful when the
		// far end asked for event reporting.
		if (ev->edge != KEY_PRESS && !(kf & KF_EVENT_TYPES))
			return n;   // it only wants presses
		u32 cp = ev->codepoint ? ev->codepoint : ev->evdev;
		if (!cp)
			return n;
		if (ev->mods || (kf & KF_EVENT_TYPES))
			n += snprintf((char*)out + n, cap - n, "\x1B[%lu;%u:%uu",
			              (unsigned long)cp, ev->mods + 1, ev->edge);
		else
			n += snprintf((char*)out + n, cap - n, "\x1B[%luu",
			              (unsigned long)cp);
		return n;
	}

	// Plain terminal: presses (and repeats) only, as ordinary bytes.
	if (ev->edge != KEY_RELEASE && ev->bytes && ev->nbytes > 0 &&
	    n + ev->nbytes <= cap) {
		memcpy(out + n, ev->bytes, ev->nbytes);
		n += ev->nbytes;
	}
	return n;
}
