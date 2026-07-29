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
	CHECK(fl->flags & PB_FLAG_3D, "Futureland is tagged 3D-capable");

	// The v2 seed adds showcase boards but must not duplicate hosts that
	// already exist (baked defaults include Vertrauen)
	int vert = 0, cave = 0;
	for (int i = 0; i < pbCount(); i++) {
		if (!strcmp(pbGet(i)->host, "vert.synchro.net")) vert++;
		if (!strcmp(pbGet(i)->host, "cavebbs.homeip.net")) cave++;
	}
	CHECK(vert == 1, "seed does not duplicate an existing host");
	CHECK(cave == 1, "seed adds missing showcase boards");

	// Toggling cycles rlogin -> ssh -> telnet -> rlogin, ports following
	// only while they sit on the outgoing protocol's default
	pbToggleProto(0);
	fl = pbGet(0);
	CHECK(fl->proto == PROTO_SSH && fl->port == 4022,
	      "rlogin -> ssh:4022 (Futureland's non-standard SSH port)");
	pbToggleProto(0);
	fl = pbGet(0);
	CHECK(fl->proto == PROTO_TELNET && fl->port == 23, "ssh -> telnet:23");
	pbToggleProto(0);
	fl = pbGet(0);
	CHECK(fl->proto == PROTO_RLOGIN && fl->port == 1513,
	      "telnet -> rlogin:1513 (host-specific port)");

	// Terminal size: entries default to 0x0, which resolves to 80x25 so
	// phonebooks written before the field existed keep working
	u16 c = 0, r = 0;
	CHECK(pbGet(0)->cols == 0 && pbGet(0)->rows == 0, "size defaults to unset");
	pbSizeOf(0, &c, &r);
	CHECK(c == 80 && r == 25, "unset size resolves to 80x25");

	// The picker offers the SyncTERM screen modes, 80x25 first and 132x60
	// last; none may exceed the grid the renderer is budgeted for
	int np = pbSizePresetCount();
	CHECK(np >= 13, "the full SyncTERM mode list is offered");
	pbSizePreset(0, &c, &r);
	CHECK(c == 80 && r == 25, "first preset is 80x25");
	pbSizePreset(np - 1, &c, &r);
	CHECK(c == 132 && r == 60, "last preset is 132x60");
	for (int k = 0; k < np; k++) {
		pbSizePreset(k, &c, &r);
		CHECK(c <= PB_MAX_COLS && r <= PB_MAX_ROWS, "preset within budget");
	}

	// Choosing a preset stores it; choosing the default stores 0x0 rather
	// than pinning today's numbers
	pbSizePreset(4, &c, &r);
	pbSetSize(0, c, r);
	pbSizeOf(0, &c, &r);
	CHECK(c == 80 && r == 50, "preset 4 (80x50) applies");
	pbSetSize(0, 80, 25);
	CHECK(pbGet(0)->cols == 0 && pbGet(0)->rows == 0,
	      "choosing the default stores 0x0, not a pinned 80x25");

	// A custom override is kept verbatim; out-of-range is clamped, not
	// accepted (the renderer's vertex budget is sized from PB_MAX_*)
	pbSetSize(0, 100, 40);
	pbSizeOf(0, &c, &r);
	CHECK(c == 100 && r == 40, "custom size stored verbatim");
	pbSetSize(0, 400, 400);
	pbSizeOf(0, &c, &r);
	CHECK(c == PB_MAX_COLS && r == PB_MAX_ROWS, "oversized request is clamped");
	pbSetSize(0, 0, 0);
	CHECK(pbGet(0)->cols == 0, "0x0 restores the default");

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
