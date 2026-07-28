#include <stdlib.h>
#include <string.h>
#include "termbuf.h"

#include "palette.h"

static u32 resolveFg(const Terminal* t)
{
	if (t->fgDirect)
		return t->fgRGB;
	return palAnsi(t->fgIdx + (t->bold ? 8 : 0));
}

static u32 resolveBg(const Terminal* t)
{
	if (t->bgDirect)
		return t->bgRGB;
	return palAnsi(t->bgIdx + (t->iceColors && t->blinkAttr ? 8 : 0));
}

u32 termCurFg(const Terminal* t)
{
	return t->reverse ? resolveBg(t) : resolveFg(t);
}

u32 termCurBg(const Terminal* t)
{
	return t->reverse ? resolveFg(t) : resolveBg(t);
}

static TermCell blankCell(const Terminal* t)
{
	TermCell c = { termCurFg(t), termCurBg(t), ' ', 0 };
	return c;
}

static void fillCells(Terminal* t, int from, int count, TermCell c)
{
	for (int i = 0; i < count; i++)
		t->cells[from + i] = c;
}

bool termInit(Terminal* t, int cols, int rows)
{
	memset(t, 0, sizeof(*t));
	t->cells = NULL;
	return termResize(t, cols, rows);
}

void termFree(Terminal* t)
{
	free(t->cells);
	t->cells = NULL;
}

bool termResize(Terminal* t, int cols, int rows)
{
	if (cols < TERM_MIN_COLS) cols = TERM_MIN_COLS;
	if (rows < TERM_MIN_ROWS) rows = TERM_MIN_ROWS;
	if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
	if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;

	TermCell* cells = malloc(sizeof(TermCell) * cols * rows);
	if (!cells)
		return false;
	free(t->cells);
	t->cells = cells;
	t->cols = cols;
	t->rows = rows;
	termReset(t);
	return true;
}

void termReset(Terminal* t)
{
	t->cx = t->cy = t->savedX = t->savedY = 0;
	t->fgIdx = 7;
	t->bgIdx = 0;
	t->fgDirect = t->bgDirect = false;
	t->bold = t->blinkAttr = t->reverse = false;
	t->autowrap = true;
	t->cursorVisible = true;
	t->iceColors = false;
	t->mouseX10 = t->mouseNormal = t->mouseSGR = false;
	t->marginTop = 0;
	t->marginBot = t->rows - 1;
	fillCells(t, 0, t->cols * t->rows, blankCell(t));
	if (t->onClearAll)
		t->onClearAll();
	t->rev++;
}

void termSetMargins(Terminal* t, int top, int bot)
{
	if (top < 0) top = 0;
	if (bot >= t->rows) bot = t->rows - 1;
	if (top >= bot) {
		top = 0;
		bot = t->rows - 1;
	}
	t->marginTop = top;
	t->marginBot = bot;
	termMoveCursor(t, 0, 0); // DECSTBM homes the cursor
}

// Scroll within the DECSTBM region only (region defaults to full screen)
void termScrollUp(Terminal* t, int n)
{
	int top = t->marginTop, bot = t->marginBot;
	int span = bot - top + 1;
	if (n < 1) n = 1;
	if (n > span) n = span;
	memmove(t->cells + top * t->cols, t->cells + (top + n) * t->cols,
	        sizeof(TermCell) * (span - n) * t->cols);
	fillCells(t, (bot - n + 1) * t->cols, n * t->cols, blankCell(t));
	if (t->onScroll)
		t->onScroll(n, top, bot);
	t->rev++;
}

void termScrollDown(Terminal* t, int n)
{
	int top = t->marginTop, bot = t->marginBot;
	int span = bot - top + 1;
	if (n < 1) n = 1;
	if (n > span) n = span;
	memmove(t->cells + (top + n) * t->cols, t->cells + top * t->cols,
	        sizeof(TermCell) * (span - n) * t->cols);
	fillCells(t, top * t->cols, n * t->cols, blankCell(t));
	if (t->onScroll)
		t->onScroll(-n, top, bot);
	t->rev++;
}

void termPutGlyph(Terminal* t, u8 ch)
{
	if (t->onOverwrite)
		t->onOverwrite(t->cx, t->cx, t->cy);
	TermCell* c = &t->cells[t->cy * t->cols + t->cx];
	c->ch = ch;
	c->fg = termCurFg(t);
	c->bg = termCurBg(t);
	c->blink = (!t->iceColors && t->blinkAttr) ? 1 : 0;

	if (t->cx < t->cols - 1) {
		t->cx++;
	} else if (t->autowrap) {
		// CTerm semantics: wrap immediately on writing the last column
		t->cx = 0;
		if (t->cy == t->marginBot)
			termScrollUp(t, 1);
		else if (t->cy < t->rows - 1)
			t->cy++;
	}
	t->rev++;
}

void termLineFeed(Terminal* t)
{
	if (t->cy == t->marginBot)
		termScrollUp(t, 1);
	else if (t->cy < t->rows - 1)
		t->cy++;
	t->rev++;
}

void termCarriageReturn(Terminal* t)
{
	t->cx = 0;
	t->rev++;
}

void termBackspace(Terminal* t)
{
	if (t->cx > 0)
		t->cx--;
	t->rev++;
}

void termTab(Terminal* t)
{
	if (t->cx >= t->cols - 1) {
		// CTerm: tab wraps only from the last column
		termCarriageReturn(t);
		termLineFeed(t);
		return;
	}
	t->cx = (t->cx / 8 + 1) * 8;
	if (t->cx > t->cols - 1)
		t->cx = t->cols - 1;
	t->rev++;
}

void termMoveCursor(Terminal* t, int x, int y)
{
	t->cx = x < 0 ? 0 : (x >= t->cols ? t->cols - 1 : x);
	t->cy = y < 0 ? 0 : (y >= t->rows ? t->rows - 1 : y);
	t->rev++;
}

void termSaveCursor(Terminal* t)
{
	t->savedX = t->cx;
	t->savedY = t->cy;
}

void termRestoreCursor(Terminal* t)
{
	termMoveCursor(t, t->savedX, t->savedY);
}

void termClearScreen(Terminal* t, int mode)
{
	int cur = t->cy * t->cols + t->cx;
	int total = t->cols * t->rows;
	switch (mode) {
	case 0:
		fillCells(t, cur, total - cur, blankCell(t));
		if (t->onOverwrite) {
			for (int r = t->cy; r < t->rows; r++)
				t->onOverwrite(r == t->cy ? t->cx : 0, t->cols - 1, r);
		}
		break;
	case 1:
		fillCells(t, 0, cur + 1, blankCell(t));
		if (t->onOverwrite) {
			for (int r = 0; r <= t->cy; r++)
				t->onOverwrite(0, r == t->cy ? t->cx : t->cols - 1, r);
		}
		break;
	default:
		fillCells(t, 0, total, blankCell(t));
		t->cx = t->cy = 0;
		if (t->onClearAll)
			t->onClearAll();
		break;
	}
	t->rev++;
}

void termClearLine(Terminal* t, int mode)
{
	int line = t->cy * t->cols;
	int x0 = 0, x1 = t->cols - 1;
	switch (mode) {
	case 0: fillCells(t, line + t->cx, t->cols - t->cx, blankCell(t)); x0 = t->cx; break;
	case 1: fillCells(t, line, t->cx + 1, blankCell(t)); x1 = t->cx; break;
	default: fillCells(t, line, t->cols, blankCell(t)); break;
	}
	if (t->onOverwrite)
		t->onOverwrite(x0, x1, t->cy);
	t->rev++;
}

// IL/DL are how doors scroll lists: they shift the band [cy..bot] exactly
// like a scroll, so overlay graphics must be notified the same way
void termInsertLines(Terminal* t, int n)
{
	int bot = t->cy > t->marginBot ? t->rows - 1 : t->marginBot;
	int below = bot - t->cy + 1;
	if (n < 1) n = 1;
	if (n > below) n = below;
	memmove(t->cells + (t->cy + n) * t->cols, t->cells + t->cy * t->cols,
	        sizeof(TermCell) * (below - n) * t->cols);
	fillCells(t, t->cy * t->cols, n * t->cols, blankCell(t));
	if (t->onScroll)
		t->onScroll(-n, t->cy, bot);
	t->rev++;
}

void termDeleteLines(Terminal* t, int n)
{
	int bot = t->cy > t->marginBot ? t->rows - 1 : t->marginBot;
	int below = bot - t->cy + 1;
	if (n < 1) n = 1;
	if (n > below) n = below;
	memmove(t->cells + t->cy * t->cols, t->cells + (t->cy + n) * t->cols,
	        sizeof(TermCell) * (below - n) * t->cols);
	fillCells(t, (bot - n + 1) * t->cols, n * t->cols, blankCell(t));
	if (t->onScroll)
		t->onScroll(n, t->cy, bot);
	t->rev++;
}

void termInsertChars(Terminal* t, int n)
{
	if (n < 1) n = 1;
	int rem = t->cols - t->cx;
	if (n > rem) n = rem;
	int line = t->cy * t->cols;
	memmove(t->cells + line + t->cx + n, t->cells + line + t->cx,
	        sizeof(TermCell) * (rem - n));
	fillCells(t, line + t->cx, n, blankCell(t));
	t->rev++;
}

void termDeleteChars(Terminal* t, int n)
{
	if (n < 1) n = 1;
	int rem = t->cols - t->cx;
	if (n > rem) n = rem;
	int line = t->cy * t->cols;
	memmove(t->cells + line + t->cx, t->cells + line + t->cx + n,
	        sizeof(TermCell) * (rem - n));
	fillCells(t, line + t->cols - n, n, blankCell(t));
	t->rev++;
}

void termEraseChars(Terminal* t, int n)
{
	if (n < 1) n = 1;
	int rem = t->cols - t->cx;
	if (n > rem) n = rem;
	fillCells(t, t->cy * t->cols + t->cx, n, blankCell(t));
	if (t->onOverwrite)
		t->onOverwrite(t->cx, t->cx + n - 1, t->cy);
	t->rev++;
}
