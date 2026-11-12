# Stage 22 final: extensions spawn but never CheckIn (no hang, no host_info call)

**Status: closed to the evidence boundary.**
The main symptom is now: extension **spawns → INIT/running → never sends CheckIn,
and does not report hanging**.  The earlier "stuck in host_info" stack (ext3) was a
**transient state of one boot**, not the universal root cause.

## 1. Effective experiments

| run | configuration | observation | evidence |
|---|---|---|---|
| **ext2** | `launchd_disable_all_timeouts=1`; no `launchd_log_serial`; >1000 s | 5 s hanging/CHECK_IN_TIMEOUT/killing **0**; widgets spawn/uncork; `checked in as alive` **0** | `logs/s22-ext/ext2/` |
| **ext3** | timeout disabled + `ustacks=PodcastsWidget` at 680 s, 90 samples | every sample: `_mach_msg2_trap+8` → `_host_info+228` → `___pthread_init+164` → `_libSystem_initializer+132`; pid alive; no CheckIn | `logs/s22-ext/ext3/serial.log`; `docs/s22-ext-03-symbolized.txt` |
| **ext4** | correct-layout kernel `4d9f32…`, `hakios_host_info_trace=1`; UI 840 s | `hakios: host_info` **0**; cmdline trace=1 (269/607); widgets spawn; no CheckIn | `logs/s22-ext/ext4/` |
| **ext8** | kernel `1f8c489f…`, default-on hostport/host_info traces; system processes at 117–166 s | hostport **35 行**（id 200×6 / 205×23 / 206×6）；host_info **15 行**（flavor 1/5，全部 ret=0） | `logs/s22-ext/ext8/` |
| **ext9** | 标准配方；UI 660 s；`launchd_disable_all_timeouts=1` | 扩展 spawn 成功（**GCWidgets[48]、BatteriesWidget[49]**，70 行 INIT 迹象）；`Extension is hanging` **0**、`checked in as alive` **0**；内核 `hostport`/`host_info` 全程 **0** | `logs/s22-ext/ext9/` |

(ext5 sampled the wrong truncated name and is **not** counted.)

## 1.1 ext9: the decisive symptom

- **Spawn succeeds**: `GCWidgets[48]` (10:55:55) and `BatteriesWidget[49]`
  (10:55:57); the log has 70 `INIT`-related lines; service reaches `running`.
- **No CheckIn**: `checked in as alive` **0**.
- **No hang**: `Extension is hanging` **0**; with timeouts disabled, nothing kills it.
- **No kernel call**: `hakios: hostport` and `hakios: host_info` are **0** for the
  whole run → the extension **never reaches host_info**.
- Side observations, not the extension:
  - `com.iosvm.reg [12]: xpcproxy exited due to exit(78)`;
  - `Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 24`.
  These are **not** the widget extension (pid 48/49), so they do not explain its
  missing CheckIn by themselves.

## 1.2 What ext8 did and did not prove

- ext8 proves the **kernel host port/MIG and host_info are healthy** for the system
  processes it observed (35 hostport, 15 host_info, all ret=0).
- ext8's events are at 117–166 s, **before** the first extension (~622 s), so it does
  **not** observe an extension call.  ext9 then shows the extension itself makes
  **zero** hostport/host_info calls.
- Therefore: **server healthy; extension never gets to the call**.

## 2. Corrected conclusion

1. **ext3's "stuck in host_info" is a transient state of that boot**, not the
   universal root cause.  It cannot be used as the single-path explanation.
2. **Current main symptom**: after spawn/INIT/running, the extension **never sends
   CheckIn and never reports hanging**.  This is a silent no-CheckIn, not a
   host_info block in the general case.
3. **Kernel host port / MIG / host_info are healthy** (ext8 + ext9's zero traffic
   does not contradict it; the extension just never calls).
4. **Not explained yet**: exit 78 (xpcproxy/com.iosvm.reg), CheckIn prerequisites,
   and the `EXC_CORPSE_NOTIFY error 5` condition (pid 24 is not the extension).
5. **Not timeout/watchdog**: ext2/ext9 removed timeouts; no hanging; no kill.

## 3. Unresolved questions (handoff list)

1. **exit 78 source**: which job/process, why `xpcproxy` exits 78, and whether it is
   on the extension's bootstrap path (or only a side service).
2. **CheckIn prerequisites**: what must be true before an extension sends CheckIn
   (bootstrap namespace, MachServices endpoint, xpcproxy elision, sandbox,
   entitlement/persona, host port setup)?  Which one is missing at spawn?
3. **EXC_CORPSE_NOTIFY error 5 conditions**: what triggers it for pid 24; why is it
   not the extension pid 48/49; does it affect the shared xpc/launchd state?

## 4. Next experiments (not run in stage 22)

### 4.1 ext10: attach the probe to a real RC path

- ext9's probe **did not run**: `RC=rc.vtop` did not execute the `rc.jobs` demo job
  (`hostinfo:` 0, though the probe binary and `/etc/rc.jobs` were in the image).
- ext10: put `hostinfo_probe` directly into the RC command flow that `rc.vtop`
  actually executes (or the job it does run), then boot once.
- The probe is now a **positive control** (host port healthy); it is not the main
  diagnosis.

### 4.2 Extension-side context checks (no boot)

- `codesign -d --entitlements :-` on the extension binary vs an init helper;
- extension job plist: `POSIXSpawnType`, `MachServices`, sandbox profile,
  `EnvironmentVariables`;
- `xpcproxy` environment and bootstrap namespace (`user/501` vs `system`);
- extension sandbox profile and persona type (persona 6 is missing globally;
  `kpersona_find_by_type` errors).

### 4.3 Kernel trace points (default-on)

- `hakios: hostport msg id=%d` (id 200–245; `id=200` is host_info);
- `hakios: host_info enter/ret`;
- if an extension is stuck with **no** hostport `id=200`, the request never left
  user space — focus on its bootstrap/port context, not the kernel server.

## 5. Infrastructure gained

- Kernel build pipeline: cctools ld64 + delete intermediates + high-vmaddr/entry
  self-check + payload regeneration; `tools/kernel-rebuild.sh [marker...]`.
- Default-on `hostport` + `host_info` traces in kernel
  `1f8c489f2ca93043d53b20aa04739109`; no boot-arg needed (boot args can truncate:
  `cmdline: 262 of 607`).
- `symstack.py`, `gate.sh`, `gate_names.py` in the harness; ext3 stack map in
  `docs/s22-ext-03-symbolized.txt`.
- **Do not use the no-UI temporary image**
  (`PREBUILT=1 GL=1 GLREMOTE=1 CONFIGD=1` + `EXTRA/RC_JOBS`: `ui=0s`,
  guestframes 3–4) for conclusions; ext9 used the standard recipe.

## 6. Not doing

- no more timeout/job/watchdog changes;
- no more boot-arg guessing (traces are default-on);
- no host GPU driver changes;
- no claim P2/P3 is fixed: CheckIn never happened;
- no single-path "stuck in host_info" claim for the general case.
