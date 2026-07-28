#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "phonebook.h"

#define PB_DIR  "sdmc:/3dBBS"
#define PB_PATH PB_DIR "/phonebook.txt"

static const char* defaultFile =
	"# 3dBBS dialing directory: name|host|port\n"
	"Futureland|futureland.today|23\n"
	"Vertrauen|vert.synchro.net|23\n";


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
	fputs("# 3dBBS dialing directory: name|host|port\n", f);
	for (int i = 0; i < count; i++)
		fprintf(f, "%s|%s|%u\n", entries[i].name, entries[i].host,
		        entries[i].port);
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
		snprintf(e->name, sizeof(e->name), "%s", name);
		snprintf(e->host, sizeof(e->host), "%s", host);
		e->port = port;
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

static void parseLine(char* line)
{
	if (count >= PB_MAX)
		return;
	char* h = strchr(line, '|');
	if (!h || line[0] == '#')
		return;
	*h++ = 0;
	char* p = strchr(h, '|');
	int port = 23;
	if (p) {
		*p++ = 0;
		port = atoi(p);
		if (port <= 0 || port > 65535)
			port = 23;
	}
	if (!*line || !*h)
		return;
	PbEntry* e = &entries[count++];
	snprintf(e->name, sizeof(e->name), "%s", line);
	snprintf(e->host, sizeof(e->host), "%s", h);
	e->port = (u16)port;
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
		char line[128];
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			parseLine(line);
		}
		fclose(f);
	}

	if (count == 0) {
		// SD unwritable or file empty: bake in the defaults
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
