# Stage 20 E1: the rebuilt system image is a confound (read-only audit)

**One-line verdict: rebuild problem/confound — the E1 C result cannot be attributed to
`ExtensionWatchdog=false`.**  The old `system.apfs` (`8bf5f838…`) is no longer on disk, so an
exact old/new file-list diff is impossible; the new image (`15167c8a…`) is a full `SYSTEM=1`
repack, not a one-file plist swap.

## What can be checked

- New image: 20:51:10Z, 2,476,752,896 B, md5 `15167c8a01fe2e074caad610686df895`, same size as
  old; `tools/apfs census` = 62,274 entries (45,052 regular, 16,548 dirs, 674 symlinks), 0 bad
  checksums.  No truncation/corruption.
- `/private/etc/com.apple.xpc.launchd.limits.plist` is present: 693 B, 0:0 0644, valid; contains
  `ExtensionWatchdog=false`, `ExtensionWatchdogTimeout=120`, **`CrashOnSIGTERMTimeout=false`**.
- Old P2e baseline had `ExtensionWatchdog=true` and `CrashOnSIGTERMTimeout=true`; two booleans
  changed in the same file, not one.
- `SYSTEM=1` repacks the whole `sysroot` from current `sysimage/rootfs` + `system.manifest`;
  the working tree also has uncommitted `build.sh`/`mkjobs.py` (and drivers/prebuilt/qemu), so
  the job/content set is not provably the 05:10 image's set.
- `/System/Library/LaunchDaemons` is a symlink to `/redwood/LaunchDaemons`; root has 38 job
  files (all mtime 22:44), including the optional `com.iosvm.layers.plist` copied by build.sh.
  Whether the old image had the same root jobs is unknown.
- `/System/Library/xpc/launchd.plist` missing and `/AppleInternal` bootstrap errors are also in
  the old P0 log; they are not new.
- New boot: SpringBoard spawns 21:00:34, `created` 21:00:39, but only 2 frames / no UI; old
  system with the same root reaches UI at 660 s.  New boot has 0 extension WILL_SPAWN/hanging/
  CheckIn; old had 12 hanging / 11 timeout after 611 s.  `configd` corpse/faults are not
  extension exit codes.

## Judgment

The plist is not swallowed, the owner/mode is normal, and the LaunchDaemons symlink exists; the
suspicion is the full repack plus the second plist boolean (and uncommitted build/job sources),
not `ExtensionWatchdog=false` itself.  A control with the same new system but the old plist
(`ExtensionWatchdog=true`, `CrashOnSIGTERMTimeout=true`) is required before reading E1 as A/B/C.
No boot/implementation here.
