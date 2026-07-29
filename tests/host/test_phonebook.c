// Host-native tests for the phonebook model. SD paths fail to open here,
// so pbLoad() falls back to the baked defaults — which is exactly the
// state we want to exercise.
#include <stdio.h>
#include <string.h>
#include "../../source/ui/phonebook.h"

static int failures;
#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
} while (0)

int main(void)
{
	pbLoad();
	CHECK(pbCount() >= 2, "defaults loaded");

	const PbEntry* fl = pbGet(0);
	CHECK(!strcmp(fl->name, "Futureland"), "entry 0 is Futureland");
	CHECK(fl->proto == PROTO_RLOGIN && fl->port == 1513,
	      "Futureland defaults to rlogin:1513 (513 is closed on that host)");

	// Toggling round-trips protocol and port together
	pbToggleProto(0);
	fl = pbGet(0);
	CHECK(fl->proto == PROTO_TELNET && fl->port == 23, "rlogin -> telnet:23");
	pbToggleProto(0);
	fl = pbGet(0);
	CHECK(fl->proto == PROTO_RLOGIN && fl->port == 1513,
	      "telnet -> rlogin:1513 (host-specific port)");

	// A generic host uses the standard rlogin port
	int idx = pbAdd("Generic", "example.org", 23);
	CHECK(idx > 0, "pbAdd appends");
	pbToggleProto(idx);
	CHECK(pbGet(idx)->proto == PROTO_RLOGIN && pbGet(idx)->port == 513,
	      "generic host toggles to rlogin:513");

	// Credentials persist on the entry
	pbSetCreds(idx, "someuser", "somepass");
	CHECK(!strcmp(pbGet(idx)->user, "someuser"), "username stored");
	CHECK(!strcmp(pbGet(idx)->pass, "somepass"), "password stored");

	// Edit and delete
	pbSetEntry(idx, "Renamed", "other.example", 9999);
	CHECK(!strcmp(pbGet(idx)->name, "Renamed") && pbGet(idx)->port == 9999,
	      "pbSetEntry updates fields");
	int before = pbCount();
	pbDelete(idx);
	CHECK(pbCount() == before - 1, "pbDelete removes");

	// The last entry can never be deleted
	while (pbCount() > 1)
		pbDelete(0);
	pbDelete(0);
	CHECK(pbCount() == 1, "last entry is protected from delete");

	printf(failures ? "%d FAILURES\n" : "all phonebook tests passed\n", failures);
	return failures ? 1 : 0;
}
