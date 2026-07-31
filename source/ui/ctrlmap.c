#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ctrlmap.h"

#define CM_DIR  "sdmc:/3dBBS"
#define CM_PATH CM_DIR "/controls.txt"

// EVDEV_KEY_* codes. These are the authoritative identity of a binding:
// layout independent, and reported on release as well as press.
#define EV_ESC 1
#define EV_BACKSPACE 14
#define EV_TAB 15
#define EV_ENTER 28
#define EV_SPACE 57
#define EV_HOME 102
#define EV_UP 103
#define EV_PAGEUP 104
#define EV_LEFT 105
#define EV_RIGHT 106
#define EV_END 107
#define EV_DOWN 108
#define EV_PAGEDOWN 109

// The byte fallbacks are the ANSI-BBS sequences Synchronet actually decodes
// (exec/dorkit/ansi_input.js): Home ESC[H, End ESC[K, PgUp ESC[V, PgDn ESC[U
// — the classic BBS set, not the xterm one.
typedef struct { const char* name; u16 evdev; const char* bytes; } Action;

static const Action actions[] = {
	{ "(unbound)",  0,            ""        },
	{ "Enter",      EV_ENTER,     "\r"      },
	{ "Escape",     EV_ESC,       "\x1B"    },
	{ "Backspace",  EV_BACKSPACE, "\b"      },
	{ "Tab",        EV_TAB,       "\t"      },
	{ "Space",      EV_SPACE,     " "       },
	{ "Up",         EV_UP,        "\x1B[A"  },
	{ "Down",       EV_DOWN,      "\x1B[B"  },
	{ "Right",      EV_RIGHT,     "\x1B[C"  },
	{ "Left",       EV_LEFT,      "\x1B[D"  },
	{ "Page Up",    EV_PAGEUP,    "\x1B[V"  },
	{ "Page Down",  EV_PAGEDOWN,  "\x1B[U"  },
	{ "Home",       EV_HOME,      "\x1B[H"  },
	{ "End",        EV_END,       "\x1B[K"  },
	// Letters and digits: evdev codes are not alphabetical, so they are
	// spelled out rather than computed.
	{ "a", 30, "a" }, { "b", 48, "b" }, { "c", 46, "c" }, { "d", 32, "d" },
	{ "e", 18, "e" }, { "f", 33, "f" }, { "g", 34, "g" }, { "h", 35, "h" },
	{ "i", 23, "i" }, { "j", 36, "j" }, { "k", 37, "k" }, { "l", 38, "l" },
	{ "m", 50, "m" }, { "n", 49, "n" }, { "o", 24, "o" }, { "p", 25, "p" },
	{ "q", 16, "q" }, { "r", 19, "r" }, { "s", 31, "s" }, { "t", 20, "t" },
	{ "u", 22, "u" }, { "v", 47, "v" }, { "w", 17, "w" }, { "x", 45, "x" },
	{ "y", 21, "y" }, { "z", 44, "z" },
	{ "1", 2, "1" }, { "2", 3, "2" }, { "3", 4, "3" }, { "4", 5, "4" },
	{ "5", 6, "5" }, { "6", 7, "6" }, { "7", 8, "7" }, { "8", 9, "8" },
	{ "9", 10, "9" }, { "0", 11, "0" },
};
#define NACTIONS (int)(sizeof(actions) / sizeof(actions[0]))

static const char* inputNames[CI_COUNT] = {
	"A", "B", "X", "Y", "L", "R", "ZL", "ZR",
	"D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
	"Circle Up", "Circle Down", "Circle Left", "Circle Right",
	"C-Stick Up", "C-Stick Down", "C-Stick Left", "C-Stick Right",
};

static CtrlMap maps[CM_MAX];
static int count;
static int active;
static bool dirty;
static int idleFrames;

int cmActionCount(void) { return NACTIONS; }
const char* cmActionName(int i)
{
	return actions[(i < 0 || i >= NACTIONS) ? 0 : i].name;
}

void cmActionAt(int i, Binding* out)
{
	const Action* a = &actions[(i < 0 || i >= NACTIONS) ? 0 : i];
	memset(out, 0, sizeof(*out));
	out->evdev = a->evdev;
	out->nbytes = (u8)strlen(a->bytes);
	memcpy(out->bytes, a->bytes, out->nbytes);
}

int cmActionIndex(const Binding* b)
{
	for (int i = 0; i < NACTIONS; i++) {
		if (actions[i].evdev == b->evdev &&
		    (int)strlen(actions[i].bytes) == b->nbytes &&
		    !memcmp(actions[i].bytes, b->bytes, b->nbytes))
			return i;
	}
	return -1;
}

const char* cmInputName(CtrlInput in)
{
	return inputNames[(in < 0 || in >= CI_COUNT) ? 0 : in];
}

const char* cmBindingLabel(const Binding* b, char* buf, int cap)
{
	int i = cmActionIndex(b);
	if (i >= 0)
		return actions[i].name;
	snprintf(buf, cap, "custom (%u)", (unsigned)b->evdev);
	return buf;
}

static void setBind(CtrlMap* m, CtrlInput in, int actionIdx)
{
	cmActionAt(actionIdx, &m->bind[in]);
}

// The stock mapping reproduces what the client did before mappings existed,
// so an upgrade changes nothing until the user goes looking.
static void defaults(CtrlMap* m)
{
	memset(m, 0, sizeof(*m));
	snprintf(m->name, sizeof(m->name), "Default");
	setBind(m, CI_A, 1);          // Enter
	setBind(m, CI_B, 3);          // Backspace
	setBind(m, CI_X, 5);          // Space
	setBind(m, CI_Y, 2);          // Escape
	setBind(m, CI_DUP, 6);
	setBind(m, CI_DDOWN, 7);
	setBind(m, CI_DRIGHT, 8);
	setBind(m, CI_DLEFT, 9);
	m->circle = STICK_DIGITAL;    // circle pad mirrors the d-pad
	setBind(m, CI_CUP, 6);
	setBind(m, CI_CDOWN, 7);
	setBind(m, CI_CRIGHT, 8);
	setBind(m, CI_CLEFT, 9);
	m->cstick = STICK_OFF;
	m->showCursor = true;
	m->repeatDelayMs = 400;
	m->repeatRateMs = 60;
	m->mouseSpeed = 4;
}

int cmCount(void) { return count; }
int cmActive(void) { return active; }

const CtrlMap* cmGet(int i)
{
	if (i < 0 || i >= count)
		i = 0;
	return &maps[i];
}

CtrlMap* cmMutable(int i)
{
	if (i < 0 || i >= count)
		i = 0;
	return &maps[i];
}

void cmSetActive(int i)
{
	if (i >= 0 && i < count && i != active) {
		active = i;
		cmDirty();
	}
}

void cmDirty(void) { dirty = true; idleFrames = 0; }

static void writeFile(void)
{
	FILE* f = fopen(CM_PATH, "w");
	if (!f)
		return;
	fputs("# 3dBBS controller mappings\n"
	      "# map <name>\n"
	      "#   <input> <evdev> <bytes as \\xNN escapes>\n"
	      "# Bindings carry an evdev code AND a byte sequence: doors that\n"
	      "# enable physical key reports get the code (with release edges),\n"
	      "# everything else gets the bytes.\n", f);
	fprintf(f, "active %d\n", active);
	for (int i = 0; i < count; i++) {
		const CtrlMap* m = &maps[i];
		fprintf(f, "map %s\n", m->name);
		fprintf(f, "  sticks %u %u cursor %d mouse %u\n",
		        m->circle, m->cstick, m->showCursor ? 1 : 0, m->mouseSpeed);
		fprintf(f, "  repeat %u %u\n", m->repeatDelayMs, m->repeatRateMs);
		for (int k = 0; k < CI_COUNT; k++) {
			const Binding* b = &m->bind[k];
			if (!b->evdev && !b->nbytes)
				continue;
			fprintf(f, "  %d %u ", k, b->evdev);
			for (int c = 0; c < b->nbytes; c++)
				fprintf(f, "\\x%02X", (unsigned char)b->bytes[c]);
			fputc('\n', f);
		}
	}
	fclose(f);
}

void cmTick(void)
{
	if (!dirty) {
		idleFrames = 0;
		return;
	}
	if (++idleFrames < 30)
		return;
	writeFile();
	dirty = false;
	idleFrames = 0;
}

void cmFlush(void)
{
	if (dirty) {
		writeFile();
		dirty = false;
	}
}

static int unhex(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void parseBytes(Binding* b, const char* s)
{
	b->nbytes = 0;
	while (*s && b->nbytes < (u8)sizeof(b->bytes)) {
		if (s[0] == '\\' && (s[1] == 'x' || s[1] == 'X') &&
		    unhex(s[2]) >= 0 && unhex(s[3]) >= 0) {
			b->bytes[b->nbytes++] = (char)(unhex(s[2]) * 16 + unhex(s[3]));
			s += 4;
		} else {
			b->bytes[b->nbytes++] = *s++;
		}
	}
}

void cmLoad(void)
{
	count = 0;
	active = 0;

	FILE* f = fopen(CM_PATH, "r");
	if (f) {
		char line[160];
		CtrlMap* cur = NULL;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			char* p = line;
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '#' || !*p)
				continue;
			if (!strncmp(p, "active ", 7)) {
				active = atoi(p + 7);
			} else if (!strncmp(p, "map ", 4)) {
				if (count >= CM_MAX)
					break;
				cur = &maps[count++];
				defaults(cur);
				snprintf(cur->name, sizeof(cur->name), "%s", p + 4);
			} else if (cur && !strncmp(p, "sticks ", 7)) {
				unsigned a, b2, c2, d;
				if (sscanf(p, "sticks %u %u cursor %u mouse %u",
				           &a, &b2, &c2, &d) == 4) {
					cur->circle = (u8)a;
					cur->cstick = (u8)b2;
					cur->showCursor = c2 != 0;
					cur->mouseSpeed = (u8)d;
				}
			} else if (cur && !strncmp(p, "repeat ", 7)) {
				unsigned a, b2;
				if (sscanf(p, "repeat %u %u", &a, &b2) == 2) {
					cur->repeatDelayMs = (u16)a;
					cur->repeatRateMs = (u16)b2;
				}
			} else if (cur) {
				int idx;
				unsigned ev;
				char rest[64] = "";
				if (sscanf(p, "%d %u %63s", &idx, &ev, rest) >= 2 &&
				    idx >= 0 && idx < CI_COUNT) {
					Binding* b = &cur->bind[idx];
					memset(b, 0, sizeof(*b));
					b->evdev = (u16)ev;
					parseBytes(b, rest);
				}
			}
		}
		fclose(f);
	}

	if (count == 0) {
		mkdir(CM_DIR, 0777);
		defaults(&maps[0]);
		count = 1;
		cmDirty();
	}
	if (active < 0 || active >= count)
		active = 0;
}

int cmAdd(const char* name)
{
	if (count >= CM_MAX)
		return -1;
	maps[count] = maps[active];   // start from what is already in use
	snprintf(maps[count].name, sizeof(maps[count].name), "%s",
	         name && *name ? name : "New");
	count++;
	cmDirty();
	return count - 1;
}

void cmDelete(int i)
{
	if (i < 0 || i >= count || count <= 1)
		return;
	memmove(&maps[i], &maps[i + 1], sizeof(CtrlMap) * (count - i - 1));
	count--;
	if (active >= count)
		active = count - 1;
	cmDirty();
}
