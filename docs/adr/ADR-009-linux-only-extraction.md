# ADR-009: firmware extraction is Linux-only; a Mac is needed only for `guest/`

- Status: Accepted
- Date: 2026-10-31
- Supersedes: ADR-007 of `docs/zh/design-baseline.md` ("build in two environments, one of them a Mac")
- Superseded by: —

## Background

The design baseline assumed that laying out an IPSW requires Apple's tools (`ipsw`, `hdiutil`),
and therefore that every contributor needs a Mac before they can boot anything. That assumption
sets the project's hardest admission bar (risk R5) and puts a proprietary toolchain in the middle
of an otherwise open pipeline.

## Evidence

HakiOS's `sysimage/unpack.sh` does the whole job on Linux. Its own header lists the requirements:
`python3`, `7z` (7-Zip 23 or newer, for APFS), `cc` — and it compiles a small `unzbm` from
`xnu-deps/build/lzbitmap/` to finish the LZBITMAP-compressed files that 7-Zip leaves empty.
The script copies the `.dmg` members out of the IPSW with `ipsw-member.py`, extracts the APFS
containers with 7-Zip, and lays out `rootfs/`, `cryptex/`, the App cryptex and the dyld shared
cache. No `hdiutil`, no `ipsw` CLI, no `otool`, no `codesign`, no macOS.

`tools/doctor.sh` on this host confirms the pieces: `python3 3.12`, `7-Zip 23.01`, `cc`, and
`lz4` — all present or one `apt install` away.

## Decision

1. `tools/unpack-ipsw.sh` and `tools/make-images.sh` in this repository are **Linux-only** and
   must stay that way. Any step that needs macOS is a bug in the tool, not a documented
   limitation.
2. The Mac requirement is confined to building `guest/metal-plugin`, which compiles ObjC against
   the iPhoneOS SDK (see `IMPLEMENTATION.md` for M2). That code is **built once and shipped as a
   guest-side component inside the user's own image**, never as an Apple-derived artifact.
3. CI has one Linux tier that is the merge gate, and the Mac-only guest build is a documented
   manual step (`ci/mac.md`), not a CI job.

## Consequences

**Positive**

- The contributor gate is: a Linux box, a C compiler, Python, 7-Zip. No Mac, no Xcode, no Apple
  account. R5 drops sharply.
- The pipeline is inspectable end to end. A reviewer can read every byte that reaches the guest.

**Negative**

- 7-Zip must be ≥ 23 for APFS. Older 7-Zip silently produces empty files instead of failing;
  `unpack-ipsw.sh` must check its version and refuse, not limp.
- We re-implement a small amount of what Apple's tools do (LZBITMAP, decmpfs, the dyld cache
  layout). Each of those is a place to get it subtly wrong, so each gets a test.
- The Mac path is still required for `guest/`. A Linux-only contributor can do M0, M1 and most of
  M2, but not the final Metal plugin. That is a real, documented split — not a hidden one.

## Follow-up

- `tools/unpack-ipsw.sh` must print the exact `7z` version it found and refuse below 23.
- A `make unpack-check` target that verifies an extraction against a manifest of **counts and
  sizes only** (no Apple content, no hashes of Apple files that would leak information) so a
  contributor can tell "it worked" without uploading anything.
