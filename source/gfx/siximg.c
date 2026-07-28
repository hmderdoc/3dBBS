#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include "siximg.h"

#define MAX_IMAGES 16

typedef struct {
	bool used;
	C3D_Tex tex;
	int texPH;            // padded texture height (for crop UVs)
	int xPix, yPix;       // grid-pixel anchor
	int w, h;
	int clipTop, clipBot; // image rows destroyed by scrolling out of a band
} SixImage;

typedef struct Pending {
	struct Pending* next;
	u32* rgba;
	int w, h, xPix, yPix, clipTop, clipBot;
} Pending;

static SixImage images[MAX_IMAGES];
static Pending* pendHead;
static LightLock pendLock;
static u32 statSubmitted, statTextured, statTexFail;
static int statLastW, statLastH;

void siximgInit(void)
{
	LightLock_Init(&pendLock);
}

static void freeImage(SixImage* im)
{
	if (im->used) {
		C3D_TexDelete(&im->tex);
		im->used = false;
	}
}

void siximgExit(void)
{
	siximgClearAll();
	LightLock_Lock(&pendLock);
	while (pendHead) {
		Pending* p = pendHead;
		pendHead = p->next;
		free(p->rgba);
		free(p);
	}
	LightLock_Unlock(&pendLock);
}

void siximgSubmit(u32* rgba, int w, int h, int cellX, int cellY)
{
	Pending* p = malloc(sizeof(Pending));
	if (!p) {
		free(rgba);
		return;
	}
	p->rgba = rgba;
	p->w = w;
	p->h = h;
	p->xPix = cellX * 8;
	p->yPix = cellY * 16;
	p->clipTop = p->clipBot = 0;
	LightLock_Lock(&pendLock);
	p->next = pendHead;
	pendHead = p;
	statSubmitted++;
	statLastW = w;
	statLastH = h;
	LightLock_Unlock(&pendLock);
}

static u32 npot(u32 v)
{
	u32 r = 8;
	while (r < v)
		r <<= 1;
	return r;
}

// 3DS textures are 8x8-tile Morton order
static inline u32 mortonOfs(u32 x, u32 y)
{
	static const u32 xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
	static const u32 yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
	return xl[x & 7] | yl[y & 7];
}

static void makeTexture(SixImage* im, const u32* rgba, int w, int h)
{
	u32 pw = npot(w), ph = npot(h);
	// RGBA5551: sixel color resolution doesn't need 8bpc, alpha is binary,
	// and big frames at 2 bytes/texel halve linear-memory pressure
	if (!C3D_TexInit(&im->tex, pw, ph, GPU_RGBA5551)) {
		statTexFail++;
		return;
	}
	C3D_TexSetFilter(&im->tex, GPU_NEAREST, GPU_NEAREST);

	u16* dst = im->tex.data;
	memset(dst, 0, (size_t)pw * ph * 2);
	for (int y = 0; y < h; y++) {
		u32 ty = y; // measured on hardware: v=1.0 samples memory row 0
		for (int x = 0; x < w; x++) {
			u32 px = rgba[y * w + x];
			u16 texel = (u16)((((px) & 0xF8) << 8) |        // R -> 15..11
			                  (((px >> 8) & 0xF8) << 3) |   // G -> 10..6
			                  (((px >> 16) & 0xF8) >> 2) |  // B -> 5..1
			                  ((px >> 31) & 1));            // A -> 0
			u32 ofs = mortonOfs(x, ty) + (((x >> 3) + (ty >> 3) * (pw >> 3)) << 6);
			dst[ofs] = texel;
		}
	}
	C3D_TexFlush(&im->tex);

	im->texPH = ph;
	im->w = w;
	im->h = h;
	im->used = true;
	statTextured++;
}

void siximgDebugStats(u32* submitted, u32* textured, u32* texFail,
                      int* lastW, int* lastH)
{
	LightLock_Lock(&pendLock);
	*submitted = statSubmitted;
	*textured = statTextured;
	*texFail = statTexFail;
	*lastW = statLastW;
	*lastH = statLastH;
	LightLock_Unlock(&pendLock);
}

void siximgPoll(void)
{
	for (;;) {
		LightLock_Lock(&pendLock);
		Pending* p = pendHead;
		if (p)
			pendHead = p->next;
		LightLock_Unlock(&pendLock);
		if (!p)
			return;

		// A new image REPLACES anything it overlaps (streamed frames at one
		// spot hold one texture; stale art doesn't linger as artifacts)
		for (int i = 0; i < MAX_IMAGES; i++) {
			SixImage* o = &images[i];
			if (o->used &&
			    p->xPix < o->xPix + o->w && p->xPix + p->w > o->xPix &&
			    p->yPix < o->yPix + o->h && p->yPix + p->h > o->yPix)
				freeImage(o);
		}

		// Find a free slot, else evict the oldest (index 0, shift down)
		SixImage* im = NULL;
		for (int i = 0; i < MAX_IMAGES; i++) {
			if (!images[i].used) {
				im = &images[i];
				break;
			}
		}
		if (!im) {
			freeImage(&images[0]);
			memmove(&images[0], &images[1], sizeof(SixImage) * (MAX_IMAGES - 1));
			images[MAX_IMAGES - 1].used = false;
			im = &images[MAX_IMAGES - 1];
		}
		im->xPix = p->xPix;
		im->yPix = p->yPix;
		im->clipTop = p->clipTop;
		im->clipBot = p->clipBot;
		makeTexture(im, p->rgba, p->w, p->h);
		if (!im->used) {
			// Allocation failed: evict the oldest live image and retry once
			for (int i = 0; i < MAX_IMAGES; i++) {
				if (images[i].used) {
					freeImage(&images[i]);
					break;
				}
			}
			makeTexture(im, p->rgba, p->w, p->h);
		}
		free(p->rgba);
		free(p);
	}
}

void siximgDraw(const TermView* v)
{
	for (int i = 0; i < MAX_IMAGES; i++) {
		SixImage* im = &images[i];
		if (!im->used)
			continue;
		int visH = im->h - im->clipTop - im->clipBot;
		if (visH <= 0)
			continue;
		// Crop the destroyed rows out via the subtexture
		Tex3DS_SubTexture sub;
		sub.width = im->w;
		sub.height = visH;
		sub.left = 0.0f;
		sub.right = (float)im->w / im->tex.width;
		sub.top = 1.0f - (float)im->clipTop / im->texPH;
		sub.bottom = 1.0f - (float)(im->h - im->clipBot) / im->texPH;
		C2D_Image img = { &im->tex, &sub };
		float x = v->ox + im->xPix * v->scale;
		float y = v->oy + (im->yPix + im->clipTop) * v->scale;
		C2D_DrawImageAt(img, x, y, 0.6f, NULL, v->scale, v->scale);
	}
}

#include "sixel.h" // sixelShiftClip: scrolled-out rows are destroyed

#define shiftClip sixelShiftClip

void siximgScroll(int rows, int topRow, int botRow)
{
	int dPix = rows * 16;
	int bandTop = topRow * 16, bandBot = (botRow + 1) * 16;

	for (int i = 0; i < MAX_IMAGES; i++) {
		SixImage* im = &images[i];
		if (!im->used)
			continue;
		if (shiftClip(&im->yPix, im->h, &im->clipTop, &im->clipBot,
		              dPix, bandTop, bandBot))
			freeImage(im);
	}
	// Images still decoding on the worker must track scrolls too, or they
	// arrive anchored to where the content USED to be (rendered too low)
	LightLock_Lock(&pendLock);
	for (Pending* p = pendHead; p; p = p->next)
		shiftClip(&p->yPix, p->h, &p->clipTop, &p->clipBot,
		          dPix, bandTop, bandBot);
	LightLock_Unlock(&pendLock);
}

static u32 statClears;

void siximgClearAll(void)
{
	for (int i = 0; i < MAX_IMAGES; i++)
		freeImage(&images[i]);
	statClears++;
}

// v1 semantics: any text overwrite touching an image destroys the whole
// image (real terminals destroy only the overlapped pixels; doors repaint
// entire regions, so whole-image drop converges to the same result)
void siximgOverwrite(int x0, int x1, int row)
{
	int rx0 = x0 * 8, rx1 = (x1 + 1) * 8;
	int ry0 = row * 16, ry1 = (row + 1) * 16;

	for (int i = 0; i < MAX_IMAGES; i++) {
		SixImage* im = &images[i];
		if (!im->used)
			continue;
		int iy0 = im->yPix + im->clipTop, iy1 = im->yPix + im->h - im->clipBot;
		if (rx0 < im->xPix + im->w && rx1 > im->xPix &&
		    ry0 < iy1 && ry1 > iy0)
			freeImage(im);
	}

	LightLock_Lock(&pendLock);
	Pending** pp = &pendHead;
	while (*pp) {
		Pending* p = *pp;
		int py0 = p->yPix + p->clipTop, py1 = p->yPix + p->h - p->clipBot;
		if (rx0 < p->xPix + p->w && rx1 > p->xPix && ry0 < py1 && ry1 > py0) {
			*pp = p->next;
			free(p->rgba);
			free(p);
		} else {
			pp = &p->next;
		}
	}
	LightLock_Unlock(&pendLock);
}

void siximgDebugLive(int* live, u32* clears)
{
	int n = 0;
	for (int i = 0; i < MAX_IMAGES; i++)
		if (images[i].used)
			n++;
	*live = n;
	*clears = statClears;
}
