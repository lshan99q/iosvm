# Stage 20 E1 — `ExtensionWatchdog=false` (30-minute timebox)

**Verdict (at 21:13): C — need more information (inconclusive, not "infeasible"); final after control: C（不可判/不可行）.** The single
700 s boot did not reach the extension phase: `checked in as alive` 0,
`Extension is hanging` 0, `CHECK_IN_TIMEOUT` 0, and no extension exit code.  The
only `EXC_CORPSE_NOTIFY` is **configd pid 22** (4 lines: launchd 2 + kernel 2),
not an extension.  The new system image boot is stuck at 2 frames / no UI
(`biggest 24,960 B`) at 700 s, while the P0 baseline (old system, same root)
reached the UI at 660 s and the first extension `hanging` at 611 s.  The 30-minute
cap ended the experiment before the UI/extension phase; the watchdog-false effect
could not be observed.

> **Update after control (2026-11-10): final C（不可判/不可行）。** Control（system
> `6be494f1`）到 UI 720s、hanging 10、CHECK_IN_TIMEOUT 10、alive 0；单变量
> `a3a895b0` 只差 Watchdog，650s 已有 hanging → **`ExtensionWatchdog=false` 是坏键**；
> check-in timeout 未解决，P2/P3 不重开，system 已恢复 `6be494f1`。详见
> `docs/gpu-acceleration-stage20-e1-control.md`。下面的 “Why C/Next” 是当时记录；
> “control 未跑”已不再适用。

## Build

- `system.apfs`: 20:51:10Z, 2,476,752,896 B, md5 **15167c8a01fe2e074caad610686df895**
  (old 8bf5f838), rebuilt with `SYSTEM=1`.
- `/private/etc/com.apple.xpc.launchd.limits.plist` extracted from the image:
  `SIGTERMTimeout=3`, **`ExtensionWatchdog=false`**, `ExtensionWatchdogTimeout=120`,
  **`CrashOnSIGTERMTimeout=false`**.  C1 had `CrashOnSIGTERMTimeout=true`; code's
  20:45 plist edit set it false — a second limits key, noted as a caveat (not a
  timeout/job change).
- `root.apfs` unchanged (`785bcbac`; restored after the build); the boot used
  `/tmp/root-byref.apfs` `3207a35e` (the C1 P2d root is gone).

## Boot

`xb: s20-e1-watchdog end=qemu-exited ui=0s ui_bytes=0 2026-11-09T21:12:51+00:00
guestframes=2 hostframes=2 MATCH=1 DIFF=1 SIGSEGV=0 usertrap=0 panic=0 biggest=24960`

| item | E1 | P0 baseline (old system, same root) |
|---|---|---|
| UI | **0 s / 24,960 B (boot logo)** | 660 s / 327,177 B |
| host frames | **2** | 1,080 |
| `checked in as alive` | **0** | 0 |
| `Extension is hanging` | **0** | **12** (first at 611 s) |
| `CHECK_IN_TIMEOUT` | **0** | **11** |
| `EXC_CORPSE_NOTIFY` | **4, all configd pid 22** | configd-class |
| extension service names (mobilecal/news.tag/ScreenTime/WidgetExtension) | **0** | present |

The E1 zeros are **not** evidence that the watchdog-off works: no extension
service was spawned (`WILL_SPAWN` 68 are system jobs).  The configd corpse/faults
(`Full corpse enqueued for configd`; `configd[22] Corpse failure, too many faults 2`
at 143/166 s) accompany the new-system boot; they are not an extension exit code.

## Why C, not A/B

The new system image (15167c8a) boots to 2 frames / no UI, while the old system
with the same root reaches UI at 660 s.  The `SYSTEM=1` rebuild is a confound:
the boot stop cannot be attributed to the plist false, to
`CrashOnSIGTERMTimeout=false`, or to the rebuild itself.  The 30-minute cap
(ended 21:13:46) left no time for a control boot or a longer run; by the agreed
rule, close as C and do not invest further.

## Next (control later run; historical plan)

1. **Control:** same new system with `ExtensionWatchdog=true` /
   `CrashOnSIGTERMTimeout=true` for 700–900 s.  If it still stops at 2 frames,
   the rebuild/image is the problem, not the watchdog.
2. If the control reaches UI/extensions, run false for 900–1200 s (P0's first
   extension hang is at 611 s), wait ≥60 s after spawn, and record
   `checked in as alive` / hanging / CHECK_IN_TIMEOUT / exit code.
3. No concurrent boots; no job/timeout changes.

Artifacts (not committed): `/host/iosvm/logs/s20-e1-watchdog/`
(`s20-e1` is the same directory).
