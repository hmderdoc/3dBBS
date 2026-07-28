#ifndef APCAUDIO_H
#define APCAUDIO_H

#include <3ds/types.h>
#include <stdbool.h>

// SyncTERM-compatible APC audio engine (cterm.adoc "SyncTERM:A;*" verbs),
// mapped onto ndsp. 256 patch slots of mono PCM16; channels 2-15 are
// BBS-queueable (0/1 are reserved for cterm music/fx, as in SyncTerm).
//
// Requires sdmc:/3ds/dspfirm.cdc; without it audioInit() reports false and
// everything is a silent no-op (matching the spec's "errors are silent").

bool audioInit(void (*respond)(const u8* data, int len));
void audioExit(void);
void audioSetCacheDir(const char* dir);  // where A;Load finds C;S-stored files
void audioReset(void);                   // flush channels, free patches
void audioPoll(void);                    // call once per frame

void audioHandleA(const char* cmd, int len);  // text after "SyncTERM:A;"
void audioHandleQ(const char* cmd);           // text after "SyncTERM:Q;"
void audioStatusQuery(int channel);           // CSI = 7 [; ch] n  (-1 = all)

// Dev telemetry: bitmask of playing channels + cumulative drain notifies
void audioDebugStats(u32* playingMask, u32* drainCount);

#endif
