#ifndef POWER_H
#define POWER_H

#include <3ds/types.h>
#include <stdbool.h>

// Lid-close keepalive.
//
// Closing the lid normally puts the console straight to sleep, which drops
// the TCP connection and ends the BBS session. libctru's aptSetSleepAllowed
// (false) makes the app refuse APT's sleep query, so the lid does nothing —
// but there is no "lid closed" event in that state, so the countdown is
// driven by polling ptm:u's shell state instead.
//
// While the hold is active the console is fully awake with the screens
// dark: it costs real battery. That is why the window is bounded and
// configurable (settings.txt, lid_keepalive_min).

void powerInit(void);
void powerExit(void);

// Diagnostics for the dev overlay. These failures all look identical from
// outside — the feature simply appears to be off — so they are reported
// rather than inferred. lastShutMs/lastShutFrames cover the previous
// lid-closed stretch: frames ≈ 60 per second means the app really did keep
// running, and near zero means the console slept regardless.
typedef struct {
	bool ptmOk;          // ptm:u open (supplies the lid state)
	bool ndmOk;          // ndm:u open and the infrastructure lock held
	bool sleepBlocked;   // we are currently refusing sleep
	bool lidShut;
	u32 lastShutMs, lastShutFrames;
} PowerStatus;

void powerStatus(PowerStatus* s);

// Call once per frame. `connected` gates the whole feature: sleep is only
// blocked while a session is live. `keepaliveMin` of 0 disables it (stock
// sleep behaviour). Returns true while the lid is shut and the hold is
// keeping the session up — the caller can skip rendering work nobody can
// see.
bool powerUpdate(bool connected, int keepaliveMin);

#endif
