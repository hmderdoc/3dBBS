#ifndef KEYMODE_H
#define KEYMODE_H

#include <3ds/types.h>
#include <stdbool.h>

// Keyboard reporting modes, and the encoder that turns one key edge into the
// bytes the far end asked for.
//
// A plain terminal sends bytes on press and nothing on release, which is
// useless for anything wanting hold-to-move. Two protocols fix that, and the
// Synchronet door library (src/doors/termgfx/keymode.c) climbs them in this
// order — so a terminal claiming SyncTERM compatibility implements the same
// ladder:
//
//   evdev   The SyncTERM-native path, and the one that matters here. CTDA
//           advertises capability 8; the far end enables reports with
//           `CSI = 1 h` and optionally suppresses the translated byte stream
//           with `CSI = 2 h`. Edges go back as `CSI = Pk[;Pk...] K` (press)
//           and `... k` (release), where Pk is an EVDEV_KEY_* code. The
//           identity is a physical keycode, so it is layout-independent.
//
//   kitty   For terminals that answer the `CSI ? u` progressive-enhancement
//           query. The far end pushes flags with `CSI > Ps u` and pops with
//           `CSI < u`; edges go back as CSI-u events carrying an explicit
//           press/repeat/release type.
//
//   bytes   Neither enabled: the ordinary translated sequences, press only.
//
// All of this is specified in vendor/synchronet/cterm.adoc (CTDA capability
// list, `CSI = 1 h`, `CSI = 2 h`, and the report forms).

typedef enum {
	KEY_PRESS = 1,
	KEY_REPEAT = 2,
	KEY_RELEASE = 3,
} KeyEdge;

// Modifier bits, matching the kitty encoding's bit order.
#define KEYMOD_SHIFT 1
#define KEYMOD_ALT   2
#define KEYMOD_CTRL  4

typedef struct {
	u16 evdev;         // EVDEV_KEY_* code, 0 if this key has no physical id
	u32 codepoint;     // unicode for kitty, 0 if not a character key
	const char* bytes; // translated sequence for the legacy path (may be NULL)
	int nbytes;
	u8 mods;
	KeyEdge edge;
} KeyEvent;

void keymodeReset(void);          // connection teardown: back to plain bytes

// Set by the parser from the far end's requests.
void keymodeSetPhysical(bool on); // CSI = 1 h / l
void keymodeSetSuppress(bool on); // CSI = 2 h / l
void keymodePushKitty(u32 flags); // CSI > Ps u
void keymodePopKitty(void);       // CSI < u
void keymodeSetKitty(u32 flags, int mode);  // CSI = Ps ; Pm u
u32  keymodeKittyFlags(void);

bool keymodePhysical(void);
bool keymodeSuppress(void);

// Encode one edge into `out`; returns bytes written (0 = nothing to send,
// which is normal for a release with no protocol that reports releases).
int keymodeEncode(const KeyEvent* ev, u8* out, int cap);

#endif
