#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ansi.h"
#include "palette.h"

enum {
	ST_GROUND,
	ST_ESC,
	ST_CSI,
	ST_STR,       // APC/DCS/OSC collection
	ST_STR_ESC,   // saw ESC inside string (ESC \ = ST)
	ST_MUSIC,     // CSI M/N/| music string, terminated by 0x0E
};

bool ansiInit(AnsiParser* p, Terminal* term, const AnsiHooks* hooks)
{
	memset(p, 0, sizeof(*p));
	p->term = term;
	p->hooks = *hooks;
	p->state = ST_GROUND;
	p->strBuf = malloc(ANSI_STR_MAX);
	return p->strBuf != NULL;
}

void ansiFree(AnsiParser* p)
{
	free(p->strBuf);
	p->strBuf = NULL;
}

static void respond(AnsiParser* p, const char* s)
{
	if (p->hooks.respond)
		p->hooks.respond((const u8*)s, strlen(s));
}

static int param(AnsiParser* p, int i, int def)
{
	if (i >= p->nParams || p->params[i] < 0)
		return def;
	return p->params[i];
}

static void csiReset(AnsiParser* p)
{
	p->nParams = 0;
	p->paramSeen = false;
	p->priv = 0;
	p->intermediate = 0;
	memset(p->params, 0xFF, sizeof(p->params)); // -1 = unset
}

static void strStart(AnsiParser* p, AnsiStrKind kind)
{
	p->strKind = kind;
	p->strLen = 0;
	p->state = ST_STR;
}

static void strDispatch(AnsiParser* p)
{
	if (p->hooks.string)
		p->hooks.string(p->strKind, p->strBuf, p->strLen);
	p->state = ST_GROUND;
}

// Handles SGR 38/48 extended color: ;5;n (xterm-256) and ;2;r;g;b (truecolor).
// Returns params consumed beyond the 38/48 itself.
static int extColor(AnsiParser* p, int i, bool isFg)
{
	Terminal* t = p->term;
	u32 color;
	int used;
	switch (param(p, i + 1, -1)) {
	case 5:
		color = palAnsi(param(p, i + 2, 0));
		used = 2;
		break;
	case 2:
		color = palRGB(param(p, i + 2, 0), param(p, i + 3, 0), param(p, i + 4, 0));
		used = 4;
		break;
	default:
		return 0;
	}
	if (isFg) { t->fgDirect = true; t->fgRGB = color; }
	else      { t->bgDirect = true; t->bgRGB = color; }
	return used;
}

static void doSGR(AnsiParser* p)
{
	Terminal* t = p->term;
	if (p->nParams == 0) {
		p->nParams = 1;
		p->params[0] = 0;
	}
	for (int i = 0; i < p->nParams; i++) {
		int v = param(p, i, 0);
		switch (v) {
		case 0:
			t->fgIdx = 7; t->bgIdx = 0;
			t->fgDirect = t->bgDirect = false;
			t->bold = t->blinkAttr = t->reverse = false;
			break;
		case 1: t->bold = true; break;
		case 2: case 22: t->bold = false; break;
		case 5: case 6: t->blinkAttr = true; break;
		case 25: t->blinkAttr = false; break;
		case 7: t->reverse = true; break;
		case 27: t->reverse = false; break;
		case 38: i += extColor(p, i, true); break;
		case 48: i += extColor(p, i, false); break;
		case 39: t->fgIdx = 7; t->fgDirect = false; break;
		case 49: t->bgIdx = 0; t->bgDirect = false; break;
		default:
			if (v >= 30 && v <= 37) { t->fgIdx = v - 30; t->fgDirect = false; }
			else if (v >= 40 && v <= 47) { t->bgIdx = v - 40; t->bgDirect = false; }
			else if (v >= 90 && v <= 97) { t->fgDirect = true; t->fgRGB = palAnsi(v - 90 + 8); }
			else if (v >= 100 && v <= 107) { t->bgDirect = true; t->bgRGB = palAnsi(v - 100 + 8); }
			break;
		}
	}
}

static void doMode(AnsiParser* p, bool set)
{
	Terminal* t = p->term;
	for (int i = 0; i < p->nParams; i++) {
		int v = param(p, i, -1);
		if (p->priv == '?') {
			switch (v) {
			case 7:  t->autowrap = set; break;
			case 25: t->cursorVisible = set; break;
			case 33: t->iceColors = set; break; // CTerm: blink-as-bright-bg
			case 9:    t->mouseX10 = set; break;
			case 1000: t->mouseNormal = set; break;
			case 1006: t->mouseSGR = set; break;
			}
		}
		// non-private and CSI = modes: none implemented yet
	}
	t->rev++;
}

static void doDA(AnsiParser* p)
{
	if (p->priv == 0) {
		// Plain DA: the CTerm banner (what Synchronet's autodetect parses)
		respond(p, ANSI_CTERM_DA);
	} else if (p->priv == '<') {
		// CTDA: capability list. 2 = bright background, 4 = pixel
		// operations (sixel), 7 = mouse — what we actually implement.
		respond(p, "\x1B[<0;2;4;7c");
	}
}

// CSI = Ps n state reports (CTSMRR) — queried by SyncTerm-aware software;
// an unanswered query means the BBS waits out a timeout, so answer them all
static void doStateReport(AnsiParser* p)
{
	Terminal* t = p->term;
	char buf[96];
	int ps = param(p, 0, 1);
	switch (ps) {
	case 1: // font state: first loadable slot; no font selections made
		respond(p, "\x1B[=1;43;0;0;0;0;0n");
		break;
	case 2: { // list of currently-set private modes
		int off = snprintf(buf, sizeof(buf), "\x1B[=2");
		bool any = false;
		struct { int mode; bool on; } modes[] = {
			{ 7, t->autowrap }, { 9, t->mouseX10 }, { 25, t->cursorVisible },
			{ 33, t->iceColors }, { 1000, t->mouseNormal }, { 1006, t->mouseSGR },
		};
		for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
			if (modes[i].on) {
				off += snprintf(buf + off, sizeof(buf) - off, ";%d", modes[i].mode);
				any = true;
			}
		}
		snprintf(buf + off, sizeof(buf) - off, any ? "n" : ";n");
		respond(p, buf);
		break;
	}
	case 3: // character cell size (pixels): height;width
		respond(p, "\x1B[=3;16;8n");
		break;
	case 4: // LCF mode enabled?
		respond(p, "\x1B[=4;0n");
		break;
	case 5: // LCF mode forced?
		respond(p, "\x1B[=5;0n");
		break;
	case 6: // OSC 8 hyperlink support?
		respond(p, "\x1B[=6;0n");
		break;
	case 7: // audio state
		if (p->hooks.audioStatus)
			p->hooks.audioStatus(param(p, 1, -1));
		break;
	}
}

// DECRQM (CSI [?=] Ps $ p) -> DECRPM (CSI [?=] Ps ; Pm $ y)
// Pm: 0 unrecognized, 1 set, 2 reset, 3 permanently set, 4 permanently reset
static void doRequestMode(AnsiParser* p)
{
	Terminal* t = p->term;
	char buf[32];
	int ps = param(p, 0, 0);
	int pm = 0;

	if (p->priv == '?') {
		switch (ps) {
		case 7:    pm = t->autowrap ? 1 : 2; break;
		case 9:    pm = t->mouseX10 ? 1 : 2; break;
		case 25:   pm = t->cursorVisible ? 1 : 2; break;
		case 33:   pm = t->iceColors ? 1 : 2; break;
		case 1000: pm = t->mouseNormal ? 1 : 2; break;
		case 1006: pm = t->mouseSGR ? 1 : 2; break;
		default:   pm = 0; break;
		}
		snprintf(buf, sizeof(buf), "\x1B[?%d;%d$y", ps, pm);
	} else if (p->priv == '=') {
		switch (ps) {
		case 4: case 5: pm = 2; break;  // LCF: reset, not forced
		default: pm = 4; break;         // key reports/DoorWay: not supported
		}
		snprintf(buf, sizeof(buf), "\x1B[=%d;%d$y", ps, pm);
	} else {
		// ANSI modes per CTerm: 14/16 changeable (reset), 21/22 permanently
		// set, other recognized modes permanently reset
		if (ps == 14 || ps == 16) pm = 2;
		else if (ps == 21 || ps == 22) pm = 3;
		else if (ps >= 1 && ps <= 18) pm = 4;
		else pm = 0;
		snprintf(buf, sizeof(buf), "\x1B[%d;%d$y", ps, pm);
	}
	respond(p, buf);
}

static void doWindowOp(AnsiParser* p)
{
	if (param(p, 0, 0) != 8 || !p->hooks.resize)
		return;
	int rows = param(p, 1, 0);
	int cols = param(p, 2, 0);
	p->hooks.resize(cols, rows);
}

static void csiDispatch(AnsiParser* p, u8 final)
{
	Terminal* t = p->term;
	int n = param(p, 0, 1);
	char buf[48];

	// DECRQM: CSI [priv] Ps $ p
	if (p->intermediate == '$' && final == 'p') {
		doRequestMode(p);
		p->state = ST_GROUND;
		return;
	}

	// Sequences with a private marker never fall through to the plain
	// cursor/editing handlers below
	if (p->priv != 0) {
		switch (final) {
		case 'c':
			doDA(p);
			break;
		case 'h': doMode(p, true); break;
		case 'l': doMode(p, false); break;
		case 'n':
			if (p->priv == '=') {
				doStateReport(p);
			} else if (p->priv == '?') {
				if (param(p, 0, 0) == 62) {
					respond(p, "\x1B[32767*{"); // DECMSR: macro space
				} else if (param(p, 0, 0) == 63) {
					// DECCKSR: checksum of (our zero) macros
					snprintf(buf, sizeof(buf), "\x1BP%d!0000\x1B\\",
					         param(p, 1, 1));
					respond(p, buf);
				}
			}
			break;
		case 'S':
			// XTSRGA: CSI ? 2 ; 1 S -> graphics screen size in pixels
			if (p->priv == '?' && param(p, 0, 0) == 2 && param(p, 1, 0) == 1) {
				snprintf(buf, sizeof(buf), "\x1B[?2;0;%d;%dS",
				         t->cols * 8, t->rows * 16);
				respond(p, buf);
			}
			break;
		case 'z':
			// Text depth layers (protocol 0.3):
			//   CSI = Ps z       select active layer (0..15)
			//   CSI = Ps ; Pd * z  set layer Ps depth to Pd centi-world-
			//                      units behind the glass (0 = glass)
			if (p->priv == '=') {
				if (p->intermediate == '*')
					termSetLayerDepth(t, param(p, 0, 0),
					                  param(p, 1, 0) / 100.0f);
				else
					termSelectLayer(t, param(p, 0, 0));
			}
			break;
		default:
			break; // unknown private sequence: swallow
		}
		p->state = ST_GROUND;
		return;
	}

	switch (final) {
	case 'A': termMoveCursor(t, t->cx, t->cy - n); break;
	case 'B': termMoveCursor(t, t->cx, t->cy + n); break;
	case 'C': termMoveCursor(t, t->cx + n, t->cy); break;
	case 'D': termMoveCursor(t, t->cx - n, t->cy); break;
	case 'E': termMoveCursor(t, 0, t->cy + n); break;
	case 'F': termMoveCursor(t, 0, t->cy - n); break;
	case 'G': termMoveCursor(t, n - 1, t->cy); break;
	case 'd': termMoveCursor(t, t->cx, n - 1); break;
	case 'H': case 'f':
		termMoveCursor(t, param(p, 1, 1) - 1, param(p, 0, 1) - 1);
		break;
	case 'J': termClearScreen(t, param(p, 0, 0)); break;
	case 'K': termClearLine(t, param(p, 0, 0)); break;
	case 'L': termInsertLines(t, n); break;
	case 'M':
		if (!p->paramSeen && p->hooks.music) {
			// SyncTERM default: bare CSI M introduces ANSI music
			p->strLen = 0;
			p->state = ST_MUSIC;
			return;
		}
		termDeleteLines(t, n);
		break;
	case 'N': case '|':
		// CSI N (BananaCom) and CSI | (CTerm standard) music introducers
		if (p->hooks.music) {
			p->strLen = 0;
			p->state = ST_MUSIC;
			return;
		}
		break;
	case '@': termInsertChars(t, n); break;
	case 'P': termDeleteChars(t, n); break;
	case 'X': termEraseChars(t, n); break;
	case 'S': termScrollUp(t, n); break;
	case 'T': termScrollDown(t, n); break;
	case 'r': // DECSTBM
		termSetMargins(t, param(p, 0, 1) - 1, param(p, 1, t->rows) - 1);
		break;
	case 's': termSaveCursor(t); break;
	case 'u': termRestoreCursor(t); break;
	case 'h': doMode(p, true); break;
	case 'l': doMode(p, false); break;
	case 'm': doSGR(p); break;
	case 'n':
		switch (param(p, 0, 0)) {
		case 6: // CPR
			snprintf(buf, sizeof(buf), "\x1B[%d;%dR", t->cy + 1, t->cx + 1);
			respond(p, buf);
			break;
		case 5: // DSR: ready
			respond(p, "\x1B[0n");
			break;
		case 255: // BANSI BCDSR: report terminal size as bottom-right CPR
			snprintf(buf, sizeof(buf), "\x1B[%d;%dR", t->rows, t->cols);
			respond(p, buf);
			break;
		}
		break;
	case 'c': doDA(p); break;
	case 't': doWindowOp(p); break;
	default:
		// Unimplemented finals (fonts, mouse, margins...) are ignored
		break;
	}
	p->state = ST_GROUND;
}

static void escDispatch(AnsiParser* p, u8 c)
{
	Terminal* t = p->term;
	switch (c) {
	case '[': csiReset(p); p->state = ST_CSI; return;
	case '_': strStart(p, ANSI_STR_APC); return;
	case 'P': strStart(p, ANSI_STR_DCS); return;
	case ']': strStart(p, ANSI_STR_OSC); return;
	case 'D': termLineFeed(t); break;
	case 'M': // reverse index (region-aware)
		if (t->cy == t->marginTop) termScrollDown(t, 1);
		else if (t->cy > 0) termMoveCursor(t, t->cx, t->cy - 1);
		break;
	case 'E': termCarriageReturn(t); termLineFeed(t); break;
	case '7': termSaveCursor(t); break;
	case '8': termRestoreCursor(t); break;
	case 'c': termReset(t); break;
	default: break; // unsupported ESC sequence, swallow
	}
	p->state = ST_GROUND;
}

static void ground(AnsiParser* p, u8 c)
{
	Terminal* t = p->term;
	switch (c) {
	case 0x1B: p->state = ST_ESC; break;
	case '\r': termCarriageReturn(t); break;
	case '\n': termLineFeed(t); break;
	case '\b': termBackspace(t); break;
	case '\t': termTab(t); break;
	case '\f': termClearScreen(t, 2); break;
	case 0x07: break; // BEL: beep later
	case 0x00: break;
	default:
		termPutGlyph(t, c); // everything else is a CP437 glyph
		break;
	}
}

void ansiFeed(AnsiParser* p, const u8* data, int len)
{
	for (int i = 0; i < len; i++) {
		u8 c = data[i];
		switch (p->state) {
		case ST_GROUND:
			ground(p, c);
			break;

		case ST_ESC:
			escDispatch(p, c);
			break;

		case ST_CSI:
			if (c >= '0' && c <= '9') {
				if (p->nParams == 0) p->nParams = 1;
				int* v = &p->params[p->nParams - 1];
				if (*v < 0) *v = 0;
				*v = *v * 10 + (c - '0');
				p->paramSeen = true;
			} else if (c == ';') {
				if (p->nParams == 0) p->nParams = 1;
				if (p->nParams < ANSI_MAX_PARAMS) p->nParams++;
				p->paramSeen = true;
			} else if (c == '<' || c == '=' || c == '?' || c == '>') {
				p->priv = c;
			} else if (c == ' ' || c == '!' || c == '"' || c == '$' || c == '*') {
				p->intermediate = c;
			} else if (c == 0x1B) {
				p->state = ST_ESC; // malformed; restart
			} else if (c >= 0x40 && c <= 0x7E) {
				csiDispatch(p, c);
			} else if (c == 0x07 || c < 0x20) {
				// stray control inside CSI: process CR/LF etc., stay in CSI
				ground(p, c == 0x1B ? 0 : c);
			}
			break;

		case ST_STR:
			if (c == 0x1B) {
				p->state = ST_STR_ESC;
			} else if (c == 0x07 && p->strKind == ANSI_STR_OSC) {
				strDispatch(p); // BEL terminates OSC
			} else if (p->strLen < ANSI_STR_MAX) {
				p->strBuf[p->strLen++] = c;
			}
			break;

		case ST_STR_ESC:
			if (c == '\\') {
				strDispatch(p);
			} else {
				// Not a terminator; abort string, reprocess as escape
				p->state = ST_ESC;
				escDispatch(p, c);
			}
			break;

		case ST_MUSIC:
			if (c == 0x0E) {
				if (p->hooks.music)
					p->hooks.music(p->strBuf, p->strLen);
				p->state = ST_GROUND;
			} else if (p->strLen < ANSI_STR_MAX) {
				p->strBuf[p->strLen++] = c;
			}
			break;
		}
	}
}
