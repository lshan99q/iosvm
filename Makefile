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
HAKIOS  ?=
SECONDS_RUN ?= 120

# The guest firmware is the contributor's own. Nothing here downloads it.
IPSW    ?=
FIRMWARE_DIR := firmware
USERLAND_DIR := userland

BUILD   := build
VKBRIDGE := $(BUILD)/host/vkbridge/vkbridge
PROBE   := $(BUILD)/guest/iosvm-probe

.PHONY: help doctor test-open test test-gpu provenance style fmt host guest qemu run clean submodule unpack unpack-plan images qemu-open qemu-open-check proto components

help: ## this list
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

doctor: ## report what this machine lacks, and what to install
	@tools/doctor.sh

# ---------------------------------------------------------------------------
# Tier 1: no Apple file, no QEMU fork, no GPU. The CI gate.
# ---------------------------------------------------------------------------

test-open: provenance ## every check that needs no Apple file (the contributor gate)
	@ran=0; \
	if [ -f proto/Makefile ]; then echo "== protocol (proto/ submodule) =="; $(MAKE) -s -C proto test >/dev/null && echo "  proto: pass" || { echo "  proto: FAIL"; exit 1; }; ran=$$((ran+1)); else echo "  proto/: not fetched (make submodule) — skipped"; fi; \
	echo "test-open: provenance clean, $$ran component suite(s) run"

provenance: ## no Apple artifacts, no undeclared binaries, no credentials
	@$(PYTHON) tools/provenance-check.py --root .

style: ## SPDX headers, dangling doc references, clang-format
	@$(PYTHON) tools/check-spdx.py
	@$(PYTHON) tools/check-links.py
	@if git ls-files -z '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm' | grep -qz .; then \
		git ls-files -z '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm' \
			| xargs -0 clang-format --dry-run --Werror; \
	else echo "  (no C sources yet)"; fi

fmt: ## format C/C++/ObjC in place
	@git ls-files -z '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.m' '*.mm' \
		| xargs -0 -r clang-format -i

test: test-open ## everything, including the parts that need a GPU
	@$(MAKE) test-gpu

test-gpu: ## build the backend and run its probe on lavapipe (no Apple file, no real GPU needed)
	@test -f host/vkbridge/Makefile || { echo "host/vkbridge is not fetched: run make submodule" >&2; exit 2; }
	$(MAKE) -C host/vkbridge
	@mkdir -p $(BUILD)
	VK_ICD_FILENAMES="$${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" \
		host/vkbridge/build/vkbridge-probe --screenshot $(BUILD)/screenshot.png

# ---------------------------------------------------------------------------
# Host side
# ---------------------------------------------------------------------------

host: ## build the host Vulkan backend (host/vkbridge submodule)
	@test -f host/vkbridge/Makefile || { echo "host/vkbridge is not fetched: run make submodule" >&2; exit 2; }
	$(MAKE) -C host/vkbridge

# ---------------------------------------------------------------------------
# Guest side (needs the iPhoneOS SDK, so: a Mac — see ADR-009)
# ---------------------------------------------------------------------------

guest: ## build guest/metal-plugin
	@echo "guest: FAILS BY DESIGN — needs the iPhoneOS SDK, and is M2 (ci/mac.md)" >&2
	@exit 1

# ---------------------------------------------------------------------------
# QEMU fork
# ---------------------------------------------------------------------------

submodule: ## fetch the pinned component repositories (ADR-010)
	git submodule update --init --recursive
	@tools/sync-components.sh --check

components: submodule ## build and test every pinned component
	$(MAKE) -C proto test
	$(MAKE) -C host/vkbridge test

proto: ## build and test the protocol library (proto/ submodule)
	@test -f proto/Makefile || { echo "proto/ is not fetched: run make submodule" >&2; exit 2; }
	$(MAKE) -C proto test

qemu: ## build QEMU with hw/virtio-iosvm-gpu
	@echo "qemu: FAILS BY DESIGN — neither the fork nor hw/virtio-iosvm-gpu exists yet (M2)" >&2
	@exit 1

# ---------------------------------------------------------------------------
# The boot smoke test that needs no IPSW: an open-source XNU under QEMU.
# It needs an upstream checkout because the kernel binary is 16 MB and cannot live here.
# ---------------------------------------------------------------------------

qemu-open: ## boot an open-source XNU under QEMU and show the log (needs HAKIOS=dir)
	@test -n "$(HAKIOS)" || { echo "give HAKIOS=/path/to/upstream/checkout (see tools/qemu-open.sh --help)"; exit 2; }
	tools/qemu-open.sh --hakios "$(HAKIOS)" --seconds "$(SECONDS_RUN)"

qemu-open-check: ## assert the boot markers (CI-able; exit 0 means BSD init was reached)
	@test -n "$(HAKIOS)" || { echo "give HAKIOS=/path/to/upstream/checkout (see tools/qemu-open.sh --help)"; exit 2; }
	tools/qemu-open.sh --hakios "$(HAKIOS)" --seconds "$(SECONDS_RUN)" --check

# ---------------------------------------------------------------------------
# The guest that needs an IPSW — the contributor's own, never fetched
# ---------------------------------------------------------------------------

unpack-plan: ## show what unpack would do, write nothing: make unpack-plan IPSW=/path/to/your.ipsw
	@test -n "$(IPSW)" || { echo "give IPSW=/path/to/your.ipsw (see tools/unpack-ipsw.sh --help)"; exit 2; }
	tools/unpack-ipsw.sh "$(IPSW)"

unpack: ## extract firmware from YOUR IPSW: make unpack IPSW=/path/to/your.ipsw
	@test -n "$(IPSW)" || { echo "give IPSW=/path/to/your.ipsw (see tools/unpack-ipsw.sh --help)"; exit 2; }
	tools/unpack-ipsw.sh "$(IPSW)" --apply

images: ## build userland/system.apfs from an unpacked IPSW (APFS writer: M1)
	tools/make-images.sh --apply

run: ## boot the VM
	@echo "run: FAILS BY DESIGN — no boot harness here yet (M0 tail, docs/phases.md)" >&2
	@echo "     what is proven, upstream: HakiOS 'make qemu-open' (no Apple file)" >&2
	@exit 1

clean: ## remove build products (never touches firmware/ or userland/)
	rm -rf $(BUILD) logs
