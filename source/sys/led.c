#include <3ds.h>
#include <string.h>
#include "led.h"

static bool mcuOk;
static float rate;           // bytes/sec, peak-meter ballistics
static int level = -1;       // last uploaded activity level
static int refresh;          // frames until the pattern is re-asserted

// Activity levels by receive rate. The top of the scale is the ~45 KB/s an
// 8 KB window allows from a distant board (DESIGN.md §7.5), so a WAN
// session at full tilt actually reaches red instead of sitting mid-scale.
static const u32 levelBps[] = { 256, 2048, 8192, 24576, 40960 };
#define NLEVELS ((int)(sizeof(levelBps) / sizeof(levelBps[0])) + 1)

// Load reads as colour, VU-meter style: green idle, through amber, to red
// at the ceiling. Hue is CONSTANT within a pattern — an earlier version
// swept the whole spectrum across the 32 steps and let the MCU smooth
// between them, which averages the colour wheel and comes out white.
static const u8 levelRgb[NLEVELS][3] = {
	{   0, 255,  40 },   // idle          green
	{   0, 255,   0 },   // trickle       green
	{ 160, 255,   0 },   // light         yellow-green
	{ 255, 190,   0 },   // moderate      amber
	{ 255,  90,   0 },   // heavy         orange
	{ 255,   0,   0 },   // at the wire   red
};

// Pulses packed into the 32-step pattern, and the per-step delay (1/16 s).
// Together these set the throb rate: 32/pulses steps at delay/16 s each, so
// idle breathes about once every 8s and a full-rate transfer flickers at
// roughly 2 Hz.
static const u8 levelPulses[NLEVELS] = { 1, 1, 2, 2, 4, 4 };
static const u8 levelDelay[NLEVELS]  = { 4, 3, 2, 2, 1, 1 };
static const u8 levelPeak[NLEVELS]   = { 70, 130, 180, 220, 240, 255 };

#define REFRESH_FRAMES 120

// One pulse: instant attack, quadratic decay to a floor. Quadratic reads
// as a snap-and-fade rather than the even ramp a linear decay gives.
static u8 envelope(int step, int stepsPerPulse)
{
	int t = step % stepsPerPulse;
	int n = stepsPerPulse - 1;
	if (n <= 0)
		return 255;
	int rem = n - t;
	return (u8)(25 + (230 * rem * rem) / (n * n));
}

static void upload(int lv)
{
	InfoLedPattern p;
	memset(&p, 0, sizeof(p));

	if (lv < 0) {
		// All-zero pattern played once: LED dark, MCU idle.
		p.delay = 0x10;
		p.loopDelay = 0xFF;
		MCUHWC_SetInfoLedPattern(&p);
		return;
	}

	int steps = 32 / levelPulses[lv];
	p.delay = levelDelay[lv];
	// Light smoothing rounds the decay without blunting the attack. Safe
	// at any value now that a pattern only ever holds one hue.
	p.smoothing = 0x10;
	p.loopDelay = 0;   // loop continuously
	p.blinkSpeed = 0;

	for (int i = 0; i < 32; i++) {
		u32 env = envelope(i, steps) * levelPeak[lv] / 255;
		p.redPattern[i]   = (u8)(levelRgb[lv][0] * env / 255);
		p.greenPattern[i] = (u8)(levelRgb[lv][1] * env / 255);
		p.bluePattern[i]  = (u8)(levelRgb[lv][2] * env / 255);
	}
	MCUHWC_SetInfoLedPattern(&p);
}

void ledInit(void)
{
	mcuOk = R_SUCCEEDED(mcuHwcInit());
}

bool ledOk(void) { return mcuOk; }

void ledExit(void)
{
	if (mcuOk) {
		upload(-1);   // give the system back a dark notification LED
		mcuHwcExit();
	}
	mcuOk = false;
}

void ledUpdate(bool connected, u32 rxBytesThisFrame)
{
	if (!mcuOk)
		return;

	if (!connected) {
		rate = 0.0f;
		if (level != -1) {
			level = -1;
			upload(-1);
		}
		return;
	}

	// Peak-meter ballistics: jump almost straight to a burst, fall back
	// slowly. A BBS screen paint is a few KB in a fraction of a second —
	// an even average washes it out entirely, while a fast attack makes
	// every keypress visibly kick the colour up the scale.
	float inst = (float)rxBytesThisFrame * 60.0f;
	rate += (inst > rate ? 0.5f : 0.03f) * (inst - rate);

	int lv = 0;
	while (lv < NLEVELS - 1 && (u32)rate >= levelBps[lv])
		lv++;

	// Re-assert periodically as well as on change: the pattern is MCU-side
	// state anything on the system could overwrite, and one I2C write every
	// two seconds is cheap insurance against a stuck colour.
	if (lv != level || --refresh <= 0) {
		level = lv;
		refresh = REFRESH_FRAMES;
		upload(lv);
	}
}
