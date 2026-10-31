# Validating the APFS writer against Apple's own reader

The APFS container that `tools/apfs/mkapfs.py` writes is checked locally by `apfsck`
(`linux-apfs/apfsprogs`) and by a 7-Zip read-back. Neither is Apple's code. On 2026-10-31 a
cloud Mac (macOS 26.3, arm64) was available for one hour, so the images were put in front of
Apple's own filesystem checker.

## Result: `fsck_apfs` accepts them

Small synthetic container (4 MiB, two files, one directory level):

```
** The volume /dev/r.apfss1 with UUID 4FB9F375-… appears to be OK.
** The container /tmp/m.apfs appears to be OK.
```

44 MiB container built from **911 real Darwin 16.5 files and 1,095 directories** taken out of the
extracted system volume — including 862 files stored compressed (276 in resource forks, 586 whole
in the `decmpfs` attribute) and 6 symbolic links:

```
** Checking the fsroot tree.
** Checking the extent ref tree.
** Verifying volume object map space.
** The volume /dev/real.apfss1 with UUID D540F53D-… appears to be OK.
** The container /tmp/real.apfs appears to be OK.
```

That is three independent readers agreeing: `apfsck`, 7-Zip, and `fsck_apfs`.

## What `hdiutil attach` needs, and why it does not matter here

`hdiutil attach -imagekey diskimage-class=CRawDiskImage` refuses a `mkapfs` image with
`无可装载的文件系统` / `无法识别映像`. The reason is structural: `mkapfs` writes a **bare APFS
container**, the way a container sits inside a partition when newfs_apfs runs on a slice, and
`hdiutil` wants a partitioned disk image. Wrapping the container in a GPT with partition type
`7C3457EF-0000-11AA-AA11-00306543ECAC` did not make this hdiutil version accept it either, and the
one-hour window did not allow finishing that investigation.

It does not block anything: the guest gets the container as a **raw virtio disk**, which is exactly
what `mkapfs` produces and what the kernel's APFS layer reads — no partition table in the path. The
mount-with-hdiutil question is a convenience for inspecting images on a Mac, not a property the
boot depends on.

## The Mac's toolchain, as measured

- macOS 26.3 on arm64 with **Command Line Tools only** — no `Xcode.app`, therefore no `iphoneos`
  SDK and no `iig`. Anything that needs those (the arm64e `xnu-12377` kernel build in
  `docs/research/d1-prime-arm64e.md`) **cannot be built on such a box at all**, in an hour or a day.
- `clang -arch arm64` produces Mach-O binaries that run: `clang -arch arm64 -o h h.c && ./h` prints
  and exits 0. `-arch arm64e` also produces `Mach-O 64-bit executable arm64e`.
- On Apple Silicon every binary must be signed — an unsigned one dies with SIGKILL (status 137).
  `codesign -s - -f BINARY` fixes it for ordinary dynamically linked programs.
- A `-nostdlib -static` no-libc binary stayed SIGKILL even after ad-hoc signing (its `LC_UNIXTHREAD`
  entry was correct and the symbol was there). Not pursued: guest binaries link against libSystem and
  resolve from the dyld shared cache anyway, which is the arrangement M1 needs.
