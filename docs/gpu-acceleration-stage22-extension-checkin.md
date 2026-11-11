# Extension check-in: lifecycle, ranked causes, one-boot experiment

Status: read-only analysis + one-boot design.  No boot from this document.

## 1. Timeline (raw launchd logs)

### s19o-p2e-c2 — GradientPosterExtension (xpcproxy forced)

| time | event |
|---|---|
| 06:37:02.003 | `launching: launch job demand` |
| 06:37:02.172 | `xpcproxy spawned with pid 57` |
| 06:37:02.250 | `SOURCE_ATTACH` |
| 06:37:08.466 | `service state: running`, `INIT` — **spawn+6.46 s** |
| 06:37:13.030 | `uncorking exec source upfront`, `created` — **INIT+4.56 s** |
| 06:37:13.03x | `Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 57` (immediately after exec) |
| 06:37:13.584 | `Extension is hanging on launch. Killing.` — **INIT+5.12 s**, exec+0.55 s |
| 06:37:14.687 | `service state: SIGKILLed`, `scheduling cleanup ... Killed: 9` |
| 06:37:17.549 | `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` / exit 0x3 |

### s20-e1-control — PhotosReliveWidget (xpcproxy elided, C1 limits)

| time | event |
|---|---|
| 23:19:31.101 | `WILL_SPAWN` / spawn scheduled |
| 23:19:31.412 | `service spawned with pid 45` |
| 23:19:31.413 | `INIT` — **spawn+0.001 s** |
| 23:19:35.183 | `uncorking exec source upfront`, `created` — **INIT+3.77 s** |
| 23:19:36.418 | `Extension is hanging on launch. Killing.` — **INIT+5.005 s**, exec+1.235 s |
| 23:19:36.419 | `service state: SIGKILLed`, cleanup `Killed: 9` |
| 23:19:37.827 | `CHECK_IN_TIMEOUT` / exit 0x3; corpse notification after the kill |

**Who kills**: launchd itself.  The sequence is launchd's
`killing extension hanging on launch` → `SIGKILLed` → `scheduling cleanup ... Killed: 9`.
There is no xpcproxy kill and no separate watchdog process.

**Timer origin**: the kill lands at **INIT + ~5.0 s** in both runs, while spawn→INIT varies
(6.46 s vs 0.001 s) and exec→kill varies.  `ExtensionWatchdogTimeout=120` never changes this.
`EXC_CORPSE_NOTIFY error 5` appears at exec or after the kill; it is a corpse notification
failure, not a separate killer.  `CheckIn` / `checked in as alive` is absent from every run.

## 2. Ranked hypotheses

### H1 (strongest) — hard-coded ~5 s launch watchdog from INIT; CheckIn deadline is simply too short

- Support: INIT→kill 5.12 s (C2) and 5.005 s (E1); timeout plist 120 ignored; launchd strings
  contain `launchd_disable_all_timeouts=` and `launch watchdog timeout = %u`.
- Counter: if the extension were normally under 5 s, it should check in; TCG startup may be
  >5 s.  The timeout key being ignored is itself a launchd bug/version mismatch.
- One-boot falsifier: `launchd_disable_all_timeouts=1`.  If `Extension is hanging` goes to 0 and
  the process stays alive or `checked in as alive` appears, H1 is confirmed.

### H2 — the extension process dies/wedges at exec before it can CheckIn

- Support: C2 corpse immediately after `uncorking exec`; no extension app log; no CheckIn line.
- Counter: E1's corpse is after the kill; corpse notification itself may only mean launchd's
  exception port failed, so it is not proof of an exec crash.
- One-boot falsifier: same `launchd_disable_all_timeouts=1` boot.  If a live pid remains but never
  checks in, H2/H3; if it disappears without a kill, H2.

### H3 — CheckIn is attempted but the launchd endpoint/name/bootstrap path is wrong

- Support: only `SOURCE_ATTACH`; no `checked in as alive`; C2 forces xpcproxy and still fails;
  extension services use the `...apple-extension-service` endpoint.
- Counter: launchd logs no endpoint error; forcing xpcproxy changed nothing, so elision is not it.
- Falsifier: disable timeouts, then watch `launchd_log_serial=1` and `checked in as alive`; if the
  pid lives but launchd never logs CheckIn, need an endpoint trace/stack, not more timeout runs.

### H4 — persona/entitlement/sandbox blocks the extension launch/check-in

- Support: every extension spawn is preceded by
  `kpersona_find_by_type() failed ... errno 3` / `Unable to find persona with type 6`.
- Counter: the same persona error appears for other user jobs, and system services run; it is
  not specific to extensions.  C2's forced xpcproxy did not help.
- Falsifier: a working persona type-6 setup is not a boot-arg; the cheap boot only records whether
  the persona lines precede **every** extension (they do).  This stays lower until H1–H3 are
  excluded.

## 3. Recommended cheapest one boot

Base: C1 control system **6be494f1…**, root **3207a35e…** (do not rebuild).
One boot, 900–1200 s, 1 vCPU:

```sh
XCMD="glwire_local=1 launchd_disable_all_timeouts=1 launchd_log_serial=1" \
HOST_ENV="GLWIRE_NO_PANEL=1" \
  ./harness/boot-measure.sh ext1 /tmp/root-byref.apfs 1000 --no-panel
```

Filters:

```sh
grep -aE "launch watchdog timeout|Extension is hanging|checked in as alive|CHECK_IN_TIMEOUT|uncorking exec|EXC_CORPSE|killing extension" /tmp/ext1.log
```

Pass/fail matrix:

| observation | conclusion |
|---|---|
| `launch watchdog timeout = (globally disabled)` and hanging count 0 | boot-arg reached launchd |
| `checked in as alive` >0 | H1 confirmed: watchdog was the gate |
| extension pid alive >120 s, no CheckIn, no kill | H2/H3; watchdog not the gate |
| extension still killed at INIT+5 s or corpse before check-in | not H1; H2 exec/init or H3 endpoint |
| boot hangs / no UI | disabling all timeouts too broad; record and revert |

This one boot is low cost (no image rebuild, no new job) and can falsify H1 decisively.
Do not change the limits plist, jobs, or timeout again in this boot.

## 4. Can three boots decide?

**No, not the complete root cause.**  One boot can decide **watchdog vs not** (H1) because it
removes the timer.  Two more can separate H2 (process dies) from H3 (process lives but CheckIn
is not delivered), but the actual CheckIn break needs evidence that a boot-only A/B cannot give:

- a **live extension pid sample/stack** (`sample`, `spindump`, or a debugger) while timeouts are
  disabled, or
- **launchd/xpcproxy source-level trace** around `CheckIn` / the implicit endpoint, or
- permission to rebuild those binaries with a trace (not a boot arg).

Without one of those, the project can only say "watchdog or exec/check-in" and must not claim the
root cause.  Three boots are enough to classify, not to fix.

## 5. ext1 post-mortem and boot 2 (budget 3)

`logs/s22-ext/ext1` ran to **acceptance at ui=660 s**, but the first extension in the C1 control
appears at **666 s** (E1 control: PhotosReliveWidget hanging at 23:19:36 = 666 s).  So the run
stopped **before the extension phase**; the five zero counters are expected, not a result.

The guest serial file is only 2.3 KB (loader header).  `launchd_log_serial=1` was in the cmdline;
it is the suspected cause of the serial console going quiet (launchd takes/logs the console), and
must not be combined with the normal kernel console capture.

### Boot 2 (H1 falsifier, no serial flag, past extension phase)

Base: C1 control system `6be494f1…`, root `/tmp/root-byref.apfs` (3207a35e…), `QEMU_SMP=1`,
`ZERO_RAM=1`.

```sh
XCMD="glwire_local=1 launchd_disable_all_timeouts=1" \
HOST_ENV="GLWIRE_NO_PANEL=1" \
MINFRAMES=999999 MINSHOT=300000 UI_GRACE=400 NOUI_BY=900 MAXSECS=1120 \
  ./harness/boot-measure.sh ext2 /tmp/root-byref.apfs 1120 --no-panel
```

- `MINFRAMES=999999` disables the normal 200-frame acceptance;
- `UI_GRACE=400` keeps running 400 s after the first 300 KB shot (UI ~660 s → through ~1060 s),
  which covers the first extension at ~666 s and >300 s of extension lifetime;
- no `launchd_log_serial`; kernel serial should stay in `/tmp/ext2.log`.

Filters:

```sh
grep -aE "launch watchdog timeout|Extension is hanging|checked in as alive|CHECK_IN_TIMEOUT|uncorking exec|EXC_CORPSE|killing extension|xpcproxy spawned" /tmp/ext2.log
```

Decision:

- `launch watchdog timeout = (globally disabled)` + hanging 0 → H1 boot-arg reached launchd;
- `checked in as alive` >0 → **H1 confirmed** (watchdog gate);
- extension pid alive after 900 s, no check-in, no kill → H2/H3;
- extension still killed at INIT+5 s or corpse early → not H1;
- **serial file still only loader header** → capture QEMU serial separately (for example a dedicated
  `-serial file:` chardev without `launchd_log_serial`) before spending boot 3.

If boot 2 gets the serial and answers H1, boot 3 is only for H2 vs H3 (a live-pid sample).  If the
serial is still missing, stop: the budget is better spent on a serial-capture fix than another
content boot.
