#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apcaudio.h"
#include "../proto/apc.h"
#include "../proto/b64.h"

#define N_PATCHES   256
#define N_CHANNELS  16
#define FIRST_BBS_CH 2      // 0/1 are cterm-owned (music/fx)
#define SYNTH_RATE  44100   // XPBEEP_SAMPLE_RATE
#define BASE_DB     -12.0f  // SyncTerm's APC channel base level
#define SYNTH_AMP   24575   // 0.75 * 32767

typedef struct {
	s16* frames;   // linear memory, mono PCM16
	u32 nframes;
	u32 rate;
} Patch;

// Unbounded per-channel FIFO (matches SyncTerm's queue semantics): a burst
// of Queue verbs — a melody of Synth notes, streamed WAV chunks — must all
// be accepted, not capped. ndsp chains any number of wave buffers as long
// as each node stays allocated until DONE.
typedef struct QNode {
	struct QNode* next;
	ndspWaveBuf wb;
	s16* data;
} QNode;

typedef struct {
	QNode* head;      // oldest queued buffer
	u32 rate;         // current ndsp channel rate (0 = never set)
	float dbL, dbR;
	bool armed;       // A;Update one-shot notification
	bool wasPlaying;
} Chan;

static bool ok;
static u32 drainNotifies; // cumulative =7;ch;0 emissions (telemetry)
static void (*respondFn)(const u8*, int);
static char cacheDir[128] = "sdmc:/3dBBS/cache/none";
static Patch patches[N_PATCHES];   // touched by the APC worker thread only
static Chan chans[N_CHANNELS];     // shared worker/main: guard with alock
static LightLock alock;

static void respond(const char* s)
{
	if (respondFn)
		respondFn((const u8*)s, strlen(s));
}

bool audioInit(void (*respond_)(const u8*, int))
{
	respondFn = respond_;
	LightLock_Init(&alock);
	ok = R_SUCCEEDED(ndspInit());
	if (ok)
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	for (int i = 0; i < N_CHANNELS; i++)
		chans[i].dbL = chans[i].dbR = BASE_DB;
	return ok;
}

void audioSetCacheDir(const char* dir)
{
	snprintf(cacheDir, sizeof(cacheDir), "%s", dir);
}

static void freePatch(Patch* p)
{
	if (p->frames)
		linearFree(p->frames);
	memset(p, 0, sizeof(*p));
}

static void flushChannel(int ch)
{
	if (!ok)
		return;
	ndspChnWaveBufClear(ch);
	Chan* c = &chans[ch];
	while (c->head) {
		QNode* n = c->head;
		c->head = n->next;
		linearFree(n->data);
		free(n);
	}
}

void audioReset(void)
{
	LightLock_Lock(&alock);
	for (int i = FIRST_BBS_CH; i < N_CHANNELS; i++) {
		flushChannel(i);
		chans[i].dbL = chans[i].dbR = BASE_DB;
		chans[i].armed = chans[i].wasPlaying = false;
		chans[i].rate = 0;
	}
	LightLock_Unlock(&alock);
	for (int i = 0; i < N_PATCHES; i++)
		freePatch(&patches[i]);
}

void audioExit(void)
{
	if (ok) {
		audioReset();
		ndspExit();
		ok = false;
	}
}

void audioPoll(void)
{
	if (!ok)
		return;
	char buf[24];
	LightLock_Lock(&alock);
	for (int ch = FIRST_BBS_CH; ch < N_CHANNELS; ch++) {
		Chan* c = &chans[ch];
		// Buffers complete in FIFO order; free finished ones from the head
		while (c->head && c->head->wb.status == NDSP_WBUF_DONE) {
			QNode* n = c->head;
			c->head = n->next;
			linearFree(n->data);
			free(n);
		}
		bool playing = ndspChnIsPlaying(ch);
		if (c->wasPlaying && !playing && c->armed) {
			snprintf(buf, sizeof(buf), "\x1B[=7;%d;0n", ch);
			respond(buf);
			c->armed = false;
			drainNotifies++;
		}
		c->wasPlaying = playing;
	}
	LightLock_Unlock(&alock);
}

// --- WAV decoding (PCM16/U8, mono/stereo -> mono s16 in linear memory) ---

static bool wavDecode(const u8* buf, int len, Patch* out)
{
	if (len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
		return false;

	u16 fmt = 0, chans_ = 0, bits = 0;
	u32 rate = 0;
	const u8* data = NULL;
	u32 dataLen = 0;

	int off = 12;
	while (off + 8 <= len) {
		const u8* ck = buf + off;
		u32 cklen = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((u32)ck[7] << 24);
		if (off + 8 + (int)cklen > len)
			break;
		if (!memcmp(ck, "fmt ", 4) && cklen >= 16) {
			const u8* b = ck + 8;
			fmt = b[0] | (b[1] << 8);
			chans_ = b[2] | (b[3] << 8);
			rate = b[4] | (b[5] << 8) | (b[6] << 16) | ((u32)b[7] << 24);
			bits = b[14] | (b[15] << 8);
		} else if (!memcmp(ck, "data", 4)) {
			data = ck + 8;
			dataLen = cklen;
		}
		off += 8 + cklen + (cklen & 1);
	}

	if (fmt != 1 || !data || !rate || (bits != 8 && bits != 16) ||
	    (chans_ != 1 && chans_ != 2))
		return false;

	u32 sampleBytes = (bits / 8) * chans_;
	u32 nframes = dataLen / sampleBytes;
	s16* frames = linearAlloc(nframes * sizeof(s16));
	if (!frames)
		return false;

	for (u32 i = 0; i < nframes; i++) {
		int acc = 0;
		for (int c = 0; c < chans_; c++) {
			if (bits == 16) {
				const u8* s = data + i * sampleBytes + c * 2;
				acc += (s16)(s[0] | (s[1] << 8));
			} else {
				acc += ((int)data[i * sampleBytes + c] - 128) << 8;
			}
		}
		frames[i] = acc / chans_;
	}

	freePatch(out);
	out->frames = frames;
	out->nframes = nframes;
	out->rate = rate;
	return true;
}

// --- arg parsing (';'-separated KEY=VAL and bare flags) ---

static const char* arg(const char* args, const char* key)
{
	static char val[64];
	size_t klen = strlen(key);
	const char* p = args;
	while (p) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			const char* v = p + klen + 1;
			const char* end = strchr(v, ';');
			size_t n = end ? (size_t)(end - v) : strlen(v);
			if (n >= sizeof(val))
				n = sizeof(val) - 1;
			memcpy(val, v, n);
			val[n] = 0;
			return val;
		}
		p = strchr(p, ';');
		if (p)
			p++;
	}
	return NULL;
}

static bool flag(const char* args, const char* name)
{
	size_t nlen = strlen(name);
	const char* p = args;
	while (p) {
		if (!strncmp(p, name, nlen) && (p[nlen] == ';' || p[nlen] == 0))
			return true;
		p = strchr(p, ';');
		if (p)
			p++;
	}
	return false;
}

// Duration grammar: ms by default, 'f' = frames, 'ms' explicit, 'p' = periods
static u32 parseDur(const char* s, float freq, u32 rate)
{
	if (!s)
		return 0;
	char* end;
	float v = strtof(s, &end);
	if (v <= 0)
		return 0;
	if (*end == 'f')
		return (u32)v;
	if (*end == 'p')
		return freq > 0 ? (u32)(v * rate / freq) : 0;
	return (u32)(v * rate / 1000.0f); // ms
}

static float dbToLinear(float db)
{
	return powf(10.0f, db / 20.0f);
}

static void applyMix(int ch)
{
	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = dbToLinear(chans[ch].dbL);
	mix[1] = dbToLinear(chans[ch].dbR);
	ndspChnSetMix(ch, mix);
}

// --- verbs ---

static void verbSynth(const char* args)
{
	int slot = -1;
	const char* v;
	if ((v = arg(args, "S")))
		slot = atoi(v);
	if (slot < 0 || slot >= N_PATCHES)
		return;

	const char* shape = arg(args, "W");
	float freq = (v = arg(args, "F")) ? strtof(v, NULL) : 0;
	u32 nframes = parseDur(arg(args, "T"), freq, SYNTH_RATE);
	if (nframes == 0)
		return;

	s16* frames = linearAlloc(nframes * sizeof(s16));
	if (!frames)
		return;

	bool silence = freq <= 0 || !shape || !strcmp(shape, "SILENCE");
	for (u32 i = 0; i < nframes; i++) {
		if (silence) {
			frames[i] = 0;
			continue;
		}
		float t = (float)i * freq / SYNTH_RATE;
		float ph = t - (int)t; // 0..1
		float s;
		if (!strncmp(shape, "SQ", 2))
			s = ph < 0.5f ? 1.0f : -1.0f;
		else if (!strcmp(shape, "SAW"))
			s = 2.0f * ph - 1.0f;
		else // SIN and SINE_* variants
			s = sinf(ph * 2.0f * (float)M_PI);
		frames[i] = (s16)(s * SYNTH_AMP);
	}

	freePatch(&patches[slot]);
	patches[slot].frames = frames;
	patches[slot].nframes = nframes;
	patches[slot].rate = SYNTH_RATE;
}

static void verbLoadBlob(const char* args, const char* b64, int b64len)
{
	int slot = -1;
	const char* v;
	if ((v = arg(args, "S")))
		slot = atoi(v);
	if (slot < 0 || slot >= N_PATCHES || b64len <= 0)
		return;

	int cap = (b64len / 4 + 1) * 3;
	u8* raw = malloc(cap);
	if (!raw)
		return;
	int n = b64decode(b64, b64len, raw, cap);
	wavDecode(raw, n, &patches[slot]);
	free(raw);
}

static void verbLoad(const char* args, const char* filename)
{
	int slot = -1;
	const char* v;
	if ((v = arg(args, "S")))
		slot = atoi(v);
	if (slot < 0 || slot >= N_PATCHES || !filename || !*filename)
		return;
	if (strchr(filename, '/') || strstr(filename, ".."))
		return;

	// JIT streamers cycle C;S -> Load -> Queue per chunk: serve from the
	// RAM cache when possible, skipping the SD read
	int ramLen;
	const u8* ram = apcCacheGet(filename, &ramLen);
	if (ram) {
		wavDecode(ram, ramLen, &patches[slot]);
		return;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s/%s", cacheDir, filename);
	FILE* f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 4 * 1024 * 1024) {
		fclose(f);
		return;
	}
	u8* buf = malloc(len);
	if (buf && fread(buf, 1, len, f) == (size_t)len)
		wavDecode(buf, len, &patches[slot]);
	free(buf);
	fclose(f);
}

static void verbCopy(const char* args)
{
	const char* v;
	int s = (v = arg(args, "S")) ? atoi(v) : -1;
	int d = (v = arg(args, "D")) ? atoi(v) : -1;
	if (s < 0 || s >= N_PATCHES || d < 0 || d >= N_PATCHES || s == d)
		return;
	Patch* src = &patches[s];
	if (!src->frames)
		return;
	s16* frames = linearAlloc(src->nframes * sizeof(s16));
	if (!frames)
		return;
	memcpy(frames, src->frames, src->nframes * sizeof(s16));
	freePatch(&patches[d]);
	patches[d].frames = frames;
	patches[d].nframes = src->nframes;
	patches[d].rate = src->rate;
}

static void verbQueue(const char* args)
{
	const char* v;
	int ch = (v = arg(args, "C")) ? atoi(v) : -1;
	int slot = (v = arg(args, "S")) ? atoi(v) : -1;
	if (ch < FIRST_BBS_CH || ch >= N_CHANNELS || slot < 0 || slot >= N_PATCHES)
		return;
	Patch* p = &patches[slot];
	if (!p->frames || !ok)
		return;

	Chan* c = &chans[ch];
	QNode* ent = malloc(sizeof(QNode));
	if (!ent)
		return;
	ent->next = NULL;

	// Per-buf volume: pre-scale samples (summed with base dB at the mixer)
	float bufDb = (v = arg(args, "V")) ? strtof(v, NULL) : 0;
	if (bufDb != 0) {
		float g = dbToLinear(bufDb);
		if (g > 1.0f) g = 1.0f;
		for (u32 i = 0; i < p->nframes; i++)
			p->frames[i] = (s16)(p->frames[i] * g);
	}

	// Fades: bake envelopes into the samples
	u32 fin = parseDur(arg(args, "I"), 0, p->rate);
	u32 fout = parseDur(arg(args, "O"), 0, p->rate);
	if (fin > p->nframes) fin = p->nframes;
	if (fout > p->nframes) fout = p->nframes;
	for (u32 i = 0; i < fin; i++)
		p->frames[i] = (s16)(p->frames[i] * (float)i / fin);
	for (u32 i = 0; i < fout; i++) {
		u32 idx = p->nframes - 1 - i;
		p->frames[idx] = (s16)(p->frames[idx] * (float)i / fout);
	}

	LightLock_Lock(&alock);

	// Free finished head nodes NOW (not just in the main-thread poll):
	// a stale DONE head makes an idle channel look busy and skips the
	// restart below — the source of intermittent dropped chunks
	while (c->head && c->head->wb.status == NDSP_WBUF_DONE) {
		QNode* n = c->head;
		c->head = n->next;
		linearFree(n->data);
		free(n);
	}

	// A queue onto a looping head ends the loop; ndsp can't break a loop
	// mid-buffer, so approximate with a cut
	for (QNode* n = c->head; n; n = n->next) {
		if (n->wb.looping && n->wb.status == NDSP_WBUF_PLAYING) {
			flushChannel(ch);
			break;
		}
	}

	bool wasIdle = !ndspChnIsPlaying(ch) && c->head == NULL;
	if (wasIdle) {
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetRate(ch, p->rate);
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
		applyMix(ch);
		c->rate = p->rate;
	} else if (c->rate && p->rate != c->rate) {
		// Busy channel, different declared rate: streamers use the WAV rate
		// as a drift-correction knob (see lameboy), so honor it by
		// resampling the chunk to the channel's playback rate
		u64 outN = (u64)p->nframes * c->rate / p->rate;
		s16* rs = outN ? linearAlloc(outN * sizeof(s16)) : NULL;
		if (rs) {
			for (u64 i = 0; i < outN; i++) {
				u64 pos = i * p->rate * 256 / c->rate;
				u32 idx = (u32)(pos >> 8);
				u32 frac = (u32)(pos & 255);
				if (idx >= p->nframes - 1) {
					rs[i] = p->frames[p->nframes - 1];
				} else {
					s32 a = p->frames[idx], b = p->frames[idx + 1];
					rs[i] = (s16)(a + (((b - a) * (s32)frac) >> 8));
				}
			}
			linearFree(p->frames);
			p->frames = rs;
			p->nframes = (u32)outN;
			p->rate = c->rate;
		}
	}

	ent->data = p->frames;
	memset(&ent->wb, 0, sizeof(ent->wb));
	ent->wb.data_vaddr = ent->data;
	ent->wb.nsamples = p->nframes;
	ent->wb.looping = flag(args, "L");
	DSP_FlushDataCache(ent->data, p->nframes * sizeof(s16));

	// Append to the channel FIFO
	if (!c->head) {
		c->head = ent;
	} else {
		QNode* n = c->head;
		while (n->next)
			n = n->next;
		n->next = ent;
	}
	ndspChnWaveBufAdd(ch, &ent->wb);
	LightLock_Unlock(&alock);

	// Queue moves the buffer out of the slot
	p->frames = NULL;
	p->nframes = 0;
	p->rate = 0;
}

static void verbFlush(const char* args)
{
	const char* v;
	int ch = (v = arg(args, "C")) ? atoi(v) : -1;
	if (ch < FIRST_BBS_CH || ch >= N_CHANNELS)
		return;
	LightLock_Lock(&alock);
	flushChannel(ch); // O= fade-out approximated as immediate stop
	LightLock_Unlock(&alock);
}

static void verbVolume(const char* args)
{
	const char* v;
	int ch = (v = arg(args, "C")) ? atoi(v) : -1;
	if (ch < FIRST_BBS_CH || ch >= N_CHANNELS)
		return;
	Chan* c = &chans[ch];
	LightLock_Lock(&alock);
	if ((v = arg(args, "V")))
		c->dbL = c->dbR = strtof(v, NULL);
	if ((v = arg(args, "VL")))
		c->dbL = strtof(v, NULL);
	if ((v = arg(args, "VR")))
		c->dbR = strtof(v, NULL);
	if (ok)
		applyMix(ch); // T= ramp approximated as instant
	LightLock_Unlock(&alock);
}

static void verbUpdate(const char* args)
{
	const char* v;
	int ch = (v = arg(args, "C")) ? atoi(v) : -1;
	if (ch < FIRST_BBS_CH || ch >= N_CHANNELS)
		return;
	Chan* c = &chans[ch];
	LightLock_Lock(&alock);
	while (c->head && c->head->wb.status == NDSP_WBUF_DONE) {
		QNode* n = c->head;
		c->head = n->next;
		linearFree(n->data);
		free(n);
	}
	// Arm ONLY. SyncTerm's do_update() is literally `apc_notify[ch] = true`
	// and fires solely on a running->stopped transition seen by its poll.
	// An earlier version here also fired immediately when the channel was
	// already idle; on a starved stream that idle state is common, so it
	// emitted spurious drains — and streaming doors re-anchor their
	// playback clock on each one, which made track position jump backwards.
	c->armed = true;
	LightLock_Unlock(&alock);
}

void audioHandleA(const char* cmd, int len)
{
	if (!ok)
		return;
	if (!strncmp(cmd, "Synth;", 6)) {
		verbSynth(cmd + 6);
	} else if (!strncmp(cmd, "LoadBlob;", 9)) {
		// last ';'-field is the blob
		const char* blob = strrchr(cmd + 9, ';');
		if (blob)
			verbLoadBlob(cmd + 9, blob + 1, len - (int)(blob + 1 - cmd));
	} else if (!strncmp(cmd, "Load;", 5)) {
		const char* fn = strrchr(cmd + 5, ';');
		if (fn)
			verbLoad(cmd + 5, fn + 1);
	} else if (!strncmp(cmd, "Copy;", 5)) {
		verbCopy(cmd + 5);
	} else if (!strncmp(cmd, "Queue;", 6)) {
		verbQueue(cmd + 6);
	} else if (!strncmp(cmd, "Flush;", 6)) {
		verbFlush(cmd + 6);
	} else if (!strncmp(cmd, "Volume;", 7)) {
		verbVolume(cmd + 7);
	} else if (!strncmp(cmd, "Update;", 7)) {
		verbUpdate(cmd + 7);
	}
	// Wait: intentionally unimplemented (would stall the UI thread)
}

void audioHandleQ(const char* cmd)
{
	char buf[32];
	if (!strcmp(cmd, "libsndfile")) {
		// We decode WAV/PCM natively; per-format truth is in the next query
		snprintf(buf, sizeof(buf), "\x1B[=7;100;%dn", ok ? 1 : 0);
		respond(buf);
	} else if (!strncmp(cmd, "libsndfileFormat;", 17)) {
		long pm = 0, ps = 0;
		if (sscanf(cmd + 17, "%li;%li", &pm, &ps) == 2) {
			// SF_FORMAT_WAV with PCM_16 or PCM_U8 subtypes only
			bool sup = ok && pm == 0x010000 && (ps == 0x0002 || ps == 0x0005);
			snprintf(buf, sizeof(buf), "\x1B[=7;101;%dn", sup ? 1 : 0);
			respond(buf);
		}
	}
	// Unknown feature queries are silently ignored
}

void audioDebugStats(u32* playingMask, u32* drainCount)
{
	u32 mask = 0;
	LightLock_Lock(&alock);
	if (ok) {
		for (int ch = 0; ch < N_CHANNELS; ch++) {
			if (ndspChnIsPlaying(ch))
				mask |= 1u << ch;
		}
	}
	*drainCount = drainNotifies;
	LightLock_Unlock(&alock);
	*playingMask = mask;
}

void audioStatusQuery(int channel)
{
	char buf[160];
	LightLock_Lock(&alock);
	if (channel < 0) {
		int off = snprintf(buf, sizeof(buf), "\x1B[=7");
		for (int ch = 0; ch < N_CHANNELS; ch++) {
			if (ok && ndspChnIsPlaying(ch))
				off += snprintf(buf + off, sizeof(buf) - off, ";%d;1", ch);
		}
		snprintf(buf + off, sizeof(buf) - off, "n");
	} else {
		int st = (ok && channel < N_CHANNELS && ndspChnIsPlaying(channel)) ? 1 : 0;
		snprintf(buf, sizeof(buf), "\x1B[=7;%d;%dn", channel, st);
	}
	LightLock_Unlock(&alock);
	respond(buf);
}
