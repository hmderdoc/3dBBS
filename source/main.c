// 3dBBS — Phase 1.5
// Display modes (SELECT cycles):
//   keyboard - terminal on top, touch keyboard on bottom (primary mode)
//   mirror   - terminal on both screens; taps on the bottom send mouse clicks
//   tall     - one terminal spanning both screens (80x60 at default width)
// D-pad up/down cycles the phonebook while disconnected; tap status bar to dial.

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx/scene3d.h"
#include "gfx/termgfx.h"
#include "term/termbuf.h"
#include "term/ansi.h"
#include "net/sock.h"
#include "net/telnet.h"
#include "ui/kbd.h"
#include "ui/phonebook.h"
#include "ui/pbview.h"
#include "ui/menu.h"
#include "ui/ctrlmap.h"
#include "ui/ctrlin.h"
#include "term/keymode.h"
#include "proto/apc.h"
#include "audio/apcaudio.h"
#include "term/palette.h"
#include "net/beacon.h"
#include "net/shot.h"
#include "gfx/siximg.h"
#include "gfx/tdfsplash.h"
#include "sys/settings.h"
#include "sys/power.h"
#include "sys/led.h"

#define DEV_MAC_IP "192.168.1.61"  // dev telemetry collector (see tests/)

enum { MODE_KBD, MODE_MIRROR, MODE_TALL, MODE_COUNT };

enum { NET_IDLE, NET_CONNECTING, NET_CONNECTED, NET_FAILED, NET_CLOSED };

static Terminal term;
static AnsiParser parser;
static bool netOk;
static int mode = MODE_KBD;
static int netState = NET_IDLE;
static bool connectPending;

// Geometry this session is configured for: the dialed board's preference,
// or whatever the BBS later asked for. MODE_TALL computes its own row count
// from the screen, so this is what we restore when leaving it.
static int cfgCols = PB_DEF_COLS, cfgRows = PB_DEF_ROWS;

#ifndef RELEASE_BUILD
// Multi-view capture. R sweeps the eye offset across SHOT_VIEWS renders with
// everything else frozen, which the host turns into a parallax loop. Two
// views alone cannot do this: a 2-frame wiggle at real stereo disparity
// reads as a glitch, not as depth.
#define SHOT_VIEWS 16
#define SHOT_IOD   0.25f
static int shotStep = -1;   // -1 idle, else the view being rendered
#endif

// --- parser hooks ---

static void hookRespond(const u8* data, int len)
{
	telnetSend(data, len);
}

static void applyTallRows(void)
{
	// Width-fit scale across the narrower (bottom) screen; rows fill 2x240px
	float s = 320.0f / (term.cols * 8);
	int rows = (int)(480.0f / (16.0f * s));
	termResize(&term, term.cols, rows);
}

static void hookResize(int cols, int rows)
{
	// "Restore default" means the board's configured size here, not a
	// hardcoded 80x25 — otherwise a CSI 0;0 t would silently undo the
	// per-board preference mid-session.
	if (cols == 0 || rows == 0) { cols = cfgCols; rows = cfgRows; }
	cfgCols = cols;
	cfgRows = rows;
	termResize(&term, cols, rows);
	if (mode == MODE_TALL)
		applyTallRows();
	telnetNotifySize(term.cols, term.rows);
}

// OSC 4/10/11 color queries (CTerm answers these; unanswered = BBS timeout)
static void handleOsc(const u8* data, int len)
{
	char s[64], buf[64];
	if (len <= 0 || len >= (int)sizeof(s))
		return;
	memcpy(s, data, len);
	s[len] = 0;

	int idx = -1;
	u32 c;
	if (!strncmp(s, "10;?", 4)) {
		c = palAnsi(7); // default foreground
		idx = 10;
	} else if (!strncmp(s, "11;?", 4)) {
		c = palAnsi(0); // default background
		idx = 11;
	} else if (sscanf(s, "4;%d;?", &idx) == 1 && idx >= 0 && idx <= 255) {
		c = palAnsi(idx);
		snprintf(buf, sizeof(buf), "\x1B]4;%d;rgb:%02x/%02x/%02x\x1B\\",
		         idx, (unsigned)(c & 0xFF), (unsigned)((c >> 8) & 0xFF),
		         (unsigned)((c >> 16) & 0xFF));
		telnetSend((const u8*)buf, strlen(buf));
		return;
	} else {
		return;
	}
	snprintf(buf, sizeof(buf), "\x1B]%d;rgb:%02x/%02x/%02x\x1B\\",
	         idx, (unsigned)(c & 0xFF), (unsigned)((c >> 8) & 0xFF),
	         (unsigned)((c >> 16) & 0xFF));
	telnetSend((const u8*)buf, strlen(buf));
}

static void hookString(AnsiStrKind kind, const u8* data, int len)
{
	if (kind == ANSI_STR_OSC)
		handleOsc(data, len);
	else
		apcHandle(kind, data, len, term.cx, term.cy); // APC cmds + DCS sixel
}

static void hookMusic(const u8* data, int len)
{
	// Phase 2 wiring point: ndsp PLAY-string synth
	(void)data; (void)len;
}

static void hookAudioStatus(int channel)
{
	audioStatusQuery(channel);
}

// System software keyboard. Returns false if the user cancelled; `secret`
// masks input for password entry.
static bool promptText(const char* hint, char* buf, size_t cap, bool secret)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, cap - 1);
	swkbdSetHintText(&kb, hint);
	swkbdSetInitialText(&kb, buf);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (secret)
		swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
	SwkbdButton b = swkbdInputText(&kb, buf, cap);
	return b == SWKBD_BUTTON_CONFIRM;
}

// --- input plumbing ---

static void kbdSend(const u8* data, int len)
{
	telnetSend(data, len);
}

static void kbdToggle(void)
{
	if (telnetIsConnected()) {
		// Drop held keys before the socket goes: a door that enabled
		// physical reports would otherwise never see the release edges.
		ctrlinReleaseAll();
		telnetClose();
		apcReset();
		keymodeReset();
		netState = NET_IDLE;
	} else if (netOk) {
		connectPending = true;
		netState = NET_CONNECTING;
	}
}

static void sendMouseClick(int col, int row)
{
	char buf[24];
	if (term.mouseSGR) {
		int n = snprintf(buf, sizeof(buf), "\x1B[<0;%d;%dM", col + 1, row + 1);
		telnetSend((const u8*)buf, n);
		n = snprintf(buf, sizeof(buf), "\x1B[<0;%d;%dm", col + 1, row + 1);
		telnetSend((const u8*)buf, n);
	} else if ((term.mouseNormal || term.mouseX10) && col < 222 && row < 222) {
		u8 b[6] = { 0x1B, '[', 'M', 32 + 0, (u8)(33 + col), (u8)(33 + row) };
		telnetSend(b, 6);
		if (term.mouseNormal) {
			b[3] = 32 + 3; // release
			telnetSend(b, 6);
		}
	}
}

// Receive throughput as a modem-style bit rate. Bytes/sec is what we
// measure; bps is what a BBS user has a feel for, and it is the number that
// makes the relay-vs-direct difference (DESIGN.md §7.5) legible without a
// packet capture.
static void fmtBps(u32 bytesPerSec, char* out, size_t cap)
{
	unsigned long bps = (unsigned long)bytesPerSec * 8;
	if (bps < 10000)
		snprintf(out, cap, "%lu", bps);
	else if (bps < 1000000)
		snprintf(out, cap, "%.1fk", bps / 1000.0);
	else
		snprintf(out, cap, "%.2fM", bps / 1000000.0);
}

static void setMode(int m)
{
	if (m == mode)
		return;
	bool wasTall = (mode == MODE_TALL);
	mode = m;
	if (mode == MODE_TALL) {
		applyTallRows();
		telnetNotifySize(term.cols, term.rows);
	} else if (wasTall) {
		termResize(&term, cfgCols, cfgRows);
		telnetNotifySize(term.cols, term.rows);
	}
}

int main(void)
{
	gfxInitDefault();
	gfxSet3D(true);
	APT_SetAppCpuTimeLimit(30); // free core-1 time for the APC worker thread
	// Two independent per-frame limits sit behind these numbers, and only
	// one of them is measurable. citro2d silently stops drawing once its
	// vertex buffer is full (counted — see termgfxDropped), while the GPU
	// command buffer svcBreaks on overflow with no counter at all. A dense
	// screen at 132 columns pushes both.
	//
	// Running out of either has ugly, non-obvious consequences: the bottom
	// screen is drawn last, so it is never cleared (stale garbage, no
	// keyboard), and drawCells emits all backgrounds before any glyphs, so
	// running dry between those passes leaves solid colour blocks with no
	// text over the 3D scene.
	//
	// The answer is not a bigger buffer. What actually collapses a dense
	// screen is merging background runs in the renderer, which cuts BOTH
	// limits by the same large factor — a full-screen colour field becomes
	// a few quads per row instead of one per cell. With that in place the
	// budget stays at a size known to allocate, leaving the linear heap
	// (measured: ~25MB free) to the sixel textures and mesh cache rather
	// than spending ~6.3MB of it on vertices for a worst case the renderer
	// no longer generates.
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
	size_t c2dObjects = 32768;
	if (!C2D_Init(c2dObjects)) {
		c2dObjects = 16384;
		C2D_Init(c2dObjects);
	}
	C2D_Prepare();

	C3D_RenderTarget* topL = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	scene3dInit();
	termgfxInit();
	tdfSplashInit();
	siximgInit();
	termInit(&term, 80, 25);
	term.onScroll = siximgScroll;
	term.onClearAll = siximgClearAll;
	term.onOverwrite = siximgOverwrite;

	AnsiHooks hooks = { hookRespond, hookResize, hookString, hookMusic,
	                    hookAudioStatus };
	ansiInit(&parser, &term, &hooks);

	netOk = netInit();
	settingsLoad();
	powerInit();
	if (settingsGet()->led)
		ledInit();
	pbLoad();
	telnetSetSize(term.cols, term.rows);
	kbdInit(kbdSend, kbdToggle);
	menuInit();
	cmLoad();
	ctrlinInit(kbdSend);
	pbviewInit(promptText, kbdToggle);
	audioInit(hookRespond); // needs sdmc:/3ds/dspfirm.cdc; silent without it
	apcInit(hookRespond);
#ifdef ENABLE_SSH
	sslcInit(0); // mbedtls entropy is wired to sslcGenerateRandomData
#endif
#ifndef RELEASE_BUILD
	beaconInit(DEV_MAC_IP, 2325);
#endif

	static u8 rxbuf[8192];
	u32 frame = 0;
	char status[96];

	// Perf overlay state
	char perf[96] = "";
	float frameMsAvg = 16.7f;
	u64 lastFrameTime = osGetTime();
	u32 lastRxTotal = 0, rxRate = 0, peakRate = 0, lastFrameRx = 0;
	u32 lastRateFrame = 0;
	float worstMs = 0.0f;   // longest single frame since the last report
	u64 lastRateTime = osGetTime();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		u32 kUp = hidKeysUp();
		circlePosition circlePad, cStick;
		hidCircleRead(&circlePad);
		hidCstickRead(&cStick);
		touchPosition touch;
		hidTouchRead(&touch);

		// START opens the menu; quitting lives in there now. START and
		// SELECT stay reserved from controller remapping so the menu is
		// always reachable whatever the active mapping does.
		if (kDown & KEY_START)
			menuToggle();
		if (menuIsOpen()) {
			// Nothing should stay held while the menu is up, or a door sees
			// a key stuck down for as long as the menu is open.
			ctrlinReleaseAll();
			MenuAction ma = menuUpdate(kDown, kHeld, touch);
			if (ma == MENU_QUIT)
				break;
			// Swallow everything else: nothing behind the menu should see
			// this frame's input.
			kDown = kHeld = 0;
			touch.px = touch.py = 0;
		}
		if (kDown & KEY_SELECT)
			setMode((mode + 1) % MODE_COUNT);
#ifndef RELEASE_BUILD
		if ((kDown & KEY_R) && shotStep < 0) {
			u16 fw = 0, fh = 0;
			gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
			if (shotOpen(DEV_MAC_IP, 2327, fw, fh, SHOT_VIEWS))
				shotStep = 0;   // -> tools/shotcatch.py
		}
#endif

#ifndef RELEASE_BUILD
		// DEBUG probe: L injects a magenta square through the sixel image
		// path at cell (2,2) — isolates draw-path vs image-lifetime bugs
		if (kDown & KEY_L) {
			u32* px = malloc(16 * 16 * 4);
			if (px) {
				for (int i = 0; i < 256; i++)
					px[i] = 0xFFFF00FFu; // magenta, opaque
				siximgSubmit(px, 16, 16, 2, 2);
			}
		}
#endif

		const PbEntry* pb = pbGet(pbSelected());

		if (connectPending) {
			connectPending = false;
			rxRate = peakRate = 0;   // per-session, or the peak lies
			// Adopt the board's geometry before dialing, so the size the
			// handshake announces (rlogin termtype / SSH pty / telnet
			// NAWS) is the one we actually have.
			{
				u16 c, r;
				pbSizeOf(pbSelected(), &c, &r);
				cfgCols = c;
				cfgRows = r;
				termResize(&term, c, r);
				if (mode == MODE_TALL)
					applyTallRows();
				telnetSetSize(term.cols, term.rows);
			}
			termReset(&term);
			if (telnetConnectAs(pb->host, pb->port, pb->proto,
			                    pb->user, pb->pass)) {
				netState = NET_CONNECTED;
				apcSetHost(pb->host);
			} else {
				netState = NET_FAILED;
			}
		}

		bool conn = telnetIsConnected();

		// Bottom-screen touch, routed by mode (the phonebook editor owns
		// the bottom screen while disconnected — handled further down)
		if (!conn) {
			// nothing here; pbviewUpdate runs in the disconnected branch
		} else if (mode == MODE_KBD) {
			kbdUpdate(kDown, kHeld, touch);
		} else if (conn && (kDown & KEY_TOUCH)) {
			TermView bv;
			if (mode == MODE_MIRROR) {
				termgfxFitView(&term, 320, 240, &bv);
			} else {
				float s = 320.0f / (term.cols * 8);
				termgfxSpanView(&term, 320, 240, s, 240.0f, &bv);
			}
			int col, row;
			if (termgfxCellAt(&term, &bv, touch.px, touch.py, &col, &row))
				sendMouseClick(col, row);
		}

		bool capturing = false;
#ifndef RELEASE_BUILD
		capturing = (shotStep >= 0);
#endif
		if ((conn || netState == NET_CONNECTED) && !capturing) {
			// Always drain the socket (keeps the sender's TCP window open),
			// then parse on a ~6ms budget so dense bursts spread across
			// frames instead of stalling one
			telnetDrain();
			u64 start = svcGetSystemTick();
			const u64 budgetTicks = 6 * 268112; // ~6ms of ARM11 ticks
			while (svcGetSystemTick() - start < budgetTicks) {
				int n = telnetRead(rxbuf, sizeof(rxbuf));
				if (n < 0) {
					netState = NET_CLOSED;
					apcReset();
					conn = false;
					break;
				}
				if (n == 0)
					break;
				ansiFeed(&parser, rxbuf, n);
			}
		}

		if (conn) {
			// Every physical control now goes through the active mapping,
			// which decides both the evdev identity and the byte fallback.
			ctrlinUpdate(kDown, kUp, kHeld, circlePad, cStick);
			float px, py;
			if (ctrlinClicked() && ctrlinPointer(&px, &py)) {
				TermView tv;
				termgfxFitView(&term, 400, 240, &tv);
				int col, row;
				if (termgfxCellAt(&term, &tv, (int)px, (int)py, &col, &row))
					sendMouseClick(col, row);
			}
		} else {
			// Disconnected: the bottom screen is the phonebook editor
			pbviewUpdate(kDown, kHeld, touch);
		}

		static const char* stateNames[] = {
			"tap to connect", "connecting...", "online",
			"connect FAILED", "connection closed"
		};
		const char* protoName = pb->proto == PROTO_RLOGIN ? "rlogin" :
		                        pb->proto == PROTO_SSH    ? "ssh"    : "telnet";
		if (conn) {
			// Online, the line rate is the interesting number and the
			// host:port is not — you already know where you are. Peak is
			// held because a burst that ends before you look up is still
			// the honest answer to "how fast is this link".
			char now[16], pk[16];
			fmtBps(rxRate, now, sizeof(now));
			fmtBps(peakRate, pk, sizeof(pk));
			snprintf(status, sizeof(status), "%s %s%s  %s bps  pk %s",
			         pb->name, protoName, pb->user[0] ? "*" : "", now, pk);
		} else {
			snprintf(status, sizeof(status), "%s %s:%u %s%s - %s",
			         pb->name, pb->host, pb->port, protoName,
			         pb->user[0] ? "*" : "",   // '*' = credentials stored
			         netOk ? stateNames[netState] : "network init FAILED");
		}

		audioPoll();
		siximgPoll();
		{
			// Per-frame byte delta: the LED lamp needs the bursts, not the
			// once-a-second average, which is zero on an idle session.
			int ringNow;
			u32 rxNow;
			telnetStats(&ringNow, &rxNow);
			ledUpdate(conn, rxNow - lastFrameRx);
			lastFrameRx = rxNow;
		}
		// Lid shut with the session held open: nothing on either screen is
		// visible, so skip the frame's drawing entirely and let the loop
		// keep draining the socket. C3D_FrameBegin/End is what paces us,
		// so it still runs — only the work inside it is skipped.
		bool lidHeld = powerUpdate(conn, settingsGet()->lidKeepaliveMin);

		// Perf overlay: frame-time EMA, ring backlog, ingest rate
		{
			u64 now = osGetTime();
			float thisMs = (float)(now - lastFrameTime);
			frameMsAvg += 0.1f * (thisMs - frameMsAvg);
			// An averaged frame time hides exactly the thing worth seeing:
			// one blocking stall inside an otherwise smooth second.
			if (thisMs > worstMs)
				worstMs = thisMs;
			lastFrameTime = now;
			if (frame - lastRateFrame >= 60) {
				int ringBytes;
				u32 rxTotal;
				telnetStats(&ringBytes, &rxTotal);
				// True bytes/sec: 60 frames is 1s only at 60fps, and this
				// number is the whole point of the measurement
				u64 elapsed = now - lastRateTime;
				if (elapsed < 1)
					elapsed = 1;
				rxRate = (u32)((u64)(rxTotal - lastRxTotal) * 1000 / elapsed);
				if (rxRate > peakRate)
					peakRate = rxRate;
				lastRateTime = now;
				lastRxTotal = rxTotal;
				lastRateFrame = frame;
				worstMs = 0.0f;
				// ptm/slp/lid/led say whether the sleep and LED features
				// actually have their services. Both fail silently and
				// look exactly like "the feature is off" from outside,
				// and service access differs between the Homebrew
				// Launcher and the .cia — so they get reported, not
				// assumed.
				PowerStatus ps;
				powerStatus(&ps);
				snprintf(perf, sizeof(perf),
				         "%.1fms mx%.0f r:%dK %luK/s drop%lu %dx%d/%luk ptm%d ndm%d "
				         "slp%d lid%d led%d shut%lus/%luf",
				         frameMsAvg, worstMs, ringBytes / 1024,
				         (unsigned long)(rxRate / 1024),
				         (unsigned long)termgfxDropped(),
				         term.cols, term.rows,
				         (unsigned long)(c2dObjects / 1000),
				         ps.ptmOk, ps.ndmOk, ps.sleepBlocked, ps.lidShut,
				         ledOk(), (unsigned long)(ps.lastShutMs / 1000),
				         (unsigned long)ps.lastShutFrames);

				// Same stats + internals, phoned home so nobody has to
				// squint at the overlay
				u32 mask, drains, jobs, jobB;
				u32 sxSeen, sxFail, sxSub, sxTex, sxTexFail, sxClr;
				int sxW, sxH, sxLive;
				siximgDebugLive(&sxLive, &sxClr);
				audioDebugStats(&mask, &drains);
				apcDebugStats(&jobs, &jobB);
				apcSixelStats(&sxSeen, &sxFail);
				siximgDebugStats(&sxSub, &sxTex, &sxTexFail, &sxW, &sxH);
				char tele[288];
				snprintf(tele, sizeof(tele),
				         "f=%.1fms ring=%d rx=%lu/s jobs=%lu(%luK) "
				         "playing=%03lx drains=%lu conn=%d rcvbuf=%d hs=%d "
				         "six=%lu/%lu/%lu/%lu df=%lu %dx%d live=%d clr=%lu "
				         "drop=%lu c2d=%lu grid=%dx%d lin=%luK vram=%luK",
				         frameMsAvg, ringBytes, (unsigned long)rxRate,
				         (unsigned long)jobs, (unsigned long)(jobB / 1024),
				         (unsigned long)mask, (unsigned long)drains,
				         conn ? 1 : 0, telnetRcvBuf(), telnetRloginSent(),
				         (unsigned long)sxSeen, (unsigned long)sxSub,
				         (unsigned long)sxTex, (unsigned long)sxTexFail,
				         (unsigned long)sxFail, sxW, sxH, sxLive,
				         (unsigned long)sxClr,
				         (unsigned long)termgfxDropped(),
				         (unsigned long)c2dObjects, term.cols, term.rows,
				         (unsigned long)(linearSpaceFree() / 1024),
				         (unsigned long)(vramSpaceFree() / 1024));
				beaconSend(tele);
			}
		}

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;
#ifndef RELEASE_BUILD
		// Sweep the viewpoint across the capture, ignoring the physical
		// slider entirely — otherwise a slider at zero yields a sequence of
		// identical frames and no parallax at all.
		if (shotStep >= 0)
			iod = SHOT_IOD * (2.0f * shotStep / (SHOT_VIEWS - 1) - 1.0f);
#endif

		if (!capturing) {
			// Frozen while capturing: any animation between views would be
			// baked into the loop as judder on top of the parallax.
			scene3dUpdate();
			if (!conn && netState != NET_CLOSED)
				tdfSplashUpdate();
		}
		frame++;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		if (lidHeld) {
			// Behind a closed lid there is nothing to see, so skip walking
			// the grid — but still clear and bind a target, so the frame
			// has the same shape C3D_FrameEnd sees on every other pass.
			// The loop keeps draining the socket on the way round.
			C2D_TargetClear(topL, 0xFF000000);
			C2D_TargetClear(bottom, 0xFF000000);
			C2D_SceneBegin(bottom);
			C3D_FrameEnd(0);
			continue;
		}

		// Top screen
		bool termOnTop = conn || netState == NET_CLOSED;
		if (termOnTop) {
			TermView tv;
			if (mode == MODE_TALL) {
				float s = 320.0f / (term.cols * 8);
				termgfxSpanView(&term, 400, 240, s, 0.0f, &tv);
			} else {
				termgfxFitView(&term, 400, 240, &tv);
			}

			// BBS-driven 3D scene renders first; the terminal draws over it
			// with black backgrounds skipped, so the scene shows through
			bool scene = scene3dActive();
			if (scene) {
				scene3dRenderTo(topL, -iod);
				if (iod > 0.0f)
					scene3dRenderTo(topR, iod);
				// 2D pass must not be occluded by the scene's depth buffer
				C3D_RenderTargetClear(topL, C3D_CLEAR_DEPTH, 0, 0);
				if (iod > 0.0f)
					C3D_RenderTargetClear(topR, C3D_CLEAR_DEPTH, 0, 0);
			}

			// Text depth layers: per-eye disparity from the same stereo
			// projection as the scene. Slider at zero => all-zero shifts
			// => the flat classic draw.
			float textShifts[TERM_TEXT_LAYERS];
			scene3dTextShifts(-iod, term.layerDepth, TERM_TEXT_LAYERS,
			                  textShifts);

			C2D_Prepare();
			if (!scene)
				C2D_TargetClear(topL, termgfxPalette(0));
			C2D_SceneBegin(topL);
			termgfxRenderTermView(&term, frame, &tv, textShifts);
			siximgDraw(&tv);
			if (iod > 0.0f) {
				scene3dTextShifts(iod, term.layerDepth,
				                  TERM_TEXT_LAYERS, textShifts);
				if (!scene)
					C2D_TargetClear(topR, termgfxPalette(0));
				C2D_SceneBegin(topR);
				termgfxRenderTermView(&term, frame, &tv, textShifts);
				siximgDraw(&tv);
			}
		} else {
			// Pre-login the top screen is the splash: the product name in
			// TheDraw fonts, drifting at real stereo depth. Plain C2D over
			// the same CP437 atlas the terminal uses, so no 3D pipeline is
			// involved and the scene layer stays reserved for the BBS.
			C2D_Prepare();
			C2D_TargetClear(topL, 0xFF000000);
			C2D_SceneBegin(topL);
			tdfSplashRender(-iod);
			if (iod > 0.0f) {
				C2D_TargetClear(topR, 0xFF000000);
				C2D_SceneBegin(topR);
				tdfSplashRender(iod);
			}
		}

		// Bottom screen
		C2D_TargetClear(bottom, 0xFF181818);
		C2D_SceneBegin(bottom);
		if (!conn) {
			pbviewRender(status);
		} else if (mode == MODE_KBD) {
			kbdRender(status, conn);
		} else {
			TermView bv;
			if (mode == MODE_MIRROR) {
				termgfxFitView(&term, 320, 240, &bv);
			} else {
				float s = 320.0f / (term.cols * 8);
				termgfxSpanView(&term, 320, 240, s, 240.0f, &bv);
			}
			termgfxRenderTermView(&term, frame, &bv, NULL);
			siximgDraw(&bv);
		}

#ifndef RELEASE_BUILD
		// Perf overlay, bottom-left of the touch screen
		termgfxDrawText(2, 231, 0.5f, 0xFF00CCCC, perf);
#endif

		if (menuIsOpen())
			menuRender();

		C3D_FrameEnd(0);

#ifndef RELEASE_BUILD
		if (shotStep >= 0) {
			// After FrameEnd the framebuffer holds the view just presented.
			u16 fw = 0, fh = 0;
			u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
			if (!shotFrame(fb, (u32)fw * fh * 3) || ++shotStep >= SHOT_VIEWS) {
				shotClose();
				shotStep = -1;
			}
		}
#endif
	}

	telnetClose();
	pbFlush();   // persist any phonebook edit still buffered
	cmFlush();
	ledExit();
	powerExit();
	apcExit();
#ifdef ENABLE_SSH
	sslcExit();
#endif
	beaconExit();
	netExit();
	audioExit();
	ansiFree(&parser);
	termFree(&term);
	siximgExit();
	termgfxExit();
	scene3dExit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	// Hand back the syscore time claimed at startup. Leaving it set is a
	// known way to hang the return to the Homebrew Launcher.
	APT_SetAppCpuTimeLimit(0);
	return 0;
}
