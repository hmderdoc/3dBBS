#ifndef CTRLMAP_H
#define CTRLMAP_H

#include <3ds/types.h>
#include <stdbool.h>
#include "../term/keymode.h"

// Named controller mappings, persisted at sdmc:/3dBBS/controls.txt.
//
// Every binding carries BOTH an evdev keycode and a translated byte
// sequence, because the far end decides which it gets: a door that enabled
// physical key reports wants `CSI = Pk K`/`k` edges, while an ordinary BBS
// prompt wants the bytes. One binding serves both rungs of the ladder in
// keymode.h, so a mapping does not have to be re-authored per protocol.
//
// The evdev code is the more useful identity of the two — it is layout
// independent and reports release as well as press, which is what
// hold-to-move needs.

// Physical inputs that can be bound. START and SELECT are deliberately
// absent: START opens the menu and SELECT cycles the display mode, and if a
// mapping could capture them a bad binding would lock the user out.
typedef enum {
	CI_A, CI_B, CI_X, CI_Y,
	CI_L, CI_R, CI_ZL, CI_ZR,
	CI_DUP, CI_DDOWN, CI_DLEFT, CI_DRIGHT,
	CI_CUP, CI_CDOWN, CI_CLEFT, CI_CRIGHT,       // circle pad as digital
	CI_SUP, CI_SDOWN, CI_SLEFT, CI_SRIGHT,       // C-stick as digital
	CI_COUNT
} CtrlInput;

// What an analogue stick does. Digital emits the four direction bindings;
// mouse moves the pointer and leaves the bindings alone.
typedef enum { STICK_OFF, STICK_DIGITAL, STICK_MOUSE } StickMode;

typedef struct {
	u16 evdev;        // EVDEV_KEY_* code, 0 = unbound
	char bytes[12];   // translated sequence, "" = send nothing
	u8 nbytes;
} Binding;

#define CM_NAME_MAX 20
#define CM_MAX 8          // mappings the file may hold

typedef struct {
	char name[CM_NAME_MAX];
	Binding bind[CI_COUNT];
	u8 circle;        // StickMode
	u8 cstick;        // StickMode
	bool showCursor;  // draw a pointer when a stick is in mouse mode
	u16 repeatDelayMs;// 0 disables auto-repeat entirely
	u16 repeatRateMs;
	u8 mouseSpeed;    // pixels per frame at full deflection
} CtrlMap;

void cmLoad(void);
int  cmCount(void);
const CtrlMap* cmGet(int i);
int  cmActive(void);
void cmSetActive(int i);
CtrlMap* cmMutable(int i);      // edit in place; call cmDirty() after
void cmDirty(void);
void cmTick(void);              // deferred write, like the phonebook
void cmFlush(void);
int  cmAdd(const char* name);   // clone of the active map; -1 if full
void cmDelete(int i);           // refuses to delete the last one

// Presentation helpers for the editor.
const char* cmInputName(CtrlInput in);
const char* cmBindingLabel(const Binding* b, char* buf, int cap);

// The catalogue of things a control can be bound to, in editor order.
int  cmActionCount(void);
void cmActionAt(int i, Binding* out);
const char* cmActionName(int i);
// Index of the action matching this binding, or -1 for "custom".
int  cmActionIndex(const Binding* b);

#endif
