// Host tool: decode a captured sixel payload (from the proxy's /tmp dumps)
// and report dimensions, color histogram, and a coarse ASCII preview.
// Build/run: cc -I . -o /tmp/sixdump sixel_dump.c ../../source/gfx/sixel.c && /tmp/sixdump /tmp/sixel_0.bin
#include <stdio.h>
#include <stdlib.h>
#include "../../source/gfx/sixel.h"

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <sixel.bin>\n", argv[0]);
		return 1;
	}
	FILE* f = fopen(argv[1], "rb");
	if (!f) { perror("open"); return 1; }
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* data = malloc(len);
	fread(data, 1, len, f);
	fclose(f);

	printf("payload: %ld bytes, head: %.40s\n", len, (char*)data);

	u32* rgba;
	int w, h;
	if (!sixelDecode(data, len, &rgba, &w, &h)) {
		printf("DECODE FAILED\n");
		return 1;
	}
	printf("decoded %dx%d\n", w, h);

	// Color histogram (top 8)
	typedef struct { u32 c; int n; } Ent;
	Ent ents[256] = {0};
	int nents = 0;
	for (int i = 0; i < w * h; i++) {
		u32 c = rgba[i];
		int j;
		for (j = 0; j < nents; j++)
			if (ents[j].c == c) { ents[j].n++; break; }
		if (j == nents && nents < 256) { ents[nents].c = c; ents[nents++].n = 1; }
	}
	for (int i = 0; i < nents; i++)  // crude sort
		for (int j = i + 1; j < nents; j++)
			if (ents[j].n > ents[i].n) { Ent t = ents[i]; ents[i] = ents[j]; ents[j] = t; }
	printf("top colors (rgba r|g<<8|b<<16|a<<24):\n");
	for (int i = 0; i < nents && i < 8; i++)
		printf("  #%08x x%d\n", ents[i].c, ents[i].n);

	// ASCII preview, ~60 cols
	int step = w / 60 + 1;
	for (int y = 0; y < h; y += step * 2) {
		for (int x = 0; x < w; x += step) {
			u32 c = rgba[y * w + x];
			int lum = (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF);
			int alpha = (c >> 24) & 0xFF;
			putchar(alpha < 128 ? ' ' : lum > 500 ? '#' : lum > 250 ? '+' : lum > 60 ? '.' : '_');
		}
		putchar('\n');
	}
	free(rgba);
	free(data);
	return 0;
}
