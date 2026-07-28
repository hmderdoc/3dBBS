#ifndef PHONEBOOK_H
#define PHONEBOOK_H

#include <3ds/types.h>

// Dialing directory, persisted at sdmc:/3dBBS/phonebook.txt
// Format: one entry per line, "name|host|port" ('#' starts a comment).
// The file is created with defaults on first run; edit it on the SD card
// (or via a future in-app editor) to add boards.

#define PB_MAX 32

typedef struct {
	char name[32];
	char host[64];
	u16 port;
} PbEntry;

void pbLoad(void);
int pbCount(void);
const PbEntry* pbGet(int i);
int pbSelected(void);
void pbSelectNext(void);
void pbSelectPrev(void);

#endif
