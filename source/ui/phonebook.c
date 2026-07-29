#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "phonebook.h"

#define PB_DIR  "sdmc:/3dBBS"
#define PB_PATH PB_DIR "/phonebook.txt"

// Showcase defaults. Every host:port here was verified answering with a
// live banner before inclusion (probed 2026-07-28); flags column "3d" marks
// boards that drive the 3DS: scene protocol.
static const char* defaultFile =
	"# 3dBBS dialing directory  (v2)\n"
	"# name|host|port|proto|user|pass|flags|size  (proto: telnet/rlogin/ssh)\n"
	"# flags: 3d = board drives 3dBBS stereoscopic scenes\n"
	"# size: COLSxROWS requested for this board; empty = 80x25\n"
	"# Credentials are stored in PLAIN TEXT - anyone with this SD card can\n"
	"# read them. Leave the password empty where that matters.\n"
	"# rlogin passes user/pass in its handshake (Synchronet autologin).\n"
	"Futureland|futureland.today|1513|rlogin|||3d\n"
	"Vertrauen|vert.synchro.net|23|telnet|||\n"
	"Elec.Chicken|bbs.electronicchicken.com|23|telnet|||\n"
	"Level 29|bbs.fozztexx.com|23|telnet|||\n"
	"20 For Beers|20forbeers.com|1337|telnet|||\n"
	"The Cave|cavebbs.homeip.net|23|telnet|||\n";

static PbEntry entries[PB_MAX];
static int count;
static int selected;

// SD writes stall the frame (~tens of ms), so edits only mark the book
// dirty; pbTick() flushes once the user stops tapping.
static bool pbDirty;
static int idleFrames;

static void markDirty(void)
{
	pbDirty = true;
	idleFrames = 0;   // restart the quiet period on every edit
}

// Case-insensitive substring test (newlib has no strcasestr)
static bool hostHas(const char* host, const char* needle)
{
	size_t nl = strlen(needle);
	for (const char* p = host; *p; p++) {
		if (!strncasecmp(p, needle, nl))
			return true;
	}
	return false;
}

// Dev-loop entries pointing at the development Mac (stress server and the
// Futureland logging proxy). The Mac's DHCP address drifts, so entries are
// created if missing and REWRITTEN if they don't match these constants.
#define DEV_HOST "192.168.1.61"
#define LOCALTEST_NAME "LocalTest"
#define LOCALTEST_HOST DEV_HOST
#define LOCALTEST_PORT 2323
#define FLPROXY_NAME "FL-Proxy"
#define FLPROXY_PORT 2324

static void rewriteFile(void)
{
	FILE* f = fopen(PB_PATH, "w");
	if (!f)
		return;
	fputs("# 3dBBS dialing directory  (v2)\n"
	      "# name|host|port|proto|user|pass|flags|size\n"
	      "# flags: 3d = 3D scenes.  size: COLSxROWS, empty = 80x25\n"
	      "# Credentials are stored in PLAIN TEXT on this card.\n", f);
	for (int i = 0; i < count; i++) {
		const PbEntry* e = &entries[i];
		char size[12] = "";
		if (e->cols && e->rows)
			snprintf(size, sizeof(size), "%ux%u", e->cols, e->rows);
		fprintf(f, "%s|%s|%u|%s|%s|%s|%s|%s\n", e->name, e->host, e->port,
		        e->proto == PROTO_RLOGIN ? "rlogin" :
		        e->proto == PROTO_SSH    ? "ssh"    : "telnet",
		        e->user, e->pass,
		        (e->flags & PB_FLAG_3D) ? "3d" : "", size);
	}
	fclose(f);
}

// Dev entries are kept pinned to their configured host/port/proto (unlike
// user entries, which are never touched after creation).
static bool ensureEntry(const char* name, const char* host, u16 port,
                        ConnProto proto)
{
	for (int i = 0; i < count; i++) {
		if (!strcmp(entries[i].name, name)) {
			if (!strcmp(entries[i].host, host) && entries[i].port == port &&
			    entries[i].proto == proto)
				return false;
			strcpy(entries[i].host, host);
			entries[i].port = port;
			entries[i].proto = proto;
			return true;
		}
	}
	if (count < PB_MAX) {
		PbEntry* e = &entries[count++];
		memset(e, 0, sizeof(*e));
		snprintf(e->name, sizeof(e->name), "%s", name);
		snprintf(e->host, sizeof(e->host), "%s", host);
		e->port = port;
		e->proto = proto;
		return true;
	}
	return false;
}

// One-time migration for phonebooks written before rlogin existed: a
// Futureland entry still on telnet:23 moves to rlogin on its real port.
// Guarded by a marker line so a later deliberate switch back sticks.
static bool migrated;

static void migrateDefaults(void)
{
	if (migrated)
		return;
	for (int i = 0; i < count; i++) {
		PbEntry* e = &entries[i];
		if (e->proto == PROTO_TELNET && e->port == 23 &&
		    hostHas(e->host, "futureland.today")) {
			e->proto = PROTO_RLOGIN;
			e->port = 1513;
		}
	}
	migrated = true;
}

// One-time v1 -> v2 upgrade for phonebooks that predate the flags column:
// tag Futureland as 3D-capable and add the verified showcase boards that
// aren't already present (matched by host, so user copies are respected).
// Runs only when the file lacks the "(v2)" header marker; after the next
// flush the marker is written and user edits (including removing boards)
// stick permanently.
static bool fileIsV2;

static void seedShowcase(void)
{
	static const struct { const char* name; const char* host; u16 port; } sc[] = {
		{ "Vertrauen",    "vert.synchro.net",          23 },
		{ "Elec.Chicken", "bbs.electronicchicken.com", 23 },
		{ "Level 29",     "bbs.fozztexx.com",          23 },
		{ "20 For Beers", "20forbeers.com",            1337 },
		{ "The Cave",     "cavebbs.homeip.net",        23 },
	};
	if (fileIsV2)
		return;
	for (int i = 0; i < count; i++) {
		if (hostHas(entries[i].host, "futureland.today"))
			entries[i].flags |= PB_FLAG_3D;
	}
	for (size_t s = 0; s < sizeof(sc) / sizeof(sc[0]); s++) {
		bool have = false;
		for (int i = 0; i < count && !have; i++)
			have = hostHas(entries[i].host, sc[s].host);
		if (!have && count < PB_MAX) {
			PbEntry* e = &entries[count++];
			memset(e, 0, sizeof(*e));
			snprintf(e->name, sizeof(e->name), "%s", sc[s].name);
			snprintf(e->host, sizeof(e->host), "%s", sc[s].host);
			e->port = sc[s].port;
			e->proto = PROTO_TELNET;
		}
	}
	fileIsV2 = true;
	markDirty();
}

static void ensureLocalTest(void)
{
#ifdef RELEASE_BUILD
	// Release builds don't create the dev-machine entries — but still run
	// the migrations for phonebooks written by dev builds.
	migrateDefaults();
	markDirty();
	return;
#endif
	bool changed = ensureEntry(LOCALTEST_NAME, LOCALTEST_HOST,
	                           LOCALTEST_PORT, PROTO_TELNET);
	// FL-Proxy relays to Futureland's rlogin port, so the client must speak
	// rlogin for the handshake (and autologin) to pass through
	changed |= ensureEntry(FLPROXY_NAME, DEV_HOST, FLPROXY_PORT,
	                       PROTO_RLOGIN);
	migrateDefaults();
	markDirty(); // always: persists the migration marker and any change
	(void)changed;
}

void pbTick(void)
{
	if (!pbDirty) {
		idleFrames = 0;
		return;
	}
	// ~0.5s of quiet before touching the card, so a burst of taps costs
	// exactly one write and never hitches the tap itself
	if (++idleFrames < 30)
		return;
	rewriteFile();
	pbDirty = false;
	idleFrames = 0;
}

void pbFlush(void)
{
	if (pbDirty) {
		rewriteFile();
		pbDirty = false;
	}
}

// Split "a|b|c" in place; returns field count, fields[] point into line
static int splitFields(char* line, char** fields, int maxFields)
{
	int n = 0;
	fields[n++] = line;
	for (char* p = line; *p && n < maxFields; p++) {
		if (*p == '|') {
			*p = 0;
			fields[n++] = p + 1;
		}
	}
	return n;
}

// Clamp a requested geometry into the supported range. 0x0 in means "the
// default", and stays 0x0 so the entry keeps tracking the default rather
// than freezing today's value.
static void pbClampSize(const unsigned* inC, const unsigned* inR,
                        u16* outC, u16* outR)
{
	unsigned c = *inC, r = *inR;
	if (!c || !r) {
		*outC = 0;
		*outR = 0;
		return;
	}
	if (c < PB_MIN_COLS) c = PB_MIN_COLS;
	if (c > PB_MAX_COLS) c = PB_MAX_COLS;
	if (r < PB_MIN_ROWS) r = PB_MIN_ROWS;
	if (r > PB_MAX_ROWS) r = PB_MAX_ROWS;
	*outC = (u16)c;
	*outR = (u16)r;
}

static void parseLine(char* line)
{
	if (count >= PB_MAX || line[0] == '#' || !strchr(line, '|'))
		return;

	char* f[8] = { NULL };
	int n = splitFields(line, f, 8);
	if (n < 2 || !*f[0] || !*f[1])
		return;

	PbEntry* e = &entries[count];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", f[0]);
	snprintf(e->host, sizeof(e->host), "%s", f[1]);

	int port = (n > 2 && *f[2]) ? atoi(f[2]) : 23;
	e->proto = PROTO_TELNET;
	if (n > 3) {
		if (!strcasecmp(f[3], "rlogin"))
			e->proto = PROTO_RLOGIN;
		else if (!strcasecmp(f[3], "ssh"))
			e->proto = PROTO_SSH;
	}
	if (port <= 0 || port > 65535)
		port = e->proto == PROTO_RLOGIN ? 513 :
		       e->proto == PROTO_SSH    ? 22  : 23;
	e->port = (u16)port;

	if (n > 4)
		snprintf(e->user, sizeof(e->user), "%s", f[4]);
	if (n > 5)
		snprintf(e->pass, sizeof(e->pass), "%s", f[5]);
	if (n > 6 && hostHas(f[6], "3d"))
		e->flags |= PB_FLAG_3D;
	if (n > 7 && *f[7]) {
		unsigned c = 0, r = 0;
		if (sscanf(f[7], "%ux%u", &c, &r) == 2)
			pbClampSize(&c, &r, &e->cols, &e->rows);
	}
	count++;
}

void pbLoad(void)
{
	count = 0;
	selected = 0;

	FILE* f = fopen(PB_PATH, "r");
	if (!f) {
		mkdir(PB_DIR, 0777);
		f = fopen(PB_PATH, "w");
		if (f) {
			fputs(defaultFile, f);
			fclose(f);
		}
		f = fopen(PB_PATH, "r");
	}

	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[0] == '#' && strstr(line, "(v2)"))
				fileIsV2 = true;
			parseLine(line);
		}
		fclose(f);
	}

	if (count == 0) {
		// SD unwritable or file empty: bake in the defaults
		memset(entries, 0, sizeof(entries));
		strcpy(entries[0].name, "Futureland");
		strcpy(entries[0].host, "futureland.today");
		entries[0].port = 1513;
		entries[0].proto = PROTO_RLOGIN;
		strcpy(entries[1].name, "Vertrauen");
		strcpy(entries[1].host, "vert.synchro.net");
		entries[1].port = 23;
		count = 2;
	}

	seedShowcase();
	ensureLocalTest();
}

int pbCount(void) { return count; }

const PbEntry* pbGet(int i)
{
	if (i < 0 || i >= count)
		i = 0;
	return &entries[i];
}

int pbSelected(void) { return selected; }

void pbSelectNext(void) { selected = (selected + 1) % count; }
void pbSelectPrev(void) { selected = (selected + count - 1) % count; }

void pbSetCreds(int i, const char* user, const char* pass)
{
	if (i < 0 || i >= count)
		return;
	if (user)
		snprintf(entries[i].user, sizeof(entries[i].user), "%s", user);
	if (pass)
		snprintf(entries[i].pass, sizeof(entries[i].pass), "%s", pass);
	markDirty();
}

void pbSetEntry(int i, const char* name, const char* host, u16 port)
{
	if (i < 0 || i >= count)
		return;
	if (name && *name)
		snprintf(entries[i].name, sizeof(entries[i].name), "%s", name);
	if (host && *host)
		snprintf(entries[i].host, sizeof(entries[i].host), "%s", host);
	if (port)
		entries[i].port = port;
	markDirty();
}

int pbAdd(const char* name, const char* host, u16 port)
{
	if (count >= PB_MAX)
		return -1;
	PbEntry* e = &entries[count];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", name && *name ? name : "New BBS");
	snprintf(e->host, sizeof(e->host), "%s", host ? host : "");
	e->port = port ? port : 23;
	e->proto = PROTO_TELNET;
	count++;
	markDirty();
	return count - 1;
}

void pbDelete(int i)
{
	if (i < 0 || i >= count || count <= 1)
		return;
	memmove(&entries[i], &entries[i + 1], sizeof(PbEntry) * (count - i - 1));
	count--;
	if (selected >= count)
		selected = count - 1;
	markDirty();
}

void pbSelect(int i)
{
	if (i >= 0 && i < count)
		selected = i;
}

// SyncTERM's screen modes (syncterm.net), which is what BBS authors design
// their screens against. 132x60 is also the largest grid CTerm documents,
// so the list needs no ceiling of its own. Index 0 is the default; PROTO
// and SIZE both cycle, so the entry's own value is found in the list first
// and a custom size simply steps to the following preset.
static const struct { u16 c, r; } sizePresets[] = {
	{  80, 25 }, {  80, 28 }, {  80, 30 }, {  80, 43 }, {  80, 50 },
	{  80, 60 },
	{ 132, 25 }, { 132, 28 }, { 132, 30 }, { 132, 34 }, { 132, 43 },
	{ 132, 50 }, { 132, 60 },
};
#define NSIZES (int)(sizeof(sizePresets) / sizeof(sizePresets[0]))

void pbSizeOf(int i, u16* cols, u16* rows)
{
	const PbEntry* e = pbGet(i);
	*cols = e->cols ? e->cols : PB_DEF_COLS;
	*rows = e->rows ? e->rows : PB_DEF_ROWS;
}

void pbSetSize(int i, u16 cols, u16 rows)
{
	if (i < 0 || i >= count)
		return;
	// Asking for the default stores 0x0 rather than pinning today's
	// numbers, so the entry keeps tracking the default and the file stays
	// free of redundant columns.
	if (cols == PB_DEF_COLS && rows == PB_DEF_ROWS)
		cols = rows = 0;
	unsigned c = cols, r = rows;
	pbClampSize(&c, &r, &entries[i].cols, &entries[i].rows);
	markDirty();
}

int pbSizePresetCount(void) { return NSIZES; }

void pbSizePreset(int k, u16* cols, u16* rows)
{
	if (k < 0 || k >= NSIZES)
		k = 0;
	*cols = sizePresets[k].c;
	*rows = sizePresets[k].r;
}

// Some boards don't run rlogin on the standard port; keep a small table of
// known exceptions so the toggle lands somewhere that actually answers.
static u16 rloginPortFor(const char* host)
{
	if (hostHas(host, "futureland.today"))
		return 1513;   // verified: 1513 open, 513 closed
	return 513;
}

static u16 sshPortFor(const char* host)
{
	if (hostHas(host, "futureland.today"))
		return 4022;   // per the sysop; standard 22 is not it
	return 22;
}

// Cycle telnet -> rlogin -> ssh -> telnet. The port follows only when it
// still sits on the previous protocol's default, so custom ports stick.
void pbToggleProto(int i)
{
	if (i < 0 || i >= count)
		return;
	PbEntry* e = &entries[i];
	switch (e->proto) {
	case PROTO_TELNET:
		e->proto = PROTO_RLOGIN;
		if (e->port == 23)
			e->port = rloginPortFor(e->host);
		break;
	case PROTO_RLOGIN:
		e->proto = PROTO_SSH;
		if (e->port == 513 || e->port == rloginPortFor(e->host))
			e->port = sshPortFor(e->host);
		break;
	default:
		e->proto = PROTO_TELNET;
		if (e->port == 22 || e->port == sshPortFor(e->host))
			e->port = 23;
		break;
	}
	markDirty();
}
