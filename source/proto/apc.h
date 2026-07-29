#ifndef APC_H
#define APC_H

#include <3ds/types.h>
#include "../term/ansi.h"

// APC command channel dispatcher. Two namespaces:
//
//   SyncTERM:  — wire-compatible subset of SyncTerm's APC protocol
//     C;S;<name>;<b64>   store file in per-host SD cache
//     C;L[;<glob>]       list cache: replies APC "SyncTERM:C;L\n<fn>\t<md5>\n..."
//     A;*                audio verbs (see audio/apcaudio.h)
//     Q;*                feature queries
//     VER                replies APC SyncTERM:VER;3dBBS <ver> ST
//
//   3DS:  — our own namespace (3D protocol lands here in Phase 3)
//     Query              replies APC 3DS:Ver;<maj>;<min> ST
//
// Cache lives at sdmc:/3dBBS/cache/<host>/.

#define APC_VER_STRING "3dBBS 0.2"
#define APC_3DS_MAJOR 0
#define APC_3DS_MINOR 3   // 0.3: text depth layers (CSI = z)

typedef void (*ApcRespondFn)(const u8* data, int len);

// apcInit spawns a worker thread (core 1 when available) — all APC jobs
// (SD I/O, base64/WAV decode, audio verbs) execute there in FIFO order so
// the main loop never stalls on them.
void apcInit(ApcRespondFn respond);
void apcExit(void);                  // joins the worker thread
void apcSetHost(const char* host);   // call on connect; scopes the cache
void apcReset(void);                 // call on disconnect; drops pending jobs
// Enqueue a string sequence for the worker. APC = protocol commands;
// DCS = sixel (decoded off-thread, anchored at the given cursor cell).
void apcHandle(AnsiStrKind kind, const u8* data, int len, int cellX, int cellY);

// RAM-first lookup of a recently C;S-stored file (worker thread only).
// Returns NULL if not in the RAM cache (fall back to the SD path).
const u8* apcCacheGet(const char* name, int* len);

// Dev telemetry: current worker job backlog + sixel pipeline entry counters
void apcDebugStats(u32* jobs, u32* bytes);
void apcSixelStats(u32* seen, u32* decodeFail);

#endif
