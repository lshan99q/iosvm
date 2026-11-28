#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the upstream QEMU 8.2.2 tree that the M2 accelerator work patches.
#
# Everything happens outside the repository: the tarball, the source tree and
# the build tree all live under $QEMU_WORK_DIR (default /home/lshan/xlt). The
# script is idempotent: an existing qemu-system-aarch64 is reused, a partial
# build is continued with ninja.
set -euo pipefail

QEMU_VERSION=8.2.2
# sha256 of qemu-8.2.2.tar.xz, cross-checked against the release signature
# (GPG key CEACC9E15534EBABB82D3FA03353C9CEF108B584, Michael Roth).
QEMU_SHA256=847346c1b82c1a54b2c38f6edbd85549edeb17430b7d4d3da12620e2962bc4f3

WORK_DIR=${QEMU_WORK_DIR:-/home/lshan/xlt}
JOBS=${QEMU_JOBS:-16}
LOG=${QEMU_BUILD_LOG:-/tmp/qemu-build.log}

SRC_DIR="$WORK_DIR/qemu-$QEMU_VERSION"
BUILD_DIR="$WORK_DIR/qemu-$QEMU_VERSION-build"
TARBALL="$WORK_DIR/qemu-$QEMU_VERSION.tar.xz"
QEMU_BIN="$BUILD_DIR/qemu-system-aarch64"
URL="https://download.qemu.org/qemu-$QEMU_VERSION.tar.xz"

if [ -x "$QEMU_BIN" ]; then
    echo "$QEMU_BIN"
    exit 0
fi

mkdir -p "$WORK_DIR"

if [ ! -f "$TARBALL" ]; then
    echo "downloading $URL" >&2
    wget -O "$TARBALL.part" "$URL"
    mv "$TARBALL.part" "$TARBALL"
fi

echo "$QEMU_SHA256  $TARBALL" | sha256sum -c - >&2

if [ ! -d "$SRC_DIR" ]; then
    echo "extracting $TARBALL" >&2
    tar -C "$WORK_DIR" -xf "$TARBALL"
fi

if [ "$(cat "$SRC_DIR/VERSION")" != "$QEMU_VERSION" ]; then
    echo "unexpected VERSION in $SRC_DIR: $(cat "$SRC_DIR/VERSION")" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
echo "build log: $LOG" >&2
{
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        (
            cd "$BUILD_DIR"
            "$SRC_DIR/configure" \
                --target-list=aarch64-softmmu \
                --disable-docs \
                --disable-werror \
                --disable-gtk \
                --disable-sdl \
                --disable-vnc \
                --disable-opengl \
                --disable-spice \
                --disable-libssh \
                --disable-guest-agent
        )
    fi
    ninja -C "$BUILD_DIR" -j"$JOBS"
} 2>&1 | tee "$LOG"

if [ ! -x "$QEMU_BIN" ]; then
    echo "build finished but $QEMU_BIN is missing; see $LOG" >&2
    exit 1
fi

echo "$QEMU_BIN"
