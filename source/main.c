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
#include "proto/apc.h"
#include "audio/apcaudio.h"
#include "term/palette.h"
#include "net/beacon.h"
#include "gfx/siximg.h"

#define DEV_MAC_IP "192.168.1.61"  // dev telemetry collector (see tests/)

enum { MODE_KBD, MODE_MIRROR, MODE_TALL, MODE_COUNT };

enum { NET_IDLE, NET_CONNECTING, NET_CONNECTED, NET_FAILED, NET_CLOSED };

static Terminal term;
static AnsiParser parser;
static bool netOk;
static int mode = MODE_KBD;
static int netState = NET_IDLE;
static bool connectPending;

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
	if (cols == 0 || rows == 0) { cols = 80; rows = 25; } // "restore default"
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
		telnetClose();
		apcReset();
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
		termResize(&term, term.cols, 25);
		telnetNotifySize(term.cols, term.rows);
	}
}

int main(void)
{
	gfxInitDefault();
	gfxSet3D(true);
	APT_SetAppCpuTimeLimit(30); // free core-1 time for the APC worker thread
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
	C2D_Init(32768); // worst case: 80x60 grid fully colored, both screens
	C2D_Prepare();

	C3D_RenderTarget* topL = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	scene3dInit();
	termgfxInit();
	siximgInit();
	termInit(&term, 80, 25);
	term.onScroll = siximgScroll;
	term.onClearAll = siximgClearAll;
	term.onOverwrite = siximgOverwrite;

	AnsiHooks hooks = { hookRespond, hookResize, hookString, hookMusic,
	                    hookAudioStatus };
	ansiInit(&parser, &term, &hooks);

	netOk = netInit();
	pbLoad();
	telnetSetSize(term.cols, term.rows);
	kbdInit(kbdSend, kbdToggle);
	pbviewInit(promptText, kbdToggle);
	audioInit(hookRespond); // needs sdmc:/3ds/dspfirm.cdc; silent without it
	apcInit(hookRespond);
	beaconInit(DEV_MAC_IP, 2325);

	static u8 rxbuf[8192];
	u32 frame = 0;
	char status[96];

	// Perf overlay state
	char perf[48] = "";
	float frameMsAvg = 16.7f;
	u64 lastFrameTime = osGetTime();
	u32 lastRxTotal = 0, rxRate = 0;
	u32 lastRateFrame = 0;
	u64 lastRateTime = osGetTime();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		touchPosition touch;
		hidTouchRead(&touch);

		// Hold START ~1.5s to quit (a stray tap shouldn't kill a session)
		static int startHeld;
		if (kHeld & KEY_START) {
			if (++startHeld >= 90)
				break;
		} else {
			startHeld = 0;
		}
		if (kDown & KEY_SELECT)
			setMode((mode + 1) % MODE_COUNT);

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

		const PbEntry* pb = pbGet(pbSelected());

		if (connectPending) {
			connectPending = false;
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

		if (conn || netState == NET_CONNECTED) {
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
			if (kDown & KEY_DUP)    telnetSend((const u8*)"\x1B[A", 3);
			if (kDown & KEY_DDOWN)  telnetSend((const u8*)"\x1B[B", 3);
			if (kDown & KEY_DRIGHT) telnetSend((const u8*)"\x1B[C", 3);
			if (kDown & KEY_DLEFT)  telnetSend((const u8*)"\x1B[D", 3);
			if (kDown & KEY_A)      telnetSend((const u8*)"\r", 1);
			if (kDown & KEY_B)      telnetSend((const u8*)"\b", 1);
			if (kDown & KEY_X)      telnetSend((const u8*)" ", 1);
			if (kDown & KEY_Y)      telnetSend((const u8*)"\x1B", 1);
		} else {
			// Disconnected: the bottom screen is the phonebook editor
			pbviewUpdate(kDown, kHeld, touch);
		}

		static const char* stateNames[] = {
			"tap to connect", "connecting...", "online",
			"connect FAILED", "connection closed"
		};
		snprintf(status, sizeof(status), "%s %s:%u %s%s - %s",
		         pb->name, pb->host, pb->port,
		         pb->proto == PROTO_RLOGIN ? "rlogin" : "telnet",
		         pb->user[0] ? "*" : "",   // '*' = credentials stored
		         netOk ? stateNames[netState] : "network init FAILED");

		audioPoll();
		siximgPoll();

		// Perf overlay: frame-time EMA, ring backlog, ingest rate
		{
			u64 now = osGetTime();
			frameMsAvg += 0.1f * ((float)(now - lastFrameTime) - frameMsAvg);
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
				lastRateTime = now;
				lastRxTotal = rxTotal;
				lastRateFrame = frame;
				snprintf(perf, sizeof(perf), "%.1fms r:%dK %luK/s",
				         frameMsAvg, ringBytes / 1024,
				         (unsigned long)(rxRate / 1024));

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
				char tele[192];
				snprintf(tele, sizeof(tele),
				         "f=%.1fms ring=%d rx=%lu/s jobs=%lu(%luK) "
				         "playing=%03lx drains=%lu conn=%d rcvbuf=%d hs=%d "
				         "six=%lu/%lu/%lu/%lu df=%lu %dx%d live=%d clr=%lu",
				         frameMsAvg, ringBytes, (unsigned long)rxRate,
				         (unsigned long)jobs, (unsigned long)(jobB / 1024),
				         (unsigned long)mask, (unsigned long)drains,
				         conn ? 1 : 0, telnetRcvBuf(), telnetRloginSent(),
				         (unsigned long)sxSeen, (unsigned long)sxSub,
				         (unsigned long)sxTex, (unsigned long)sxTexFail,
				         (unsigned long)sxFail, sxW, sxH, sxLive,
				         (unsigned long)sxClr);
				beaconSend(tele);
			}
		}

		float slider = osGet3DSliderState();
		float iod = slider / 3.0f;

		scene3dUpdate();
		frame++;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

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
			scene3dRenderTo(topL, -iod);
			if (iod > 0.0f)
				scene3dRenderTo(topR, iod);
			C2D_Prepare();
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

		// Perf overlay, bottom-left of the touch screen
		termgfxDrawText(2, 231, 0.5f, 0xFF00CCCC, perf);

		C3D_FrameEnd(0);
	}

	telnetClose();
	pbFlush();   // persist any phonebook edit still buffered
	apcExit();
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
