#ifndef TERMBUF_H
#define TERMBUF_H

#include <3ds/types.h>
#include <stdbool.h>

// Dynamic cell-grid terminal model. 80x25 is the supported minimum; the grid
// is heap-allocated and resizable at runtime (phonebook config or CSI 8;r;c t).

#define TERM_MIN_COLS 80
#define TERM_MIN_ROWS 25
#define TERM_MAX_COLS 240
#define TERM_MAX_ROWS 100

typedef struct {
	u32 fg, bg;  // resolved ABGR colors (truecolor-capable)
	u8 ch;       // CP437 glyph index
	u8 blink;    // classic blink attribute (unused when iCE colors active)
} TermCell;

typedef struct {
	int cols, rows;
	TermCell* cells;

	int cx, cy;           // cursor (0-based)
	int savedX, savedY;

	// Current SGR state. Palette-indexed colors resolve at write time so
	// bold (fg+8) and iCE (bg+8) still work; direct colors bypass that.
	u8 fgIdx, bgIdx;      // 0-7 base palette color
	bool fgDirect, bgDirect;
	u32 fgRGB, bgRGB;     // used when *Direct
	bool bold;            // intensity -> fg+8 (palette mode)
	bool blinkAttr;
	bool reverse;

	bool autowrap;        // CSI ?7
	bool cursorVisible;   // CSI ?25
	bool iceColors;       // blink bit = bright background

	// DECSTBM scroll region, 0-based inclusive rows
	int marginTop, marginBot;

	// Mouse reporting modes (Synchronet hotspots use normal+SGR)
	bool mouseX10;        // CSI ?9
	bool mouseNormal;     // CSI ?1000
	bool mouseSGR;        // CSI ?1006 (coordinate encoding)

	u32 rev;              // bumped on every mutation (render dirty check)

	// Optional overlay-graphics coupling (sixel images track the grid).
	// topRow/botRow bound the scrolled band (0-based inclusive).
	void (*onScroll)(int rows, int topRow, int botRow);
	void (*onClearAll)(void);
	// Cells [x0..x1] of `row` were overwritten by text/erase — graphics
	// under them are destroyed (real terminals rasterize sixel into the
	// framebuffer, so any overwrite replaces those pixels)
	void (*onOverwrite)(int x0, int x1, int row);
} Terminal;

// DECSTBM (CSI r); rows 0-based inclusive, resets cursor to home
void termSetMargins(Terminal* t, int top, int bot);

bool termInit(Terminal* t, int cols, int rows);
void termFree(Terminal* t);
bool termResize(Terminal* t, int cols, int rows);   // clears screen
void termReset(Terminal* t);                        // ESC c / fresh connect

// Writing (parser calls these)
void termPutGlyph(Terminal* t, u8 ch);   // write at cursor, advance, wrap, scroll
void termLineFeed(Terminal* t);
void termCarriageReturn(Terminal* t);
void termBackspace(Terminal* t);
void termTab(Terminal* t);

// Cursor (coordinates 0-based, clamped)
void termMoveCursor(Terminal* t, int x, int y);
void termSaveCursor(Terminal* t);
void termRestoreCursor(Terminal* t);

// Editing ops (n >= 1)
void termClearScreen(Terminal* t, int mode);  // 0=cursor..end 1=start..cursor 2=all+home
void termClearLine(Terminal* t, int mode);
void termScrollUp(Terminal* t, int n);
void termScrollDown(Terminal* t, int n);
void termInsertLines(Terminal* t, int n);
void termDeleteLines(Terminal* t, int n);
void termInsertChars(Terminal* t, int n);
void termDeleteChars(Terminal* t, int n);
void termEraseChars(Terminal* t, int n);

// Effective cell colors for current SGR state (resolved ABGR)
u32 termCurFg(const Terminal* t);
u32 termCurBg(const Terminal* t);

#endif
