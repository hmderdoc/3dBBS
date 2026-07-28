// Host-side tool: extracts raw font bitmaps from vendor/synchronet/allfonts.c
// Build/run via tools/mkfont.sh — compiles against a shim ciolib.h (below)
// so we don't drag in the real ciolib headers.
//
// Usage: dumpfont <index> <height> <outfile>
//   index:  conio_fontdata[] entry (0 = Codepage 437 English)
//   height: 16, 14, or 8 (selects the 8x16 / 8x14 / 8x8 bitmap)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allfonts.c"

int main(int argc, char** argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s <index> <height> <outfile>\n", argv[0]);
		return 1;
	}
	int idx = atoi(argv[1]);
	int height = atoi(argv[2]);

	const struct conio_font_data_struct* f = &conio_fontdata[idx];
	const char* data =
		height == 16 ? f->eight_by_sixteen :
		height == 14 ? f->eight_by_fourteen :
		height == 8  ? f->eight_by_eight : NULL;
	if (!data) {
		fprintf(stderr, "no %dpx bitmap for entry %d (%s)\n", height, idx, f->desc);
		return 1;
	}

	FILE* out = fopen(argv[3], "wb");
	if (!out) { perror("fopen"); return 1; }
	fwrite(data, 1, 256 * height, out);
	fclose(out);
	fprintf(stderr, "wrote %d bytes: entry %d '%s' %dpx\n", 256 * height, idx, f->desc, height);
	return 0;
}
