// Host-native tests for the terminal core: wrap semantics, SGR/truecolor,
// query replies, resize hook, APC collection, and a fuzz pass.
// Build/run: tests/host/run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../source/term/termbuf.h"
#include "../../source/term/ansi.h"
#include "../../source/term/keymode.h"
#include "../../source/term/palette.h"
#include "../../source/gfx/sixel.h"

static int failures;
#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
} while (0)

// --- captured hook output ---
static char resp[4096];
static int respLen;
static int gotCols, gotRows;
static char strData[1024];
static int strLen_, strKind_ = -1;
static char musicData[256];
static int musicLen;

static void hkRespond(const u8* d, int n)
{
	if (respLen + n < (int)sizeof(resp) - 1) {
		memcpy(resp + respLen, d, n);
		respLen += n;
		resp[respLen] = 0;
	}
}
static void hkResize(int c, int r) { gotCols = c; gotRows = r; }
static void hkString(AnsiStrKind k, const u8* d, int n)
{
	strKind_ = k;
	strLen_ = n < (int)sizeof(strData) ? n : (int)sizeof(strData);
	memcpy(strData, d, strLen_);
}
static void hkMusic(const u8* d, int n)
{
	musicLen = n < (int)sizeof(musicData) ? n : (int)sizeof(musicData);
	memcpy(musicData, d, musicLen);
}
static void hkAudio(int ch) { (void)ch; }

static Terminal term;
static AnsiParser parser;

static char scrollLog[128];
static void hkScroll(int rows, int top, int bot)
{
	size_t off = strlen(scrollLog);
	snprintf(scrollLog + off, sizeof(scrollLog) - off, "%d:%d-%d;", rows, top, bot);
}

static void feed(const char* s)
{
	ansiFeed(&parser, (const u8*)s, strlen(s));
}

static void resetAll(void)
{
	termReset(&term);
	respLen = 0;
	resp[0] = 0;
	strKind_ = -1;
}

static TermCell* cellAt(int x, int y) { return &term.cells[y * term.cols + x]; }

int main(void)
{
	AnsiHooks hooks = { hkRespond, hkResize, hkString, hkMusic, hkAudio };
	termInit(&term, 80, 25);
	term.onScroll = hkScroll;
	ansiInit(&parser, &term, &hooks);

	// --- CTerm wrap semantics: immediate wrap on writing the last column ---
	resetAll();
	for (int i = 0; i < 80; i++)
		feed("X");
	CHECK(term.cx == 0 && term.cy == 1, "immediate wrap after col 80");

	// no-wrap mode: cursor pinned at last column
	resetAll();
	feed("\x1B[?7l");
	for (int i = 0; i < 85; i++)
		feed("Y");
	CHECK(term.cx == 79 && term.cy == 0, "no-wrap pins cursor");
	feed("\x1B[?7h");

	// bottom-right write scrolls
	resetAll();
	feed("\x1B[1;1HTOP");
	feed("\x1B[25;80H");
	feed("Z");
	CHECK(term.cy == 24 && term.cx == 0, "bottom-right wrap scrolled+homed column");
	CHECK(cellAt(0, 0)->ch != 'T', "scroll consumed row 0");

	// --- SGR: classic, bright, truecolor, 256, iCE ---
	resetAll();
	feed("\x1B[1;31mA");
	CHECK(cellAt(0, 0)->fg == palAnsi(9), "bold red = bright red");
	feed("\x1B[0m\x1B[38;2;255;128;0mB");
	CHECK(cellAt(1, 0)->fg == palRGB(255, 128, 0), "truecolor fg");
	feed("\x1B[48;5;196mC");
	CHECK(cellAt(2, 0)->bg == palAnsi(196), "256-color bg");
	feed("\x1B[0m\x1B[?33h\x1B[5;44mD");
	CHECK(cellAt(3, 0)->bg == palAnsi(12), "iCE: blink+blue bg = bright blue bg");
	CHECK(cellAt(3, 0)->blink == 0, "iCE consumes blink attr");
	feed("\x1B[?33l");

	// --- DA / CTDA ---
	resetAll();
	feed("\x1B[0c");
	CHECK(strcmp(resp, ANSI_CTERM_DA) == 0, "plain DA returns CTerm banner");
	resetAll();
	feed("\x1B[<0c");
	CHECK(strcmp(resp, "\x1B[<0;2;4;7;8c") == 0,
	      "CTDA advertises 8 (physical key reports) alongside 2/4/7");

	// --- keyboard reporting modes (cterm.adoc) ---
	// Doors climb evdev -> kitty -> plain bytes; each rung has to work and,
	// more importantly, must not fire when the far end never asked for it.
	{
		u8 out[64];
		KeyEvent ev;
		memset(&ev, 0, sizeof(ev));
		ev.evdev = 103;            // EVDEV_KEY_UP
		ev.bytes = "\x1B[A";
		ev.nbytes = 3;
		ev.edge = KEY_PRESS;

		keymodeReset();
		int n = keymodeEncode(&ev, out, sizeof(out));
		CHECK(n == 3 && !memcmp(out, "\x1B[A", 3),
		      "no modes set: plain translated bytes");
		ev.edge = KEY_RELEASE;
		CHECK(keymodeEncode(&ev, out, sizeof(out)) == 0,
		      "no modes set: releases are silent");

		// CSI = 1 h — physical reports, translation still flowing
		resetAll();
		feed("\x1B[=1h");
		CHECK(keymodePhysical(), "CSI = 1 h enables physical reports");
		ev.edge = KEY_PRESS;
		n = keymodeEncode(&ev, out, sizeof(out));
		out[n] = 0;
		CHECK(!strcmp((char*)out, "\x1B[=103K\x1B[A"),
		      "physical press report precedes the translated bytes");
		ev.edge = KEY_RELEASE;
		n = keymodeEncode(&ev, out, sizeof(out));
		out[n] = 0;
		CHECK(!strcmp((char*)out, "\x1B[=103k"), "release reported as lower k");

		// CSI = 2 h — suppress translation, physical only
		feed("\x1B[=2h");
		CHECK(keymodeSuppress(), "CSI = 2 h suppresses translated input");
		ev.edge = KEY_PRESS;
		n = keymodeEncode(&ev, out, sizeof(out));
		out[n] = 0;
		CHECK(!strcmp((char*)out, "\x1B[=103K"),
		      "suppressed: physical report only, no translated bytes");

		feed("\x1B[=2l\x1B[=1l");
		CHECK(!keymodePhysical() && !keymodeSuppress(), "CSI = 1/2 l reset");

		// kitty progressive enhancement
		resetAll();
		feed("\x1B[?u");
		CHECK(!strcmp(resp, "\x1B[?0u"),
		      "CSI ? u answers with current flags (0 = nothing pushed)");
		feed("\x1B[>11u");
		CHECK(keymodeKittyFlags() == 11, "CSI > 11 u pushes flags");
		ev.edge = KEY_PRESS;
		ev.codepoint = 0;
		n = keymodeEncode(&ev, out, sizeof(out));
		out[n] = 0;
		CHECK(!strcmp((char*)out, "\x1B[103;1:1u"),
		      "kitty press carries modifiers and event type");
		ev.edge = KEY_RELEASE;
		n = keymodeEncode(&ev, out, sizeof(out));
		out[n] = 0;
		CHECK(!strcmp((char*)out, "\x1B[103;1:3u"), "kitty reports releases");
		feed("\x1B[<u");
		CHECK(keymodeKittyFlags() == 0, "CSI < u pops back");
		keymodeReset();
	}

	// --- sixel decode ---
	{
		u32* rgba;
		int w, h;
		// Two full columns of palette color 1, then one column of color 2
		CHECK(sixelDecode((const u8*)"0;0;0q#1~~#2~", 13, &rgba, &w, &h),
		      "sixel decodes");
		CHECK(w == 3 && h == 6, "sixel dimensions");
		CHECK(rgba[0] == rgba[1] && rgba[0] != rgba[2], "sixel colors painted");
		free(rgba);
	}

	// --- DSR/CPR ---
	resetAll();
	feed("\x1B[5;10H\x1B[6n");
	CHECK(strcmp(resp, "\x1B[5;10R") == 0, "CPR reports 1-based position");
	resetAll();
	feed("\x1B[255n");
	CHECK(strcmp(resp, "\x1B[25;80R") == 0, "BCDSR reports size");
	resetAll();
	feed("\x1B[=3n");
	CHECK(strcmp(resp, "\x1B[=3;16;8n") == 0, "cell-size report");
	resetAll();
	feed("\x1B[?62n");
	CHECK(strcmp(resp, "\x1B[32767*{") == 0, "DECMSR macro space");
	resetAll();
	feed("\x1B[?7$p");
	CHECK(strcmp(resp, "\x1B[?7;1$y") == 0, "DECRQM: autowrap set");

	// private-marker sequences must not hit movement handlers
	resetAll();
	feed("\x1B[2;2HQ\x1B[?2;1S");
	CHECK(term.cy == 1, "XTSRGA did not scroll");
	CHECK(strcmp(resp, "\x1B[?2;0;640;400S") == 0, "XTSRGA pixel size");

	// --- resize hook ---
	resetAll();
	gotCols = 0;
	feed("\x1B[8;60;132t");
	CHECK(gotCols == 132 && gotRows == 60, "CSI 8;r;c t resize hook");

	// --- APC collection ---
	resetAll();
	feed("\x1B_SyncTERM:VER\x1B\\");
	CHECK(strKind_ == ANSI_STR_APC, "APC dispatched");
	CHECK(strLen_ == 12 && memcmp(strData, "SyncTERM:VER", 12) == 0, "APC payload");

	// --- ANSI music: bare CSI M collects until 0x0E ---
	resetAll();
	musicLen = 0;
	feed("\x1B[MFT120O4CDE\x0E");
	CHECK(musicLen == 10 && memcmp(musicData, "FT120O4CDE", 10) == 0, "music string");
	// ...but CSI Pn M is still delete-line
	resetAll();
	feed("\x1B[1;1HA\x1B[1;1H\x1B[1M");
	CHECK(cellAt(0, 0)->ch == ' ', "CSI 1 M deletes line");

	// --- DECSTBM scroll region ---
	resetAll();
	feed("\x1B[1;1HTOPLINE");
	feed("\x1B[6;10r"); // region rows 6-10 (0-based 5-9)
	CHECK(term.marginTop == 5 && term.marginBot == 9, "DECSTBM sets margins");
	CHECK(term.cx == 0 && term.cy == 0, "DECSTBM homes cursor");
	feed("\x1B[10;1HREGIONBOT\n"); // LF at region bottom: region-only scroll
	CHECK(term.cy == 9, "cursor pinned at region bottom");
	CHECK(cellAt(0, 0)->ch == 'T', "content above region untouched");
	CHECK(cellAt(0, 8)->ch == 'R', "region content scrolled up one row");
	CHECK(cellAt(0, 9)->ch == ' ', "region bottom row blanked");
	feed("\x1B[r");
	CHECK(term.marginTop == 0 && term.marginBot == 24, "CSI r resets margins");

	// --- sixel regression: real fl_records payload (tests/fixtures) ---
	{
		FILE* f = fopen("../fixtures/fl_sixel.bin", "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long flen = ftell(f);
			fseek(f, 0, SEEK_SET);
			u8* fdata = malloc(flen);
			fread(fdata, 1, flen, f);
			fclose(f);
			u32* rgba;
			int w, h;
			CHECK(sixelDecode(fdata, flen, &rgba, &w, &h), "fixture decodes");
			CHECK(w == 138 && h >= 160 && h <= 168,
			      "fixture dims honor raster attrs (no double-height slab)");
			int nonBlack = 0;
			for (int i = 0; i < w * h; i++)
				if ((rgba[i] & 0xFFFFFF) != 0)
					nonBlack++;
			CHECK(nonBlack > w * h / 4, "fixture is mostly painted, not black");
			free(rgba);
			free(fdata);
		} else {
			printf("note: fixture fl_sixel.bin missing, skipping\n");
		}
	}

	// --- sixel scroll-clip geometry (the artifact-on-scroll-out rules) ---
	{
		// Image 32px tall at y=160 inside band [160,320); scroll up 16px
		int y = 160, ct = 0, cb = 0;
		CHECK(!sixelShiftClip(&y, 32, &ct, &cb, 16, 160, 320), "clip: shifted, alive");
		CHECK(y == 144 && ct == 16 && cb == 0, "clip: top row destroyed at band edge");
		CHECK(sixelShiftClip(&y, 32, &ct, &cb, 16, 160, 320), "clip: fully consumed");
		// Image parked ABOVE the band must not move on a band scroll
		y = 0; ct = cb = 0;
		CHECK(!sixelShiftClip(&y, 32, &ct, &cb, 16, 160, 320), "clip: outside band");
		CHECK(y == 0 && ct == 0, "clip: image outside band untouched");
		// Scroll down pushes an image out the bottom of the band
		y = 300; ct = cb = 0;
		sixelShiftClip(&y, 32, &ct, &cb, -16, 160, 320);
		CHECK(y == 316 && cb == 28, "clip: bottom rows destroyed on down-scroll");
	}

	// --- IL/DL notify the graphics layer like scrolls ---
	{
		resetAll();
		scrollLog[0] = 0;
		feed("\x1B[5;1H\x1B[2M"); // delete 2 lines at row 5
		CHECK(strcmp(scrollLog, "2:4-24;") == 0, "DL fires onScroll with band");
		scrollLog[0] = 0;
		feed("\x1B[2L"); // insert 2 lines at row 5
		CHECK(strcmp(scrollLog, "-2:4-24;") == 0, "IL fires onScroll with band");
	}

	// --- text depth layers (protocol 0.3): CSI = z select / CSI = * z depth ---
	{
		resetAll();
		feed("A"); // untagged baseline
		CHECK(cellAt(0, 0)->layer == 0, "default writes land on layer 0");
		feed("\x1B[=3z");
		CHECK(term.activeLayer == 3, "CSI = 3 z selects layer");
		feed("B");
		CHECK(cellAt(1, 0)->layer == 3, "glyphs stamp the active layer");
		feed("\x1B[=3;150*z");
		CHECK(term.layerDepth[3] > 1.49f && term.layerDepth[3] < 1.51f,
		      "CSI = 3;150 * z sets depth 1.5 world units");
		// Layer is orthogonal to SGR: reset must not touch it
		feed("\x1B[0mC");
		CHECK(cellAt(2, 0)->layer == 3, "SGR 0 keeps the active layer");
		// Erase fills stamp the active layer too (erasing is writing)
		feed("\x1B[1;10H\x1B[K");
		CHECK(cellAt(15, 0)->layer == 3, "EL fill carries the active layer");
		// Scrolls move tags with their cells; fills use the active layer
		feed("\x1B[=7z\x1B[10;5HZ");   // 'Z' at (4,9) on layer 7
		feed("\x1B[=0z\x1B[25;1H\n");  // full-screen scroll up by one
		CHECK(cellAt(4, 8)->ch == 'Z' && cellAt(4, 8)->layer == 7,
		      "scrolled cell keeps its tag");
		CHECK(cellAt(0, 24)->layer == 0, "scroll fill uses the active layer");
		// Clamps: select saturates, bad depth slot is ignored
		feed("\x1B[=99z");
		CHECK(term.activeLayer == TERM_TEXT_LAYERS - 1, "layer select clamps");
		feed("\x1B[=99;500*z");
		feed("\x1B[=2;9999*z");
		CHECK(term.layerDepth[2] > 17.9f && term.layerDepth[2] < 18.1f,
		      "depth clamps to 18");
		// Plain CSI z (no '=') is not ours: swallowed harmlessly
		int keep = term.activeLayer;
		feed("\x1B[5z");
		CHECK(term.activeLayer == keep, "plain CSI z ignored");
		// ESC c resets the whole layer state
		feed("\x1B""c");
		CHECK(term.activeLayer == 0, "RIS resets active layer");
		CHECK(term.layerDepth[3] == 0.0f, "RIS resets layer depths");
	}

	// --- fuzz: 1MB of noise must not crash or corrupt bounds ---
	srand(1234);
	for (int i = 0; i < 1024 * 1024; i++) {
		u8 b = rand() & 0xFF;
		ansiFeed(&parser, &b, 1);
		if (term.cx < 0 || term.cx >= term.cols || term.cy < 0 || term.cy >= term.rows) {
			CHECK(0, "cursor out of bounds during fuzz");
			break;
		}
	}

	printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
	return failures ? 1 : 0;
}
