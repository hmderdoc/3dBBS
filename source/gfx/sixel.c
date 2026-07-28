// Sixel decoder per the CTerm/VT340 dialect (vendor/synchronet/cterm.adoc):
// '"' raster attributes, '#' palette select/define (RGB + HLS), '!' RLE,
// '$' CR, '-' LF, data chars 0x3F-0x7E painting 6-pixel columns.
#include <stdlib.h>
#include <string.h>
#include "sixel.h"

// VT340 default palette (registers 0-15), r,g,b
static const u8 defPal[16][3] = {
	{0,0,0},{51,51,204},{204,33,33},{51,204,51},{204,51,204},{51,204,204},
	{204,204,51},{135,135,135},{66,66,66},{84,84,153},{153,66,66},{84,153,84},
	{153,84,153},{84,153,153},{153,153,84},{204,204,204},
};

static u32 packRGB(u8 r, u8 g, u8 b)
{
	return (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u;
}

static u32 hlsToRgb(int h, int l, int s)
{
	// DEC HLS: H 0-360 (blue at 0), L 0-100, S 0-100
	float H = (float)((h + 240) % 360), L = l / 100.0f, S = s / 100.0f;
	float c = (1.0f - (L * 2.0f - 1.0f < 0 ? -(L * 2.0f - 1.0f) : L * 2.0f - 1.0f)) * S;
	float hp = H / 60.0f;
	float m = hp - (int)(hp / 2) * 2; // fmod(hp,2)
	float x = c * (1.0f - (m - 1.0f < 0 ? -(m - 1.0f) : m - 1.0f));
	float r = 0, g = 0, b = 0;
	if (hp < 1)      { r = c; g = x; }
	else if (hp < 2) { r = x; g = c; }
	else if (hp < 3) { g = c; b = x; }
	else if (hp < 4) { g = x; b = c; }
	else if (hp < 5) { r = x; b = c; }
	else             { r = c; b = x; }
	float mm = L - c / 2.0f;
	return packRGB((u8)((r + mm) * 255), (u8)((g + mm) * 255), (u8)((b + mm) * 255));
}

bool sixelShiftClip(int* yPix, int h, int* clipTop, int* clipBot,
                    int dPix, int bandTop, int bandBot)
{
	int y0 = *yPix + *clipTop, y1 = *yPix + h - *clipBot;
	if (y0 >= bandBot || y1 <= bandTop)
		return false; // visible part not in band
	*yPix -= dPix;
	int visTop = *yPix + *clipTop;
	if (visTop < bandTop)
		*clipTop += bandTop - visTop;
	int visBot = *yPix + h - *clipBot;
	if (visBot > bandBot)
		*clipBot += visBot - bandBot;
	return *clipTop + *clipBot >= h; // fully consumed
}

typedef struct {
	const u8* p;
	const u8* end;
} Cur;

static int num(Cur* c, int def)
{
	if (c->p >= c->end || *c->p < '0' || *c->p > '9')
		return def;
	int v = 0;
	while (c->p < c->end && *c->p >= '0' && *c->p <= '9')
		v = v * 10 + (*c->p++ - '0');
	return v;
}

bool sixelDecode(const u8* data, int len, u32** rgbaOut, int* wOut, int* hOut)
{
	Cur c = { data, data + len };

	// Header: P1 ; P2 ; P3 q  — P2==1 means unset pixels stay transparent
	int p2 = 0;
	{
		int idx = 0;
		while (c.p < c.end && *c.p != 'q') {
			if (*c.p == ';') {
				idx++;
				c.p++;
			} else if (*c.p >= '0' && *c.p <= '9') {
				int v = num(&c, 0);
				if (idx == 1)
					p2 = v;
			} else {
				c.p++;
			}
		}
		if (c.p >= c.end)
			return false;
		c.p++; // 'q'
	}
	const u8* body = c.p;

	// Pass 1: dimensions. Raster attributes (") declare a canvas floor —
	// they do NOT advance the drawing position (getting this wrong doubles
	// the canvas height with a black slab; caught against real BBS data)
	int x = 0, y = 0, maxX = 0, rasterV = 0;
	Cur s = { body, c.end };
	while (s.p < s.end) {
		u8 ch = *s.p++;
		if (ch == '!') {
			int n = num(&s, 1);
			if (s.p < s.end && *s.p >= 0x3F && *s.p <= 0x7E) {
				s.p++;
				x += n;
			}
		} else if (ch == '"') {
			num(&s, 1); if (s.p < s.end && *s.p == ';') s.p++;
			num(&s, 1); if (s.p < s.end && *s.p == ';') s.p++;
			int ph = num(&s, 0); if (s.p < s.end && *s.p == ';') s.p++;
			int pv = num(&s, 0);
			if (ph > maxX) maxX = ph;
			if (pv > rasterV) rasterV = pv;
		} else if (ch == '#') {
			num(&s, 0);
			while (s.p < s.end && *s.p == ';') { s.p++; num(&s, 0); }
		} else if (ch == '$') {
			x = 0;
		} else if (ch == '-') {
			x = 0;
			y += 6;
		} else if (ch >= 0x3F && ch <= 0x7E) {
			x++;
		}
		if (x > maxX)
			maxX = x;
	}
	int w = maxX, h = y + 6;
	if (rasterV > h)
		h = rasterV;
	if (w <= 0 || h <= 0)
		return false;
	if (w > SIXEL_MAX_W) w = SIXEL_MAX_W;
	if (h > SIXEL_MAX_H) h = SIXEL_MAX_H;

	u32* img = malloc((size_t)w * h * 4);
	if (!img)
		return false;
	if (p2 == 1) {
		memset(img, 0, (size_t)w * h * 4); // transparent background
	} else {
		for (int i = 0; i < w * h; i++)
			img[i] = packRGB(0, 0, 0);
	}

	// Pass 2: paint
	u32 pal[256];
	for (int i = 0; i < 16; i++)
		pal[i] = packRGB(defPal[i][0], defPal[i][1], defPal[i][2]);
	for (int i = 16; i < 256; i++)
		pal[i] = packRGB(0, 0, 0);

	u32 color = pal[0];
	x = 0;
	y = 0;
	s.p = body;
	while (s.p < s.end) {
		u8 ch = *s.p++;
		int rep = 1;
		if (ch == '!') {
			rep = num(&s, 1);
			if (s.p >= s.end || *s.p < 0x3F || *s.p > 0x7E)
				continue;
			ch = *s.p++;
		} else if (ch == '"') {
			num(&s, 1); if (s.p < s.end && *s.p == ';') s.p++;
			num(&s, 1); if (s.p < s.end && *s.p == ';') s.p++;
			num(&s, 0); if (s.p < s.end && *s.p == ';') s.p++;
			num(&s, 0);
			continue;
		} else if (ch == '#') {
			int reg = num(&s, 0) & 255;
			if (s.p < s.end && *s.p == ';') {
				s.p++;
				int pu = num(&s, 0);
				int a = 0, b = 0, d = 0;
				if (s.p < s.end && *s.p == ';') { s.p++; a = num(&s, 0); }
				if (s.p < s.end && *s.p == ';') { s.p++; b = num(&s, 0); }
				if (s.p < s.end && *s.p == ';') { s.p++; d = num(&s, 0); }
				if (pu == 2)
					pal[reg] = packRGB(a * 255 / 100, b * 255 / 100, d * 255 / 100);
				else if (pu == 1)
					pal[reg] = hlsToRgb(a, b, d);
			}
			color = pal[reg];
			continue;
		} else if (ch == '$') {
			x = 0;
			continue;
		} else if (ch == '-') {
			x = 0;
			y += 6;
			continue;
		}
		if (ch < 0x3F || ch > 0x7E)
			continue;

		u8 bits = ch - 0x3F;
		for (int r = 0; r < rep; r++) {
			if (x < w && bits) {
				for (int i = 0; i < 6; i++) {
					if ((bits & (1 << i)) && y + i < h)
						img[(y + i) * w + x] = color;
				}
			}
			x++;
		}
	}

	*rgbaOut = img;
	*wOut = w;
	*hOut = h;
	return true;
}
