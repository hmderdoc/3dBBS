#include "palette.h"

// VGA text colors, ANSI index order
static const u32 vga[16] = {
	0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF0055AA,
	0xFFAA0000, 0xFFAA00AA, 0xFFAAAA00, 0xFFAAAAAA,
	0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
	0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

static u32 table[256];
static bool inited;

static void initTable(void)
{
	static const u8 cube[6] = { 0, 95, 135, 175, 215, 255 };
	for (int i = 0; i < 16; i++)
		table[i] = vga[i];
	for (int i = 0; i < 216; i++)
		table[16 + i] = palRGB(cube[i / 36], cube[(i / 6) % 6], cube[i % 6]);
	for (int i = 0; i < 24; i++) {
		u8 v = 8 + i * 10;
		table[232 + i] = palRGB(v, v, v);
	}
	inited = true;
}

u32 palAnsi(int idx)
{
	if (!inited)
		initTable();
	return table[idx & 255];
}
