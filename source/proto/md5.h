#ifndef MD5_H
#define MD5_H

#include <3ds/types.h>
#include <stddef.h>

// Minimal MD5 (RFC 1321) — used for the SyncTERM:C;L cache listing so
// BBS-side code can dedup uploads by hash.

typedef struct {
	u32 state[4];
	u64 count;      // bytes processed
	u8 buf[64];
} Md5Ctx;

void md5Init(Md5Ctx* c);
void md5Update(Md5Ctx* c, const u8* data, size_t len);
void md5Final(Md5Ctx* c, u8 digest[16]);

#endif
