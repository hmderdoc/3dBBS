// Shim ciolib.h for host-side font extraction (tools/dumpfont.c).
// Provides just enough for vendor/synchronet/allfonts.c to compile standalone.
#ifndef SHIM_CIOLIB_H
#define SHIM_CIOLIB_H

#include <stdbool.h>

#define CIOLIBEXPORT

enum ciolib_codepage {
	CIOLIB_ARMSCII8, CIOLIB_ATARIST, CIOLIB_ATASCII, CIOLIB_CP1131,
	CIOLIB_CP1251, CIOLIB_CP437, CIOLIB_CP850, CIOLIB_CP865,
	CIOLIB_CP866M, CIOLIB_CP866M2, CIOLIB_CP866U, CIOLIB_HAIK8,
	CIOLIB_ISO_8859_1, CIOLIB_ISO_8859_15, CIOLIB_ISO_8859_2,
	CIOLIB_ISO_8859_4, CIOLIB_ISO_8859_5, CIOLIB_ISO_8859_7,
	CIOLIB_ISO_8859_8, CIOLIB_ISO_8859_9, CIOLIB_KOI8_R, CIOLIB_KOI8_U,
	CIOLIB_PETSCIIL, CIOLIB_PETSCIIU, CIOLIB_PRESTEL
};

struct conio_font_data_struct {
	char* eight_by_sixteen;
	char* eight_by_fourteen;
	char* eight_by_eight;
	char* twelve_by_twenty;
	char* desc;
	enum ciolib_codepage cp;
	bool broken_bar;
};

#endif
