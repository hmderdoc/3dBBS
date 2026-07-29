#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "apc.h"
#include "b64.h"
#include "md5.h"
#include "../audio/apcaudio.h"
#include "../gfx/scene3d.h"
#include "../gfx/sixel.h"
#include "../gfx/siximg.h"

#define CACHE_ROOT "sdmc:/3dBBS/cache"

static ApcRespondFn respondFn;
static char host[64] = "none";
static char cacheDir[128] = CACHE_ROOT "/none";

// APC worker thread state (logic below at "--- worker thread ---")
typedef struct Job {
	struct Job* next;
	int len;          // -1 = reset sentinel
	s8 kind;          // 0 = APC command, 1 = DCS sixel
	u8 cellX, cellY;  // cursor anchor for sixel
	u8 data[];
} Job;

static Job* jobHead;
static Job* jobTail;
static LightLock qLock;
static LightEvent qEvent;
static Thread workerThread;
static volatile bool workerRun;
static void workerMain(void* arg);
static void enqueue(Job* j);

// SD flush thread state (logic below at the FlushJob section)
static LightLock flLock;
static LightEvent flEvent;
static Thread sdThread;
static void sdThreadMain(void* arg);

void apcInit(ApcRespondFn respond)
{
	respondFn = respond;
	mkdir("sdmc:/3dBBS", 0777);
	mkdir(CACHE_ROOT, 0777);

	LightLock_Init(&qLock);
	LightEvent_Init(&qEvent, RESET_ONESHOT);
	LightLock_Init(&flLock);
	LightEvent_Init(&flEvent, RESET_ONESHOT);
	workerRun = true;
	sdThread = threadCreate(sdThreadMain, NULL, 32 * 1024, 0x3F, -2, false);
	// Core 1 (system core, freed by APT_SetAppCpuTimeLimit in main) gives
	// true parallelism on old3DS; fall back to a low-priority thread on the
	// app core, which then runs in the main loop's VBlank idle time
	workerThread = threadCreate(workerMain, NULL, 64 * 1024, 0x3F, 1, false);
	if (!workerThread)
		workerThread = threadCreate(workerMain, NULL, 64 * 1024, 0x3F, -2, false);
}

void apcExit(void)
{
	// Bounded joins: a worker stuck in SD I/O must not wedge the whole
	// shutdown (an infinite join here froze the return to hbmenu).
	const u64 joinTimeout = 3000000000ULL;   // 3s in ns

	workerRun = false;
	LightEvent_Signal(&qEvent);
	LightEvent_Signal(&flEvent);

	if (workerThread) {
		if (R_SUCCEEDED(threadJoin(workerThread, joinTimeout)))
			threadFree(workerThread);
		workerThread = NULL;
	}
	if (sdThread) {
		if (R_SUCCEEDED(threadJoin(sdThread, joinTimeout)))
			threadFree(sdThread);
		sdThread = NULL;
	}
}

static void respond(const char* s)
{
	if (respondFn)
		respondFn((const u8*)s, strlen(s));
}

static bool safeName(const char* s)
{
	if (!*s || strlen(s) > 48)
		return false;
	for (; *s; s++) {
		char c = *s;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return false;
	}
	return true;
}

void apcSetHost(const char* h)
{
	size_t i;
	for (i = 0; i + 1 < sizeof(host) && h[i]; i++) {
		char c = h[i];
		host[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		           (c >= '0' && c <= '9') || c == '.' || c == '-') ? c : '_';
	}
	host[i] = 0;
	snprintf(cacheDir, sizeof(cacheDir), CACHE_ROOT "/%s", host);
	mkdir(cacheDir, 0777);
	audioSetCacheDir(cacheDir);
}

void apcReset(void)
{
	// Drop pending jobs, then reset audio in-order on the worker
	LightLock_Lock(&qLock);
	while (jobHead) {
		Job* j = jobHead;
		jobHead = j->next;
		free(j);
	}
	jobTail = NULL;
	LightLock_Unlock(&qLock);

	Job* j = malloc(sizeof(Job));
	if (j) {
		j->len = -1;
		enqueue(j);
	}
}

// --- SyncTERM:C; cache verbs ---

// RAM-first cache for recent C;S stores. JIT audio streamers cycle
// store->Load->Queue per chunk; serving Load from RAM removes the SD
// round-trip from the hot path. SD stays write-through for C;L/dedup.
// Worker-thread only — no locking needed.
#define RAMCACHE_SLOTS 16
typedef struct {
	char name[64];
	u8* data;
	int len;
	u32 stamp;
	bool dirty;   // not yet flushed to SD
} RamEntry;
static RamEntry ramCache[RAMCACHE_SLOTS];
static u32 ramStamp;

// SD writes live on their OWN lowest-priority thread. Telemetry proved —
// twice — that any SD write sharing the APC worker (sync, on-eviction, or
// "when idle") stalls the job queue into multi-second audio latency. The
// worker's only SD-related cost is now a memcpy into this flush queue.
typedef struct FlushJob {
	struct FlushJob* next;
	char path[256];
	u8* data;
	int len;
} FlushJob;

static FlushJob* flHead;
static FlushJob* flTail;
static volatile int flPending;

static void sdThreadDrain(void)
{
	for (;;) {
		LightLock_Lock(&flLock);
		FlushJob* j = flHead;
		if (j) {
			flHead = j->next;
			if (!flHead)
				flTail = NULL;
		}
		LightLock_Unlock(&flLock);
		if (!j)
			break;
		FILE* f = fopen(j->path, "wb");
		if (f) {
			fwrite(j->data, 1, j->len, f);
			fclose(f);
		}
		free(j->data);
		free(j);
		LightLock_Lock(&flLock);
		flPending--;
		LightLock_Unlock(&flLock);
	}
}

static void sdThreadMain(void* arg)
{
	(void)arg;
	while (workerRun) {
		LightEvent_Wait(&flEvent);
		sdThreadDrain();
	}
	sdThreadDrain(); // persist whatever's left on shutdown
}

// Hand every dirty entry to the SD thread (data copied; entry marked clean)
static void scheduleDirtyFlushes(void)
{
	for (int i = 0; i < RAMCACHE_SLOTS; i++) {
		RamEntry* e = &ramCache[i];
		if (!e->data || !e->dirty)
			continue;
		FlushJob* j = malloc(sizeof(FlushJob));
		u8* copy = j ? malloc(e->len) : NULL;
		if (!copy) {
			free(j);
			return; // retry on a later idle pass
		}
		memcpy(copy, e->data, e->len);
		snprintf(j->path, sizeof(j->path), "%s/%s", cacheDir, e->name);
		j->data = copy;
		j->len = e->len;
		j->next = NULL;
		e->dirty = false;
		LightLock_Lock(&flLock);
		if (flTail)
			flTail->next = j;
		else
			flHead = j;
		flTail = j;
		flPending++;
		LightLock_Unlock(&flLock);
	}
	LightEvent_Signal(&flEvent);
}


static void ramCachePut(const char* name, u8* data /*owned*/, int len)
{
	RamEntry* victim = &ramCache[0];
	for (int i = 0; i < RAMCACHE_SLOTS; i++) {
		if (ramCache[i].data && !strcmp(ramCache[i].name, name)) {
			victim = &ramCache[i];
			break;
		}
		if (ramCache[i].stamp < victim->stamp)
			victim = &ramCache[i];
	}
	// NEVER write SD here: this runs between streamed chunks, and a
	// synchronous write stalls the worker into audible dropouts (measured:
	// fl_records backlog sawtooth). A dropped dirty evictee just means the
	// BBS's C;L dedup misses once and re-uploads — the protocol's designed
	// recovery path.
	free(victim->data);
	snprintf(victim->name, sizeof(victim->name), "%s", name);
	victim->data = data;
	victim->len = len;
	victim->stamp = ++ramStamp;
	victim->dirty = true;
}

const u8* apcCacheGet(const char* name, int* len)
{
	for (int i = 0; i < RAMCACHE_SLOTS; i++) {
		if (ramCache[i].data && !strcmp(ramCache[i].name, name)) {
			ramCache[i].stamp = ++ramStamp;
			*len = ramCache[i].len;
			return ramCache[i].data;
		}
	}
	return NULL;
}

static void cacheStore(const char* nameStart, int remLen)
{
	// C;S;<name>;<b64>
	const char* sep = memchr(nameStart, ';', remLen);
	if (!sep)
		return;
	char name[64];
	int nlen = sep - nameStart;
	if (nlen <= 0 || nlen >= (int)sizeof(name))
		return;
	memcpy(name, nameStart, nlen);
	name[nlen] = 0;
	if (!safeName(name))
		return;

	const char* b64 = sep + 1;
	int b64len = remLen - nlen - 1;
	int cap = (b64len / 4 + 1) * 3;
	u8* raw = malloc(cap);
	if (!raw)
		return;
	int n = b64decode(b64, b64len, raw, cap);

	// RAM only; SD write happens lazily when the worker goes idle
	ramCachePut(name, raw, n); // takes ownership of raw
}

// Minimal glob: '*' matches any run, '?' matches one char
static bool globMatch(const char* pat, const char* s)
{
	if (*pat == 0)
		return *s == 0;
	if (*pat == '*')
		return globMatch(pat + 1, s) || (*s && globMatch(pat, s + 1));
	if (*s && (*pat == '?' || *pat == *s))
		return globMatch(pat + 1, s + 1);
	return false;
}

static bool fileMd5Hex(const char* path, char out[33])
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Md5Ctx ctx;
	md5Init(&ctx);
	static u8 buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		md5Update(&ctx, buf, n);
	fclose(f);
	u8 digest[16];
	md5Final(&ctx, digest);
	for (int i = 0; i < 16; i++)
		sprintf(out + i * 2, "%02x", digest[i]);
	return true;
}

static void bufMd5Hex(const u8* data, int len, char out[33])
{
	Md5Ctx ctx;
	u8 digest[16];
	md5Init(&ctx);
	md5Update(&ctx, data, len);
	md5Final(&ctx, digest);
	for (int i = 0; i < 16; i++)
		sprintf(out + i * 2, "%02x", digest[i]);
}

static void cacheList(const char* glob)
{
	// Answered by merging RAM entries (authoritative, hashed in memory)
	// with prior-session SD files — no SD writes during a session, ever
	if (!glob || !*glob)
		glob = "*";

	// Reply is one APC string: "SyncTERM:C;L\n" + "<fn>\t<md5>\n" per match
	static char reply[4096];
	int off = snprintf(reply, sizeof(reply), "\x1B_SyncTERM:C;L\n");

	for (int i = 0; i < RAMCACHE_SLOTS; i++) {
		RamEntry* e = &ramCache[i];
		if (!e->data || !globMatch(glob, e->name))
			continue;
		char md5hex[33];
		bufMd5Hex(e->data, e->len, md5hex);
		if (off + (int)strlen(e->name) + 40 > (int)sizeof(reply) - 4)
			break;
		off += snprintf(reply + off, sizeof(reply) - off, "%s\t%s\n",
		                e->name, md5hex);
	}

	DIR* d = opendir(cacheDir);
	if (d) {
		struct dirent* e;
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.' || !globMatch(glob, e->d_name))
				continue;
			int shadowLen;
			if (apcCacheGet(e->d_name, &shadowLen))
				continue; // RAM entry above is the current version
			char path[384], md5hex[33];
			snprintf(path, sizeof(path), "%s/%s", cacheDir, e->d_name);
			if (!fileMd5Hex(path, md5hex))
				continue;
			if (off + (int)strlen(e->d_name) + 40 > (int)sizeof(reply) - 4)
				break;
			off += snprintf(reply + off, sizeof(reply) - off, "%s\t%s\n",
			                e->d_name, md5hex);
		}
		closedir(d);
	}
	snprintf(reply + off, sizeof(reply) - off, "\x1B\\");
	respond(reply);
}

// --- worker thread ---
// APC handlers do SD I/O and large decodes; on the main thread those stall
// frames for 10-100s of ms (dead keys, dropped drain cadence). All APC jobs
// execute in FIFO order on a worker thread — on core 1 when available.

static void apcExecute(const u8* data, int len);

static u32 jobBytes; // queued payload total, guarded by qLock
#define JOB_BYTES_CAP (4 * 1024 * 1024)
static volatile u32 sixelSeen, sixelDecodeFail;

void apcSixelStats(u32* seen, u32* decodeFail)
{
	*seen = sixelSeen;
	*decodeFail = sixelDecodeFail;
}

static void workerMain(void* arg)
{
	(void)arg;
	while (workerRun) {
		LightEvent_Wait(&qEvent);
		for (;;) {
			LightLock_Lock(&qLock);
			Job* j = jobHead;
			if (j) {
				jobHead = j->next;
				if (!jobHead)
					jobTail = NULL;
				if (j->len > 0)
					jobBytes -= j->len;
			}
			LightLock_Unlock(&qLock);
			if (!j)
				break;
			if (j->len < 0) {
				// Disconnect: reset audio + scene, then persist surviving
				// cache entries — the ONLY time session data heads to SD
				audioReset();
				scene3dSceneClear();
				scheduleDirtyFlushes();
			} else if (j->kind == 1) {
				u32* rgba;
				int w, h;
				sixelSeen++;
				if (sixelDecode(j->data, j->len, &rgba, &w, &h))
					siximgSubmit(rgba, w, h, j->cellX, j->cellY);
				else
					sixelDecodeFail++;
			} else {
				apcExecute(j->data, j->len);
			}
			free(j);
		}
	}
	scheduleDirtyFlushes(); // app exit: persist before the SD thread drains
}

static void enqueue(Job* j)
{
	j->next = NULL;
	LightLock_Lock(&qLock);
	if (jobTail)
		jobTail->next = j;
	else
		jobHead = j;
	jobTail = j;
	LightLock_Unlock(&qLock);
	LightEvent_Signal(&qEvent);
}

void apcDebugStats(u32* jobs, u32* bytes)
{
	u32 n = 0;
	LightLock_Lock(&qLock);
	for (Job* j = jobHead; j; j = j->next)
		n++;
	*bytes = jobBytes;
	LightLock_Unlock(&qLock);
	*jobs = n;
}

static bool isSixel(const u8* data, int len)
{
	// DCS payload: optional numeric params, then 'q'
	for (int i = 0; i < len && i < 32; i++) {
		if (data[i] == 'q')
			return true;
		if (!((data[i] >= '0' && data[i] <= '9') || data[i] == ';'))
			return false;
	}
	return false;
}

void apcHandle(AnsiStrKind kind, const u8* data, int len, int cellX, int cellY)
{
	s8 jkind;
	if (kind == ANSI_STR_APC)
		jkind = 0;
	else if (kind == ANSI_STR_DCS && isSixel(data, len))
		jkind = 1;
	else
		return; // OSC handled in main; DCS fonts later
	if (len > ANSI_STR_MAX)
		return;
	// Backstop against unbounded backlog if the worker ever falls behind;
	// with lazy SD writes it should never trigger in normal streaming
	LightLock_Lock(&qLock);
	bool full = jobBytes > JOB_BYTES_CAP;
	if (!full)
		jobBytes += len;
	LightLock_Unlock(&qLock);
	if (full)
		return;
	Job* j = malloc(sizeof(Job) + len);
	if (!j)
		return;
	j->len = len;
	j->kind = jkind;
	j->cellX = (u8)(cellX < 0 ? 0 : cellX > 255 ? 255 : cellX);
	j->cellY = (u8)(cellY < 0 ? 0 : cellY > 255 ? 255 : cellY);
	memcpy(j->data, data, len);
	enqueue(j);
}

// --- 3DS: scene protocol (worker thread) ---

// arg("P", "Obj;Add=1;P=0,1,2") -> "0,1,2" (static buffer)
static const char* apcArg(const char* args, const char* key)
{
	static char val[96];
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

static void parseVec3(const char* s, float out[3], float def)
{
	out[0] = out[1] = out[2] = def;
	if (!s)
		return;
	sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]);
}

static void handle3DS(const char* cmd)
{
	const char* v;
	if (!strcmp(cmd, "Query")) {
		char buf[48];
		snprintf(buf, sizeof(buf), "\x1B_3DS:Ver;%d;%d\x1B\\",
		         APC_3DS_MAJOR, APC_3DS_MINOR);
		respond(buf);
	} else if (!strncmp(cmd, "Mesh;Load;", 10)) {
		// Mesh;Load;S=<slot>;<name>  — 3DM1 blob from the C;S cache
		const char* args = cmd + 10;
		int slot = (v = apcArg(args, "S")) ? atoi(v) : -1;
		const char* name = strrchr(args, ';');
		if (slot < 0 || !name || !safeName(++name))
			return;
		int len;
		const u8* data = apcCacheGet(name, &len);
		u8* fileBuf = NULL;
		if (!data) {
			char path[384];
			snprintf(path, sizeof(path), "%s/%s", cacheDir, name);
			FILE* f = fopen(path, "rb");
			if (!f)
				return;
			fseek(f, 0, SEEK_END);
			long flen = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (flen > 0 && flen < 2 * 1024 * 1024) {
				fileBuf = malloc(flen);
				if (fileBuf && fread(fileBuf, 1, flen, f) == (size_t)flen) {
					data = fileBuf;
					len = flen;
				}
			}
			fclose(f);
		}
		if (data)
			scene3dMeshLoad(slot, data, len);
		free(fileBuf);
	} else if (!strncmp(cmd, "Obj;Add=", 8)) {
		// Obj;Add=<id>;M=<slot>;P=x,y,z;R=x,y,z;S=scale;Spin=x,y,z
		const char* args = cmd + 4;
		int id = atoi(cmd + 8);
		int slot = (v = apcArg(args, "M")) ? atoi(v) : 0;
		float pos[3], rot[3], spin[3];
		parseVec3(apcArg(args, "P"), pos, 0.0f);
		parseVec3(apcArg(args, "R"), rot, 0.0f);
		parseVec3(apcArg(args, "Spin"), spin, 0.0f);
		float scale = (v = apcArg(args, "S")) ? strtof(v, NULL) : 1.0f;
		scene3dInstSet(id, slot, pos, rot, scale, spin);
	} else if (!strncmp(cmd, "Obj;Del=", 8)) {
		scene3dInstRemove(atoi(cmd + 8));
	} else if (!strncmp(cmd, "Cam;", 4)) {
		// Cam;P=x,y,z;L=x,y,z;Fov=deg
		const char* args = cmd + 4;
		float pos[3], look[3];
		parseVec3(apcArg(args, "P"), pos, 0.0f);
		if (!apcArg(args, "P")) { pos[2] = 4.0f; }
		parseVec3(apcArg(args, "L"), look, 0.0f);
		float fov = (v = apcArg(args, "Fov")) ? strtof(v, NULL) : 40.0f;
		scene3dCamSet(pos, look, fov);
	} else if (!strcmp(cmd, "Scene;Clear")) {
		scene3dSceneClear();
	}
}

// --- dispatch (worker thread) ---

static void apcExecute(const u8* data, int len)
{
	// NUL-terminated working copy for string ops (worker-only buffer)
	static char cmd[ANSI_STR_MAX + 1];
	memcpy(cmd, data, len);
	cmd[len] = 0;

	if (!strncmp(cmd, "SyncTERM:", 9)) {
		char* rest = cmd + 9;
		int rlen = len - 9;
		if (!strncmp(rest, "C;S;", 4))
			cacheStore(rest + 4, rlen - 4);
		else if (!strncmp(rest, "C;L", 3))
			cacheList(rest[3] == ';' ? rest + 4 : NULL);
		else if (!strncmp(rest, "A;", 2))
			audioHandleA(rest + 2, rlen - 2);
		else if (!strncmp(rest, "Q;", 2))
			audioHandleQ(rest + 2);
		else if (!strcmp(rest, "VER"))
			respond("\x1B_SyncTERM:VER;" APC_VER_STRING "\x1B\\");
	} else if (!strncmp(cmd, "3DS:", 4)) {
		handle3DS(cmd + 4);
	}
}
