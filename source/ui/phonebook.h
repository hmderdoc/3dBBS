#ifndef PHONEBOOK_H
#define PHONEBOOK_H

#include <3ds/types.h>
#include "../net/telnet.h"

// Dialing directory, persisted at sdmc:/3dBBS/phonebook.txt
//   name|host|port|proto|user|pass       ('#' starts a comment)
// Trailing fields are optional: 3 fields = telnet with no credentials.
// proto is "telnet" or "rlogin".
//
// SECURITY: credentials are stored in PLAIN TEXT on the SD card (the same
// as SyncTerm's syncterm.lst). Anyone with the card can read them. Leave
// the password empty for boards where that matters.

#define PB_MAX 32

typedef struct {
	char name[32];
	char host[64];
	u16 port;
	ConnProto proto;
	char user[32];
	char pass[32];
} PbEntry;

void pbLoad(void);
int pbCount(void);
const PbEntry* pbGet(int i);
int pbSelected(void);
void pbSelectNext(void);
void pbSelectPrev(void);

// Update credentials for entry i and persist the whole phonebook
void pbSetCreds(int i, const char* user, const char* pass);
// Switch entry i between telnet/rlogin (rlogin defaults to port 513 when
// the entry still has the telnet default) and persist
void pbToggleProto(int i);

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
