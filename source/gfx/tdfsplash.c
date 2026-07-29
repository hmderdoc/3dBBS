#include <3ds.h>
#include <math.h>
#include <string.h>
#include "tdfsplash.h"
#include "tdf_splash_data.h"
#include "termgfx.h"
#include "../term/palette.h"

#define SCREEN_W 400.0f
#define SCREEN_H 240.0f
#define GLYPH_W  8.0f
#define GLYPH_H  16.0f

// Perspective. FOCAL is deliberately large relative to the depth range:
// with a short focal length everything far away collapses onto the
// vanishing point, so banners would all spawn stacked in the middle of the
// screen no matter where they were placed. A long lens keeps them spread
// while still growing convincingly as they approach.
#define FOCAL     2200.0f
#define Z_SCREEN  1200.0f   // this depth sits on the display plane
#define Z_FAR     3400.0f
#define Z_NEAR      60.0f   // banners travel almost into your face

// Peak horizontal disparity in pixels, at full slider, for the nearest
// banner. The slider gives iod in 0..1/3, so it is normalised first —
// without that the whole stereo effect was worth about three pixels.
#define MAX_SHIFT 52.0f

// Width of a banner at the screen plane, as a fraction of the display.
// Everything scales from this, so a 6-column font and a 110-column one get
// comparable presence instead of one being invisible.
#define REF_COVERAGE 0.55f

// Four, not five: the banners are wide, and past this they spend more time
// overlapping each other than being readable.
#define NBANNERS 4

// Below this chroma score a banner is a greyscale "silver" variant, and
// gets tinted instead of drawn as-is.
#define GREY_MAX 40

typedef struct {
	int idx;
	float x, y, z;
	float vx, vy, vz;
	float sway, swaySpd;
	float base;
	u32 tint;        // 0 = draw the font's own colours
} Banner;

static Banner banners[NBANNERS];
static u32 rng = 0x2545F491u;

static u32 xrand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static float frand(float lo, float hi)
{
	return lo + (hi - lo) * ((xrand() >> 8) / 16777216.0f);
}

// Vivid hues for tinting the greyscale fonts. Their art is built from the
// shading characters, so multiplying a hue through the existing grey levels
// keeps every bit of that detail — it reads as a coloured font rather than
// a flat recolour. ABGR, like the rest of the renderer.
static const u32 tints[] = {
	0xFFFF6020u,   // cyan-ish blue
	0xFF20E0FFu,   // amber
	0xFF40FF60u,   // green
	0xFFFF40C0u,   // violet
	0xFF6060FFu,   // red
	0xFFFFC040u,   // sky
	0xFF00A0FFu,   // orange
	0xFFFF40FFu,   // magenta
};
#define NTINTS (int)(sizeof(tints) / sizeof(tints[0]))

static void respawn(Banner* b, int lane, bool spread)
{
	b->idx = (int)(xrand() % TDF_BANNER_COUNT);
	const TdfBanner* t = &tdfBanners[b->idx];

	b->tint = (t->chroma < GREY_MAX) ? tints[xrand() % NTINTS] : 0;
	b->base = (REF_COVERAGE * SCREEN_W) / (t->w * GLYPH_W);
	b->z = spread ? frand(Z_NEAR * 3.0f, Z_FAR) : frand(Z_FAR * 0.9f, Z_FAR);

	// Placement is chosen in SCREEN space and divided back out by the
	// depth, because position is multiplied by perspective on the way out.
	// Picking x/y directly put every spawn near the vanishing point — the
	// banners appeared bunched in the middle however wide the range was.
	float persp = FOCAL / (FOCAL + b->z);
	// One horizontal band each, so four banners cannot stack up on the
	// same line; they are far wider than they are tall, so separating them
	// vertically is what actually reduces overlap.
	float band = ((lane + 0.5f) / NBANNERS - 0.5f) * 0.86f;
	float sy = (band + frand(-0.06f, 0.06f)) * SCREEN_H;
	float sx = frand(-0.16f, 0.16f) * SCREEN_W;

	b->x = sx / persp;
	b->y = sy / persp;
	b->vx = frand(-0.30f, 0.30f);
	b->vy = frand(-0.10f, 0.10f);
	b->vz = frand(-6.5f, -2.6f);
	b->sway = frand(0.0f, 6.28f);
	b->swaySpd = frand(0.004f, 0.012f);
}

void tdfSplashInit(void)
{
	// osGetTime() alone is a poor xorshift seed: consecutive boots differ in
	// only the low bits, and the generator needs a few rounds before its
	// output decorrelates. Without the mixing and warm-up the opening
	// banners tended to clump on similar fonts.
	u64 t = osGetTime();
	rng ^= (u32)t * 2654435761u;
	rng ^= (u32)(t >> 32) * 40503u;
	rng |= 1u;
	for (int i = 0; i < 32; i++)
		xrand();

	for (int i = 0; i < NBANNERS; i++)
		respawn(&banners[i], i, true);
}

void tdfSplashUpdate(void)
{
	for (int i = 0; i < NBANNERS; i++) {
		Banner* b = &banners[i];
		b->z += b->vz;
		b->x += b->vx;
		b->y += b->vy;
		b->sway += b->swaySpd;
		if (b->z <= Z_NEAR)
			respawn(b, i, false);
	}
}

static float depthFade(float z)
{
	if (z > Z_FAR * 0.82f)
		return 1.0f - (z - Z_FAR * 0.82f) / (Z_FAR * 0.18f);
	if (z < Z_NEAR * 4.0f)
		return (z - Z_NEAR) / (Z_NEAR * 3.0f);
	return 1.0f;
}

static u32 dim(u32 abgr, float f)
{
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	u32 r = (u32)(( abgr        & 0xFF) * f);
	u32 g = (u32)(((abgr >>  8) & 0xFF) * f);
	u32 b = (u32)(((abgr >> 16) & 0xFF) * f);
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// Multiply a hue through a palette entry's brightness. Grey levels become
// shades of the tint, so the font's ░▒▓█ shading survives intact.
static u32 tinted(u32 pal, u32 tint)
{
	u32 r = pal & 0xFF, g = (pal >> 8) & 0xFF, b = (pal >> 16) & 0xFF;
	u32 level = r > g ? (r > b ? r : b) : (g > b ? g : b);
	u32 tr = ((tint & 0xFF) * level) / 255;
	u32 tg = (((tint >> 8) & 0xFF) * level) / 255;
	u32 tb = (((tint >> 16) & 0xFF) * level) / 255;
	return 0xFF000000u | (tb << 16) | (tg << 8) | tr;
}

static void drawBanner(const Banner* b, float iod)
{
	const TdfBanner* t = &tdfBanners[b->idx];
	float persp = FOCAL / (FOCAL + b->z);
	float scale = b->base * persp;
	float cw = GLYPH_W * scale, ch = GLYPH_H * scale;

	// The slider hands us iod in 0..1/3; normalise so MAX_SHIFT means what
	// it says. Zero disparity at Z_SCREEN, so banners start behind the
	// glass and come out through it.
	float perspS = FOCAL / (FOCAL + Z_SCREEN);
	float perspN = FOCAL / (FOCAL + Z_NEAR);
	float shift = (iod * 3.0f) * MAX_SHIFT
	              * ((persp - perspS) / (perspN - perspS));

	float sway = 16.0f * persp;
	float ox = SCREEN_W * 0.5f + (b->x + sway * sinf(b->sway)) * persp
	           - (t->w * cw) * 0.5f + shift;
	float oy = SCREEN_H * 0.5f + b->y * persp - (t->h * ch) * 0.5f;

	if (ox > SCREEN_W || oy > SCREEN_H ||
	    ox + t->w * cw < 0 || oy + t->h * ch < 0)
		return;

	float fade = depthFade(b->z);
	const u8* cells = &tdfCells[t->off];

	for (int y = 0; y < t->h; y++) {
		float py = oy + y * ch;
		if (py + ch < 0 || py > SCREEN_H)
			continue;
		for (int x = 0; x < t->w; x++) {
			u8 c = cells[(y * t->w + x) * 2];
			u8 attr = cells[(y * t->w + x) * 2 + 1];
			float px = ox + x * cw;
			if (px + cw < 0 || px > SCREEN_W)
				continue;
			u8 bgi = (attr >> 4) & 0x07;
			if (bgi) {
				u32 col = palAnsi(bgi);
				if (b->tint)
					col = tinted(col, b->tint);
				C2D_DrawRectSolid(px, py, 0.4f, cw, ch, dim(col, fade));
			}
			if (c != ' ' && c != 0) {
				u32 col = palAnsi(attr & 0x0F);
				if (b->tint)
					col = tinted(col, b->tint);
				termgfxDrawChar(px, py, scale, dim(col, fade), c);
			}
		}
	}
}

void tdfSplashRender(float iod)
{
	int order[NBANNERS];
	for (int i = 0; i < NBANNERS; i++)
		order[i] = i;
	for (int i = 1; i < NBANNERS; i++) {
		int k = order[i], j = i;
		while (j > 0 && banners[order[j - 1]].z < banners[k].z) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = k;
	}
	for (int i = 0; i < NBANNERS; i++)
		drawBanner(&banners[order[i]], iod);
}
