#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "phonebook.h"

#define PB_DIR  "sdmc:/3dBBS"
#define PB_PATH PB_DIR "/phonebook.txt"

static const char* defaultFile =
	"# 3dBBS dialing directory\n"
	"# name|host|port|proto|user|pass   (proto: telnet or rlogin)\n"
	"# Credentials are stored in PLAIN TEXT - anyone with this SD card can\n"
	"# read them. Leave the password empty where that matters.\n"
	"# rlogin passes user/pass in its handshake (Synchronet autologin).\n"
	"Futureland|futureland.today|1513|rlogin||\n"
	"Vertrauen|vert.synchro.net|23|telnet||\n";

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
	fputs("# 3dBBS dialing directory\n"
	      "# name|host|port|proto|user|pass   (proto: telnet or rlogin)\n"
	      "# Credentials are stored in PLAIN TEXT on this card.\n", f);
	for (int i = 0; i < count; i++) {
		const PbEntry* e = &entries[i];
		fprintf(f, "%s|%s|%u|%s|%s|%s\n", e->name, e->host, e->port,
		        e->proto == PROTO_RLOGIN ? "rlogin" : "telnet",
		        e->user, e->pass);
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

static void ensureLocalTest(void)
{
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

static void parseLine(char* line)
{
	if (count >= PB_MAX || line[0] == '#' || !strchr(line, '|'))
		return;

	char* f[6] = { NULL };
	int n = splitFields(line, f, 6);
	if (n < 2 || !*f[0] || !*f[1])
		return;

	PbEntry* e = &entries[count];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", f[0]);
	snprintf(e->host, sizeof(e->host), "%s", f[1]);

	int port = (n > 2 && *f[2]) ? atoi(f[2]) : 23;
	e->proto = (n > 3 && !strcasecmp(f[3], "rlogin")) ? PROTO_RLOGIN
	                                                  : PROTO_TELNET;
	if (port <= 0 || port > 65535)
		port = e->proto == PROTO_RLOGIN ? 513 : 23;
	e->port = (u16)port;

	if (n > 4)
		snprintf(e->user, sizeof(e->user), "%s", f[4]);
	if (n > 5)
		snprintf(e->pass, sizeof(e->pass), "%s", f[5]);
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

// Some boards don't run rlogin on the standard port; keep a small table of
// known exceptions so the toggle lands somewhere that actually answers.
static u16 rloginPortFor(const char* host)
{
	if (hostHas(host, "futureland.today"))
		return 1513;   // verified: 1513 open, 513 closed
	return 513;
}

void pbToggleProto(int i)
{
	if (i < 0 || i >= count)
		return;
	PbEntry* e = &entries[i];
	if (e->proto == PROTO_TELNET) {
		e->proto = PROTO_RLOGIN;
		if (e->port == 23)
			e->port = rloginPortFor(e->host);
	} else {
		e->proto = PROTO_TELNET;
		if (e->port == 513 || e->port == rloginPortFor(e->host))
			e->port = 23;
	}
	markDirty();
}
