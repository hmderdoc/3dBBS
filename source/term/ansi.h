#ifndef ANSI_H
#define ANSI_H

#include "termbuf.h"

// ANSI-BBS escape sequence parser, implemented against the CTerm spec
// (vendor/synchronet/cterm.txt). Feeds a Terminal and dispatches string
// sequences (APC/DCS/OSC) to handlers — the extension points for the
// 3D protocol, sixel, and sound.

#define ANSI_MAX_PARAMS 16
#define ANSI_STR_MAX    131072 // APC/DCS payload cap (C;S file stores, audio blobs)

typedef enum { ANSI_STR_APC, ANSI_STR_DCS, ANSI_STR_OSC } AnsiStrKind;

typedef struct AnsiParser AnsiParser;

typedef struct {
	// Terminal replies (DSR/DA responses) go here -> telnetSend
	void (*respond)(const u8* data, int len);
	// BBS requested a resize via CSI 8;rows;cols t (0 = "restore default")
	void (*resize)(int cols, int rows);
	// Completed APC/DCS/OSC string sequence
	void (*string)(AnsiStrKind kind, const u8* data, int len);
	// ANSI music string (CSI M / CSI N / CSI |)
	void (*music)(const u8* data, int len);
	// CSI = 7 [; ch] n audio channel-state query (channel -1 = all)
	void (*audioStatus)(int channel);
} AnsiHooks;

struct AnsiParser {
	Terminal* term;
	AnsiHooks hooks;

	int state;
	int params[ANSI_MAX_PARAMS];
	int nParams;
	bool paramSeen;
	char priv;          // private marker: '<', '=', '?', '>' or 0
	char intermediate;  // e.g. ' ' in CSI Ps SP D

	u8* strBuf;         // ANSI_STR_MAX, heap
	int strLen;
	AnsiStrKind strKind;
};

bool ansiInit(AnsiParser* p, Terminal* term, const AnsiHooks* hooks);
void ansiFree(AnsiParser* p);
void ansiFeed(AnsiParser* p, const u8* data, int len);

// We identify as CTerm in Device Attributes replies so existing BBS-side
// detection (e.g. Synchronet's '*' terminal autodetect, which probes with a
// plain CSI 0 c) recognizes us. Revision matches the cterm.c source our
// behavior is implemented against ($Revision: 1.332$). 3DS-specific
// identification is done via APC (SyncTERM:VER / 3DS:Query).
#define ANSI_CTERM_DA "\x1B[=67;84;101;114;109;1;332c"

#endif
