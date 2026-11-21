# SPDX-License-Identifier: MIT
#
# The host half of metalwire: the Vulkan translator and its tests.
#
#   make            metalwire_host
#   make test       build, then run the generator + host on lavapipe (skips without lavapipe)
#   make clean
#
# The guest half is built by the Darwin toolchain (guest/metal-plugin/build.sh), not here.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lvulkan

HOST_SRC = metalwire_host.c metalwire_protocol.h

all: metalwire_host

metalwire_host: $(HOST_SRC)
	$(CC) $(CFLAGS) -o $@ metalwire_host.c $(LDLIBS)

test: metalwire_host
	PYTHONDONTWRITEBYTECODE=1 python3 metalwire_test.py

clean:
	rm -f metalwire_host

.PHONY: all test clean
