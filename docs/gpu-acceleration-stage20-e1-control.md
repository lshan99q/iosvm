# Stage 20 E1 control — C1 limits, `SYSTEM=1` rebuild

**Result: the control boots normally.** UI at 720 s (`ui_bytes=309280`,
`biggest=456750`), extension `hanging` 10, `CHECK_IN_TIMEOUT` 10,
`checked in as alive` 0.  The `SYSTEM=1` rebuild itself is **not** the problem;
the bad image is the E1 `15167c8a` (Watchdog=false / CrashOnSIGTERMTimeout=false).

## What this boot changed

- vs **C1**: the limits plist is fully restored (`CrashOnSIGTERMTimeout=true`,
  `ExtensionWatchdog=true`, `ExtensionWatchdogTimeout=120`), no key change; the
  only change is the `SYSTEM=1` rebuild from the current tree (system md5
  `6be494f1`, C1's old image was `8bf5f838`).
- vs **E1**: two keys differ (E1 had both false); this is an environment-repair
  control, not E1's single-variable control.

## Build / boot

- system: 23:07:50Z, 2,476,752,896 B, md5 **6be494f16775402cab1fa2120ca24112**;
  the extracted plist has the three C1 keys.
- the bad E1 system `15167c8a` is backed up at `/tmp/system.apfs.bak-s20-bad`.
- root unchanged `785bcbac`; boot root `/tmp/root-byref.apfs` `3207a35e`;
  `glwire_local=1`, NO_PANEL, `MAXSECS=900 UI_GRACE=0`.
- `xb: s20-e1-control end=qemu-exited ui=720s ui_bytes=309280 ...
  guestframes=32 hostframes=304 MATCH=56 DIFF=248 ... panic=0 biggest=456750`.

## Extension (control)

| item | value |
|---|---|
| `Extension is hanging` | **10** (first: PhotosReliveWidget [45], 23:19:36, 666 s) |
| `CHECK_IN_TIMEOUT` | **10**, exit reason `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` (code 0x3) |
| `checked in as alive` | **0** |
| `EXC_CORPSE_NOTIFY` | 8: pid 22 configd + extension pids 47–50 |
| UI | 720 s / 309,280 B; biggest 456,750 |

So the control reproduces C1/P0: UI + extension hanging/timeout, check-in never
happens.  The rebuild is fine.

## The bad key is `ExtensionWatchdog=false`

A single-variable side control, system `a3a895b0` (`ExtensionWatchdog=true`,
`CrashOnSIGTERMTimeout=false`, Timeout 120), differs from E1 **only** in the
`ExtensionWatchdog` key.  At 650 s it already had the first `Extension is
hanging` (PeopleViewService.LockScreenPeopleWidget [44]), 6 in total; it was
stopped when the C1-limits control was requested.  E1 `15167c8a` (false/false)
had 2 frames / no UI / no extension at 700 s.  The rebuild and
`CrashOnSIGTERMTimeout=false` alone are fine; **`ExtensionWatchdog=false` is what
stops the boot/extension phase**.

E1's 700 s is only 20 s before the control's UI at 720 s, but the side control
already had extensions at 650 s while E1 had none at 700 s; this is not just a
20 s sampling difference.

## Stage 20 E1 conclusion

- **E1 = C (not decidable / not viable)**: `ExtensionWatchdog=false` does not let
  an extension live to CheckIn; it stops the boot/extension phase instead.  No
  `checked in as alive`, no extension exit code from this path.
- The C1 control still shows 0 alive and 10 `CHECK_IN_TIMEOUT` / exit 0x3: the
  extension check-in timeout remains (same family as C2's
  `EXC_CORPSE_NOTIFY`); not a single limits boolean fix, and not fixable in the
  30-minute box.
- If the owner still wants a clean "watchdog off only" test, use the **working
  C1 base with only `ExtensionWatchdog=false`** (`CrashOnSIGTERMTimeout=true`)
  for 900–1200 s; do not reuse the bad `15167c8a`.  Per the lead's "control only"
  instruction, no false second boot was run this time.

Artifacts (not committed): `/host/iosvm/logs/s20-e1-control/` and the
single-variable log under `/host/iosvm/logs/s20-e1-watchdog/control-a3a895b0-650s/`.
