#ifndef PHONEBOOK_H
#define PHONEBOOK_H

#include <3ds/types.h>
#include "../net/telnet.h"

// Dialing directory, persisted at sdmc:/3dBBS/phonebook.txt
//   name|host|port|proto|user|pass|flags|size   ('#' starts a comment)
// Trailing fields are optional: 3 fields = telnet with no credentials.
// proto is "telnet", "rlogin", or "ssh". flags is free text; "3d" marks a
// board known to drive the 3DS: scene protocol (tracked locally — it is a
// note on our list, not something probed from the wire). size is "COLSxROWS"
// (e.g. "80x50"); empty or absent means the 80x25 default.
//
// SECURITY: credentials are stored in PLAIN TEXT on the SD card (the same
// as SyncTerm's syncterm.lst). Anyone with the card can read them. Leave
// the password empty for boards where that matters.

#define PB_MAX 32

#define PB_FLAG_3D 0x01   // board drives the 3DS: 3D scene protocol

// Terminal geometry the client requests for this board. 0x0 means "use the
// default", which keeps entries written before this field existed working.
#define PB_DEF_COLS 80
#define PB_DEF_ROWS 25

// Hard ceiling on requested geometry. 132x60 is the largest screen CTerm
// documents (cterm.adoc), and the renderer's vertex budget is sized from
// these two numbers — a hand-edited file asking for more is clamped, not
// honoured.
#define PB_MAX_COLS 132
#define PB_MAX_ROWS 60
#define PB_MIN_COLS 20
#define PB_MIN_ROWS 5

typedef struct {
	char name[32];
	char host[64];
	u16 port;
	ConnProto proto;
	char user[32];
	char pass[32];
	u8 flags;
	u16 cols, rows;   // 0 = PB_DEF_COLS x PB_DEF_ROWS
} PbEntry;

void pbLoad(void);
int pbCount(void);
const PbEntry* pbGet(int i);
int pbSelected(void);
void pbSelectNext(void);
void pbSelectPrev(void);

// Update credentials for entry i and persist the whole phonebook
void pbSetCreds(int i, const char* user, const char* pass);
// Cycle entry i telnet -> rlogin -> ssh (default ports follow while the
// entry still sits on the previous protocol's default) and persist
void pbToggleProto(int i);

// Terminal geometry for entry i. pbSetSize stores any size, clamped to
// PB_MIN/PB_MAX; 0x0 (or the default itself) restores the default.
void pbSetSize(int i, u16 cols, u16 rows);

// The offered sizes: the SyncTERM screen modes, which top out at the
// 132x60 CTerm documents as its largest screen. The UI presents these as a
// list — there are thirteen of them, which is far too many to cycle
// through one press at a time.
int pbSizePresetCount(void);
void pbSizePreset(int k, u16* cols, u16* rows);

// Effective geometry for entry i, with the 0x0 default resolved.
void pbSizeOf(int i, u16* cols, u16* rows);

// Editing (all persist immediately)
void pbSetEntry(int i, const char* name, const char* host, u16 port);
int pbAdd(const char* name, const char* host, u16 port);  // -> index or -1
void pbDelete(int i);      // refuses to delete the last remaining entry
void pbSelect(int i);

// Edits are buffered: pbTick() (call once per frame) writes the file after
// a short quiet period, so taps never stall on SD I/O. pbFlush() forces it.
void pbTick(void);
void pbFlush(void);

#endif
