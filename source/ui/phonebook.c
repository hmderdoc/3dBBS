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
	"Futureland|futureland.today|23|telnet||\n"
	"Vertrauen|vert.synchro.net|23|telnet||\n";

static PbEntry entries[PB_MAX];
static int count;
static int selected;

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

static bool ensureEntry(const char* name, const char* host, u16 port)
{
	for (int i = 0; i < count; i++) {
		if (!strcmp(entries[i].name, name)) {
			if (!strcmp(entries[i].host, host) && entries[i].port == port)
				return false;
			strcpy(entries[i].host, host);
			entries[i].port = port;
			return true;
		}
	}
	if (count < PB_MAX) {
		PbEntry* e = &entries[count++];
		memset(e, 0, sizeof(*e));
		snprintf(e->name, sizeof(e->name), "%s", name);
		snprintf(e->host, sizeof(e->host), "%s", host);
		e->port = port;
		e->proto = PROTO_TELNET;
		return true;
	}
	return false;
}

static void ensureLocalTest(void)
{
	bool changed = ensureEntry(LOCALTEST_NAME, LOCALTEST_HOST, LOCALTEST_PORT);
	changed |= ensureEntry(FLPROXY_NAME, DEV_HOST, FLPROXY_PORT);
	if (changed)
		rewriteFile();
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
		entries[0].port = 23;
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
	rewriteFile();
}

void pbToggleProto(int i)
{
	if (i < 0 || i >= count)
		return;
	PbEntry* e = &entries[i];
	if (e->proto == PROTO_TELNET) {
		e->proto = PROTO_RLOGIN;
		if (e->port == 23)
			e->port = 513;
	} else {
		e->proto = PROTO_TELNET;
		if (e->port == 513)
			e->port = 23;
	}
	rewriteFile();
}
