# SPDX-License-Identifier: MIT
#
# The host half of glwire, and its tests. The guest half is built by the Darwin userland build
# (userland/build.sh GL=1), not here.
#
#   make            frame_replay, host_glwire, map_test
#   make test       map_test, e2e_test.py, then surface_flip_test.py (the offline row-order check
#                   of the GLWIRE_SURFACE_FLIP experiment)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall
LDLIBS  ?= -lEGL -lGL

HOST_SRC = wire.h names.h dispatch.inc encode.inc

all: frame_replay host_glwire map_test

frame_replay: frame_replay.c frame_record.c $(HOST_SRC)
	$(CC) $(CFLAGS) -o $@ frame_replay.c frame_record.c $(LDLIBS)

host_glwire: host_glwire.c $(HOST_SRC)
	$(CC) $(CFLAGS) -o $@ host_glwire.c $(LDLIBS)

map_test: map_test.c map_stream.c $(HOST_SRC)
	$(CC) $(CFLAGS) -o $@ map_test.c map_stream.c $(LDLIBS)

test: map_test host_glwire
	./map_test
	./e2e_test.py
	./surface_flip_test.py

clean:
	rm -f frame_replay host_glwire map_test

.PHONY: all test clean
