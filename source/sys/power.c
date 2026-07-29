#include <3ds.h>
#include "power.h"

static bool ptmOk;
static bool ndmOk;           // ndm:u opened AND the lock was taken
static bool sleepBlocked;    // mirrors aptSetSleepAllowed(false)
static bool lidShut;
static u64 shutAt;           // osGetTime() when the lid closed, 0 = open
static int pollCountdown;

// Accounting for the last lid-closed stretch, so "did we actually keep
// running?" is answerable after the fact instead of inferred from what the
// screen looked like on reopening.
static u32 shutFrames, lastShutFrames;
static u32 lastShutMs;

// The MCU shell state doesn't change fast enough to be worth an IPC call
// every frame; twice a second is plenty and keeps the service traffic low.
#define POLL_FRAMES 30

static void allowSleep(bool allow)
{
	if (sleepBlocked == !allow)
		return;
	// libctru replies APTREPLY_REJECT to the sleep query while this is
	// off, which is the whole mechanism — it needs no service of its own.
	aptSetSleepAllowed(allow);
	sleepBlocked = !allow;
}

void powerInit(void)
{
	ptmOk = R_SUCCEEDED(ptmuInit());

	// Refusing sleep only stops the *console* suspending. The network
	// daemon manager can still tear the infrastructure connection down on
	// its own — which drops the socket while the app is still happily
	// running. Taking the exclusive state and locking it is what stops
	// that; ftpd, the reference long-running 3DS network app, pairs the
	// two for exactly this reason (source/3ds/platform.cpp).
	if (R_SUCCEEDED(ndmuInit())) {
		if (R_SUCCEEDED(NDMU_EnterExclusiveState(
		                    NDM_EXCLUSIVE_STATE_INFRASTRUCTURE)) &&
		    R_SUCCEEDED(NDMU_LockState()))
			ndmOk = true;
		else
			ndmuExit();
	}
}

void powerExit(void)
{
	allowSleep(true);
	if (ndmOk) {
		NDMU_UnlockState();
		NDMU_LeaveExclusiveState();
		ndmuExit();
		ndmOk = false;
	}
	if (ptmOk)
		ptmuExit();
	ptmOk = false;
}

void powerStatus(PowerStatus* s)
{
	s->ptmOk = ptmOk;
	s->ndmOk = ndmOk;
	s->sleepBlocked = sleepBlocked;
	s->lidShut = lidShut;
	s->lastShutMs = lastShutMs;
	s->lastShutFrames = lastShutFrames;
}

bool powerUpdate(bool connected, int keepaliveMin)
{
	if (keepaliveMin <= 0 || !connected) {
		allowSleep(true);
		lidShut = false;
		shutAt = 0;
		return false;
	}

	// Sleep has to be refused *before* the lid moves — once the system has
	// been allowed to sleep there is no chance to change our mind. This is
	// deliberately NOT conditional on ptm:u: blocking sleep is the part
	// that keeps the session alive, and it works with no service at all.
	// ptm:u only supplies the lid state that ends the hold early.
	allowSleep(false);

	if (!ptmOk) {
		// No shell state to poll, so there is nothing to time against:
		// hold for as long as the session lasts and release on hangup.
		// Costs battery, which is why it is worth knowing when ptm:u is
		// missing (see powerStatus and the dev overlay).
		return false;
	}

	if (--pollCountdown <= 0) {
		pollCountdown = POLL_FRAMES;
		u8 shell = 1;   // 1 = open, 0 = closed
		if (R_SUCCEEDED(PTMU_GetShellState(&shell)))
			lidShut = (shell == 0);
	}

	if (!lidShut) {
		if (shutAt) {
			lastShutMs = (u32)(osGetTime() - shutAt);
			lastShutFrames = shutFrames;
		}
		shutAt = 0;
		shutFrames = 0;
		return false;
	}

	u64 now = osGetTime();
	if (shutAt == 0) {
		shutAt = now;
		shutFrames = 0;
	}
	shutFrames++;

	if (now - shutAt >= (u64)keepaliveMin * 60000ULL) {
		// Window expired: hand sleep back. The lid is still shut, so the
		// system suspends on the next query — exactly as if we had never
		// interfered.
		allowSleep(true);
	}
	return true;
}
