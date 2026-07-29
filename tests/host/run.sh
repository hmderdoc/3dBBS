#!/bin/sh
# Build and run the host-native terminal-core tests.
set -e
cd "$(dirname "$0")"
cc -Wall -O1 -I . -o /tmp/3dbbs_test_ansi \
	test_ansi.c \
	../../source/term/termbuf.c \
	../../source/term/ansi.c \
	../../source/term/palette.c \
	../../source/gfx/sixel.c
/tmp/3dbbs_test_ansi

cc -Wall -O1 -I . -o /tmp/3dbbs_test_pb \
	test_phonebook.c \
	../../source/ui/phonebook.c
/tmp/3dbbs_test_pb
