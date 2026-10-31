# IOSVM build entry points.
#
# Every contributor, with no iPhone, no Mac and no IPSW, must be able to run:
#
#     make test-open
#
# Keep that true. Targets under "needs an IPSW" may fail on a bare machine; that is expected
# and is why they are not in the CI gate.

SHELL   := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := help

JOBS    ?= $(shell nproc)
PYTHON  ?= python3

# The guest firmware is the contributor's own. Nothing here downloads it.
IPSW    ?=
FIRMWARE_DIR := firmware
USERLAND_DIR := userland

BUILD   := build
VKBRIDGE := $(BUILD)/host/vkbridge/vkbridge
PROBE   := $(BUILD)/guest/iosvm-probe

.PHONY: help doctor test-open test test-gpu provenance style fmt host guest qemu run clean submodule

help: ## this list
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

doctor: ## report what this machine lacks, and what to install
	@tools/doctor.sh

# ---------------------------------------------------------------------------
# Tier 1: no Apple file, no QEMU fork, no GPU. The CI gate.
# ---------------------------------------------------------------------------

test-open: provenance ## every check that needs no Apple file (the contributor gate)
	@echo "== protocol units =="
	@if [ -f tests/unit/run.sh ]; then tests/unit/run.sh; else echo "  (none yet)"; fi
	@echo "== fake-gpu =="
	@if [ -f tests/fake-gpu/run.sh ]; then tests/fake-gpu/run.sh; else echo "  (none yet)"; fi
	@echo "test-open: green"

provenance: ## no Apple artifacts, no undeclared binaries, no credentials
	@$(PYTHON) tools/provenance-check.py --root .

style: ## SPDX headers + clang-format check
	@$(PYTHON) tools/check-spdx.py
	@if git ls-files -z '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm' | grep -qz .; then \
		git ls-files -z '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm' \
			| xargs -0 clang-format --dry-run --Werror; \
	else echo "  (no C sources yet)"; fi

fmt: ## format C/C++/ObjC in place
	@git ls-files -z '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm' \
		| xargs -0 -r clang-format -i

test: test-open ## everything, including the parts that need a GPU
	@$(MAKE) test-gpu

test-gpu: ## vkbridge + probe against lavapipe (no Apple file needed, needs a Vulkan driver)
	@echo "test-gpu: not implemented yet (M2)"

# ---------------------------------------------------------------------------
# Host side
# ---------------------------------------------------------------------------

host: $(VKBRIDGE) ## build host/vkbridge

$(VKBRIDGE):
	@echo "host: not implemented yet (M2)"

# ---------------------------------------------------------------------------
# Guest side (needs the iPhoneOS SDK, so: a Mac — see ADR-009)
# ---------------------------------------------------------------------------

guest: ## build guest/metal-plugin
	@echo "guest: needs the iPhoneOS SDK; not implemented yet (M2)"

# ---------------------------------------------------------------------------
# QEMU fork
# ---------------------------------------------------------------------------

submodule: ## fetch the QEMU fork
	git submodule update --init --recursive

qemu: submodule ## build QEMU with hw/virtio-iosvm-gpu
	@echo "qemu: the hw/ device does not exist yet (M2); the unmodified fork builds with:"
	@echo "      cd qemu && ./configure --target-list=aarch64-softmmu --disable-pvg && ninja"

# ---------------------------------------------------------------------------
# The guest that needs an IPSW — the contributor's own, never fetched
# ---------------------------------------------------------------------------

unpack: ## extract firmware from YOUR IPSW: make unpack IPSW=/path/to/your.ipsw
	@test -n "$(IPSW)" || { echo "give IPSW=/path/to/your.ipsw (see tools/unpack-ipsw.sh --help)"; exit 2; }
	tools/unpack-ipsw.sh "$(IPSW)"

images: ## build userland/system.apfs and userland/data.apfs from firmware/
	tools/make-images.sh --apply

run: ## boot the VM
	@echo "run: not implemented yet (M0) — needs a firmware/ from your own IPSW"
	@echo "      upstream reference: HakiOS 'make qemu-open' (no Apple file) and its QEMU Darwin path"

clean: ## remove build products (never touches firmware/ or userland/)
	rm -rf $(BUILD) logs
