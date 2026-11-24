# stage 56: the launchd timeout — source found, disabled by boot-arg, and the next blocker exposed

> Status: **measured; partial success.** t79 located the ~5 s extension kill in `launchd.darwin`:
> it is the **default launch watchdog** (`job+0x334` = 5 s, data at `0x10006d790`), armed at
> `0x10001b4f8-0x10001b51c` and firing at `0x100021314`; the per-job `_LaunchWatchdogTimeOut` is
> read only from the job's **XPCService** dictionary, the `limits.plist` `ExtensionWatchdogTimeout`
> is structurally ignored (that file applies only nine rlimit keys), and the global `ExtensionWatchdog`
> comes from the embedded `__config` with no timeout. The minimal zero-image change is the boot-arg
> **`launchd_disable_all_timeouts=1`**. t80 booted it for 3,600 s: **`hanging on launch` 0 /
> `EXTENSION_CHECK_IN_TIMEOUT` 0** (t77: 60/20), **39 appex exec**, **seven `ca-inject: loaded`**
> lines (backboardd + GradientPosterExtension 106, CollectionsPoster 107/131, AppStoreWidgetsExtension
> 117, WeatherWidget 136, CalendarWidgetExtension 150), and the appexes stay **running**. But the
> appexes **still do not query a Metal device** (the only `is shown the device` line is backboardd),
> the host has **one client** (backboardd), there is **no 1125×2436 texture**, and the desktop is
> unchanged (≈62 % main-area black on the captured desktop frame; the t80 report lists 65.5 % —
> widgets still 100 % white, Dock 66.3 / Search 13.6). **Blocker #1 (the 5 s kill) is solved;
> blocker #2 (the appex never calls `MTLCreateSystemDefaultDevice` / the plug-in hook is not hit)
> is now exposed.** Delivery root `a4a6e2ce…`/system `49ae3cf4…` were restored at 17:28 and checked;
> kernel `f62c3d4b`. Branch `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage55-multi-client-host.md`](stage55-multi-client-host.md).

## 1. The question

Stages 53–55 closed the Info.plist watchdog and the single-client host, but the poster appex was
still killed ~5 s after spawn. t79 asked **where the 5 s comes from** and **whether launchd itself
can be told to stop timing out extensions** without changing any image; t80 tested the resulting
boot-arg with the multi-client host and variant #4 root.

## 2. The source of the 5 s (t79, read-only)

### 2.1 Default launch watchdog

`system.apfs:/sbin/launchd.darwin` (525,088 B) has a default value **5** at data `0x10006d790`
(u32, file offset `0x6d790`). On job creation the code loads it and stores it at **`job+0x334`**
(`0x100005114-0x100005118`). The timer is armed at **`0x10001b4f8-0x10001b51c`**: it reads
`[job+0x334]` in seconds, multiplies by `0x3b9aca00` (1e9) to nanoseconds, and on expiry calls
**`0x100021314`**, which prints `"Extension is hanging on launch. Killing."` (`0x100021388`) and
`"killing extension hanging on launch"` (`0x10002139c`), with exit reason
`"extension check-in timeout"` (`0x10001d0d4`, `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`).

### 2.2 The per-job override only comes from XPCService

`_LaunchWatchdogTimeOut` (string `0x10004fa2b`) is parsed from the job plist's **XPCService
dictionary** by the code at **`0x1000059c0`** (the same parser as `_VoluntaryExitTimeOut` /
`_ThrottleInterval`). An appex's Info.plist has no XPCService, so it gets the 5 s default. Adding
XPCService to the six appex plists is exactly what broke poster spawn in stage 53 (t74), so this
path is closed.

### 2.3 `limits.plist` is structurally ignored

`/private/etc/com.apple.xpc.launchd.limits.plist` is read at `0x100042010`, applied through
`_xpc_dictionary_apply_f` (`0x1000420f4`) to the generic setter **`0x10003a348`**. That setter
`strcmp`s a **nine-key table** (`0x10006d6a0`): `cpu`, `filesize`, `data`, `stack`, `core`, `rss`,
`memlock`, `maxproc`, `maxfiles` — resource rlimits. Every other key is silently ignored, so the
delivery system's `ExtensionWatchdog: true` / `ExtensionWatchdogTimeout: 120` in that file never
takes effect. The P2e limits file is ineffective **by structure**, not by a wrong value.

### 2.4 The global `ExtensionWatchdog` has no timeout

`ExtensionWatchdog` (`0x10005f1d1`) and `ExtensionWatchdogTimeout` (`0x10005f1e3`) are read only
from the embedded `__TEXT,__config` (read code `0x10004135c-0x1000413a0`; key parsing
`0x10004196c`/`0x100041988`). `__config` (VMA `0x10006085f`, `0x2056 B`) is an XML plist with
`{SIGTERMTimeout:3, CrashOnSIGTERMTimeout:true, ExtensionWatchdog:true, SystemRoots:[…],
Bootstrap:{…}, HighWatermark:50, Boot:{…}}` — **no `ExtensionWatchdogTimeout`**, so the global
path also falls back to 5 s.

### 2.5 NSExtension, per-job overrides, signatures

* The appex `NSExtension` has only `NSExtensionPointIdentifier`/`EXAppExtensionAttributes`; launchd
  does not read `NSExtensionAttributes` timeouts, and `extensionkitservice` has no timeout/checkin/
  watchdog strings — the check-in timer lives entirely in launchd.
* The original Darwin `/System/Library/xpc/launchd.plist` has 421 `AppExtensions` entries, **none**
  with `_LaunchWatchdogTimeOut` (10 have XPCService with only `JoinExistingSession`); the DB is
  omitted by design in this port (`docs/ru/notes.md:970`), and its entries carry only
  `_executablePath`/`_infoPlist`.
* A same-label `/Library/LaunchDaemons` plist does not create an extension job (and conflicts with
  the dynamic job — the PosterBoard error 17 from t56); `launchd.darwin` has no signature/
  SecStaticCode strings (kernel AMFI is NOT_ENFORCING), so the boot-arg path has no signing hurdle
  and touches no file.

### 2.6 The boot-arg mechanism and its risk

`launchd_disable_all_timeouts=1` is parsed by launchd itself at **`0x100041b00`** from
`kern.bootargs` (global `[x23+0x5d0]`, reads `0x10004135c`/`0x100012cc8`), matching the prefix
`launchd_disable_all_timeouts=` and `strtoul`-ing the value. On a hit it sets `0x10006d78c=600`,
the **watchdog-enable flag `0x10006e950=0`**, `0x10006d794=0`, `0x10006f000=0`. The flag is checked
at the arm site (`0x10001b50c`: skip if 0) and in dumpstate (`0x10001e4b4` prints
`launch watchdog timeout = (globally disabled)`), so extensions are no longer killed at 5 s.

* No image/plist change; it avoids the stage-53 XPCService regression entirely.
* The port already documents it (`docs/ru/notes.md:361`), and the guest already accepts custom
  cmdline args (`metal=1 applog=1 applog_name=…` in t77/t80).
* **Risk:** it disables **all** launchd timeouts (SIGTERM/shutdown, stuck jobs), so it is acceptable
  only for an experimental boot; a production delivery would need a narrower per-job switch.

## 3. The verification boot (t80)

### 3.1 Boot

Tag `wp4-d1d2-i`, 2026-11-24 16:28:40 → 17:28:40 (**3,600 s**), SMP=1/cpus=1, 35 B clients,
0xFF vars, lavapipe pinned, kernel `f62c3d4b`; cmdline includes
`launchd_disable_all_timeouts=1 applog=1 applog_name=…` (serial boot-args line). **0 panic,
6,039 frames**; desktop idle. Host load normal.

### 3.2 The appexes survive

| observation | t80 (boot-arg on) | t77 (boot-arg off) |
|---|---|---|
| `hanging on launch` | **0** | 60 |
| `EXTENSION_CHECK_IN_TIMEOUT` | **0** | 20 |
| appex exec | **39** | 31 |
| poster appex exec | **3** | 2 |
| `ca-inject: loaded` | **7**: backboardd + GradientPosterExtension 106, CollectionsPoster 107/131, AppStoreWidgetsExtension 117, WeatherWidget 136, CalendarWidgetExtension 150 | backboardd + 112/113 |
| appex lifetime | **running** (GradientPosterExtension 106 `job state = running`; no kill line) | killed after ~5 s |

The boot-arg is effective: the six appexes load `libcainject` and are **not** killed by the launch
watchdog.

### 3.3 But no Metal device, no second client, no wallpaper

* **appex `is shown the device` = 0** — the only `is shown` line is backboardd; the three/five
  not-shown lines are mediaserverd, SpringBoard, chronod (and FitnessWidget/MapKit Snapshot in this
  boot). No appex ever asks for a Metal device.
* **Host: `client connected (1 client(s))`, `hello` = backboardd only** — no second client; the
  multi-client host is ready but unused.
* **No 1125×2436 / ≥1000×2000 texture** (0); 8 textures total, all ≤750×1624.
* The missing-pipeline report in this boot is **0 pipelines / 0 skipped draws** (`t80-missing.txt`,
  sha `b94e955e…`); t77's `PBGRA_Tsb3Xf_Isrc` + `single_pass_blur_3_lpf` is **not reproduced** here
  (the t80 report text lists one; the saved report is 0). This does not affect D1/D2.

### 3.4 Desktop

| item | t80 | baseline | target |
|---|---|---|---|
| main-area black (captured desktop frame) | **61.8 %** (t80 report: 65.5 %) | 64.9 % | <20 % ❌ |
| widget bbox1/bbox2 | black 0 % / **white 100 %** | pure white | not pure white ❌ |
| Dock / Search / orientation | 66.3 / 13.6 / normal ✓ | same | no regression ✓ |

Stage 1 (BootUI) is 98.7 % black; stage 2 is 59.0 %; stage 3 is a bright 14.5 % transition frame and
is not the desktop representative. The desktop content is unchanged: black wallpaper, white widgets.

### 3.5 Verdict

**Partial success.** The boot-arg solved the first blocker exactly as t79 predicted (zero hanging/
timeouts, six appexes injected and running). It did **not** change D1/D2: the appexes never call
`MTLCreateSystemDefaultDevice`, never become a wire client, and never produce a wallpaper texture.
The next blocker is no longer launchd; it is the **appex's Metal initialization/scene handshake**
(or the plug-in's hook coverage).

## 4. What this establishes and what it does not

**Establishes**

* The 5 s kill is launchd's **default launch watchdog**, and the **only practical lever** found is
  the boot-arg `launchd_disable_all_timeouts=1`; it works with **zero image changes** and removes
  the kill (hanging/timeouts 0, appexes running).
* The previous plist levers are structurally dead: the per-job key requires an appex XPCService
  (which breaks spawn), and `limits.plist` only applies nine rlimit keys.
* With the kill removed, **six appexes load `libcainject` and stay alive** — the inject.conf/clients
  setup is stable.
* The next blocker is the **appex's own Metal path**: no `MTLCreateSystemDefaultDevice` call, no
  wire client, no wallpaper texture.

**Does not establish**

* **Why** the appex does not call the Metal API (scene host not asking, appex rendering only via CA,
  or the plug-in hook not covering the entry point).
* **That a surviving appex would produce a wallpaper** if it did query a device; the host's
  multi-client path has still not been exercised by a real appex.
* **A safe production setting.** The boot-arg disables all launchd timeouts; it is an experiment-only
  switch.
* **A content fix.** The desktop remains black/white (61.8 % black on the captured frame; the report
  lists 65.5 %).
* **A rate.** n=1 boot for the boot-arg; the appex exec counts vary between boots.

## 5. Next steps

1. **Unconditional logging in the plugin**: add log lines to `MTLCreateSystemDefaultDevice` and
   `MTLCopyAllDevices` (not gated by the clients check) so the next boot proves whether an appex
   calls them at all.
2. **Capture appex-internal logs**: `applog=1 applog_name=…` currently produces no appex stderr;
   verify the capture path (kernel tunable/console) so the appex's own SwiftUI/scene errors are
   visible.
3. **Check the PosterBoard → appex scene handshake**: the appex may be waiting for the scene host/
   snapshot request; if so, the Metal init happens only after that message.
4. **Keep the boot-arg for experiments only**; record it as an experimental boot flag, not a
   delivery setting. The multi-client host remains in place for the first appex `hello`.

## 6. Evidence and hashes

| item | value |
|---|---|
| boot-arg | `launchd_disable_all_timeouts=1` (cmdline line in serial) |
| t80 host / root / system / kernel | host `b0bf8f76…`; variant #4 root `899bd711…`; delivery system `49ae3cf4…`; delivery root restored `a4a6e2ce…`; kernel `f62c3d4b…` |
| t80 report | `/tmp/t80-evidence/T80-REPORT.md` sha256 `0e91b676cd7c27bfc35011a367b73c304eae42152c56c9d69e6609d538c95c1c` |
| t80 serial / host / missing / monitor | serial `ddf06b0ea28df0544942d0ce3ebb8406d05e8adce3a47693c17038e41e8631c3` (14.2 MB); host `2c4b04c4…`; missing `b94e955e…`; monitor `2297bf57…` |
| t80 captures | desktop `ae3ff375…` + thumb `32fc54ab…`; stage1 `71a943d7…`, stage2 `aecaffa7…`, stage3 `f5e447d3…` |
| t79 evidence | `launchd.darwin` 525,088 B; addresses/keys in §2; `docs/ru/notes.md:361`, `docs/ru/notes.md:970` |
| t77 comparison | serial `ff1905af…`; host `cc50144c…`; hanging 60/timeouts 20 |

### 6.1 Repository gates

`make test` is green at this tree: provenance clean (494 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 7. Files changed in this record

- `docs/stage56-launchd-timeouts.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows gain the boot-arg result and the new blocker.
- `README.md` — the stage-56 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the variant images and the
boot-arg live outside the repository.

## 8. Reproduce

```sh
# t80: the kill is gone; the device query is not
f=mt-work/logs/wp4-d1d2-i/serial.log
grep -a -c 'hanging on launch' $f          # 0
grep -a -c 'EXTENSION_CHECK_IN_TIMEOUT' $f # 0
grep -a -o 'ca-inject: loaded as pid [0-9]* ([^)]*)' $f | sort -u
grep -a 'is shown the device' $f           # backboardd only
grep -a 'client connected\|hello' /tmp/t80-metalwire-host.log
# missing report (saved): 0 pipelines
cat /tmp/t80-missing.txt
# restore (already done)
md5sum mt-work/hakios/userland/root.apfs   # a4a6e2ce50e6d0324e26a49ea20fbd84
```
