# stage 51: the appex Metal client entry — the clients file was the wrong gate

> Status: **measured; the third D1/D2 experiment is negative.** t63 reframed the previous round:
> `libcainject` is not the wire client at all — the Metal wire is `HakiosMetal.bundle`, gated by
> `/redwood/lib/HakiosMetal.clients`, and the poster/widget appexes were not listed there. It also
> showed that the ~9 s kill is launchd's **default extension check-in watchdog**, not the
> `_LaunchWatchdogTimeOut` key in `/xpc/extensions.plist`. Variant #3 added six appex names to the
> clients file (and kept the PosterBoard label dedup) and booted with `applog=1
> applog_name=…`. The 3,600 s boot shows the appexes **do exec (19 times)**, but
> **`ca-inject` loaded only into pid 20 (backboardd)**, **no appex ever asked for a Metal device**
> (one `is shown the device` line for backboardd; three `is not shown` lines for
> mediaserverd/SpringBoard/chronod; none for an appex), the host still has **one `hello`
> (backboardd)**, **no 1125×2436 texture**, the wallpaper is still black (**61.8 %** main-area black
> vs 64.9 % baseline) and the widget bboxes are still **100 % white**. The reason is structural:
> **the clients file is checked after the plug-in is loaded**, and this image's `inject.conf` is
> still the delivery 304-byte version (backboardd only) — the plug-in never enters the appex, so the
> clients gate is never reached. The single-client wire blocking was **not triggered** (the appex
> never connected). Delivery root `a4a6e2ce…` was restored at 09:04 and checked; kernel is
> `f62c3d4b`. Branch `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage50-poster-appex-wire.md`](stage50-poster-appex-wire.md).

## 1. The question

Stages 49–50 fixed the image-side defects (watchdog key, duplicate PosterBoard label) and then
added the appex names to `/etc/inject.conf`, but the appex still never appeared on the wire. t63
asked a different question: **what actually creates the Metal wire client in this guest?** The
answer changed the target: the gate is `HakiosMetal.clients`, not `inject.conf`. Variant #3 tested
that gate by adding the appex names to the clients file and enabling the kernel's appex logging
(`applog=1`), with a 3,600 s decision boot.

## 2. What t63 established (read-only)

### 2.1 `libcainject` is not the wire client

`libcainject` is backboardd's diagnostic plug-in: it logs metallib resource lookups and provides a
placeholder URL. The Metal wire client is **`HakiosMetal.bundle`**, loaded according to
`/redwood/lib/HakiosMetal.clients` (docs/zh/stage29-metal-translation.md:50-61,
docs/stage30-ca-metal.md:38). Adding appex names to `inject.conf` therefore could not make an appex
a wire client even if the plug-in loaded; the clients file is the actual admission list.

### 2.2 The clients file and the `metal=1` rule

`HakiosMetal.m:6802-6820 mw_listed_in_file()` reads the file line by line (`fgets`), cuts at the
first space/tab/CR/LF and matches with `strcmp` (`*` = all). `HakiosMetal.m:6876-6910
mw_process_allowed()` under **`metal=1`** (the boot cmdline here) falls through to the clients-file
check for processes other than `metal_test`; `metal=2/3` short-circuits and bypasses the file. The
delivery clients file has three lines (`metal_frame`, `metal_probe`, `backboardd`); no appex is
listed, so no appex is allowed to create the translated Metal device.

### 2.3 Why the previous boot saw no `ca-inject:` appex lines

The kernel does inject by exec basename: `exec_inject_env()` (`kern_exec.c:5710-5759`) matches
`cn_nameptr`/`cn_namelen`, and the call site (`kern_exec.c:5933-5934`) is unconditional; `init`
reads `/etc/inject.conf` (`init.c:1449-1490`) and the t61 serial line 871 shows the appex line in the
config. But `cai_log()` writes to **fd 2** (`guest/ca-metal-inject/ca_inject.m:43-68`); backboardd's
LaunchDaemon plist sets `StandardErrorPath=/dev/console`, so its stderr reaches the serial, while an
appex's dynamically generated job sends stderr to `/dev/null`. The missing `ca-inject:` line is
therefore a **visibility** problem, not proof that the injection failed — and in any case
`libcainject` is not the wire client.

### 2.4 The ~9 s kill is launchd's default extension watchdog

The appex path is launchd → `xpcproxy` → `extensionkitservice` → appex. launchd kills it with
`Extension is hanging on launch. Killing.` / `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`. The
watchdog value comes from the **job's `XPCService` dictionary** (launchd strings: `XPC Service
Info.plist is missing XPCService dictionary`, `launch watchdog timeout = %u`, `(globally
disabled)`); an appex's Info.plist has only `NSExtension`, no `XPCService`, so it gets the **default
extension watchdog**. The `_LaunchWatchdogTimeOut` key that t56 put in the extension-point config in
`/xpc/extensions.plist` is not read for a job's timeout — which is why the 120 s variant still saw
6.2–52.1 s kills (t59). Ways to change it: an appex `Info.plist` `XPCService` with
`_LaunchWatchdogTimeOut`, or the global `ExtensionWatchdog`/`ExtensionWatchdogTimeout` in
`/System/Library/xpc/launchd.plist` (that file is missing by design in this image; see
docs/ru/notes.md:970). Entitlements do not control this window.

### 2.5 What t63 changed in the plan

Instead of more `inject.conf` attempts, the next variant must target the **real wire gate**
(`HakiosMetal.clients`) and make appex stderr visible (`applog=1 applog_name=…`). It also has to
respect two constraints: the host accepts **one client at a time** (`metalwire_host.c:5878-5912`,
`mw_wait_for_client()`), and the appex check-in window is ~9 s.

## 3. Variant #3 (t64)

### 3.1 Changes

`mt-work/exp-d1d2c/root-d1d2c.apfs` (copy `/tmp/d1d2c-root.apfs`) is the delivery root image with
two changes:

1. `/lib/HakiosMetal.clients`: sha256 `92482275…` (35 B, 3 lines) → `4295fcb8…` (154 B, 9 lines):
   ```
   metal_frame
   metal_probe
   backboardd
   CollectionsPoster
   GradientPosterExtension
   WeatherPoster
   WeatherWidget
   CalendarWidgetExtension
   AppStoreWidgetsExtension
   ```
2. `LaunchAngels/com.apple.PosterBoard.plist` deleted (sha256 `dfc25261…`), keeping the
   LaunchDaemons copy (`89784260…`). `/xpc/extensions.plist` stays the delivery version
   (`e92ee2b8…`); no appex Info.plist is changed. The other 481 files' sha256 match delivery; 502
   entries read back; `apfsck -cw: clean`. The delivery root was never touched
   (`a4a6e2ce…`).

### 3.2 Risks

* **The host is single-client.** `mw_wait_for_client()` accepts one client and serves it until it
  disconnects; backboardd holds the wire after boot. An appex connection would sit in the backlog
  and wait for the hello ack (`MW_DEFAULT_ACK_MS = 15000`, `mw_wire.h:30`; connect 2000 ms), far
  longer than the ~9 s check-in window. If the appex ever connects, it may be killed while waiting;
  the host needs multi-client support or a single-appex slot (an iosvm-metal change, outside t64).
* **Sandbox**: the guest sandbox is a stand-in; a profile could refuse TCP to 10.0.2.2:9111 (the
  applog would show the connect error).
* **API surface**: the appex may use Metal APIs backboardd does not (`CAMetalLayer`/`MTKView` in
  process), with crash risk.
* **Secure surfaces**: the poster appex carries `com.apple.QuartzCore.secure-mode`; a secure CA
  context may not be supported by the host path.
* **No Metal entry**: if the appex never calls `MTLCreateSystemDefaultDevice` (pure CA rendering),
  the clients list cannot change the behavior — the applog device lines are the test.

The boot cmdline included `applog=1 applog_name=CollectionsPoster,GradientPosterExtension,
WeatherPoster,WeatherWidget,CalendarWidgetExtension,AppStoreWidgetsExtension` (confirmed in the
serial cmdline and launchd boot-args lines).

## 4. The decision boot (t65)

### 4.1 Boot and the disk incident

Tag `wp4-d1d2-c`, 2026-11-24 02:29:56 → 03:29:56 (**3,600 s**), SMP=1/cpus=1, 35 B clients,
0xFF vars, lavapipe pinned, delivery kernel `f62c3d4b`. **0 panic, ~5,355 frames**; the desktop
came up and idled. The host log ends at `render frame 5356` and has no clean `client disconnected`
line because the host was killed at shutdown (the `--missing-report` is 0 B for the same reason).
Host load 1.5–4, vCPU ~65–70 %, win2022 188 %.

**Disk incident:** `/tmp/t65-frames` (~37 G) plus older frame directories filled the root disk to
**100 % (318 M left)**. The observation window (02:29–03:29) was already on disk: the serial is
11.95 MB, the three stage screenshots exist (03:13), and the host log is complete through frame
5355. The cleanup restored **59 G free**. This did not affect the result; it is recorded so the
evidence can be trusted.

### 4.2 Serial: appexes exec, no Metal device

| observation | variant #3 | baseline (wp4-soak-fix3) |
|---|---|---|
| appex exec | **19** (WeatherWidget 111/117/126, GradientPosterExtension 120, CollectionsPoster 122, WeatherPoster 125, CalendarWidgetExtension 128/129, NewsToday2, RemindersWidgetExtension, FindMyWidgetPeople/Items, …) | yes |
| `ca-inject: loaded as pid …` | **only `pid 20 (backboardd)`** (27 lines, same process) | only backboardd |
| `… is shown the device` | **1 (backboardd)** | 1 |
| `… is not shown the device` | **3 (mediaserverd, SpringBoard, chronod)**; **no appex line** | 3 |
| applog lines | 4 (cmdline / launchd boot-args only); **no appex stderr lines** | — |
| `EXTENSION_CHECK_IN_TIMEOUT` | **6** | 89 |
| `hanging on launch` | **34** | 193 |

The clients file is present (154 B, 9 lines, confirmed in the image), but the plug-in is still the
delivery `inject.conf` (304 B, backboardd only), so it never enters the appex; the clients gate is
only evaluated **after** the plug-in is loaded, and the appex never queries a Metal device at all.
The appexes are still killed as hanging (34 times) within the ~9 s window.

### 4.3 Host

* **hello = 1** (`pid 20 program "backboardd"`) — no appex client.
* 7 textures, **none ≥1000×2000**; `1125x2436` **0**; `750x1624` ~10.7 k (display target only).
  No wallpaper upload/draw.

### 4.4 Desktop

| item | variant #3 | baseline | target |
|---|---|---|---|
| main-area black (desktop signature frame) | **61.8 %** | 64.9 % | <20 % ❌ |
| widget bbox1/bbox2 | black 0 % / **white 100 %** | pure white | not pure white ❌ |
| Dock / Search / orientation | 66.3 / 13.6 / normal ✓ | same | no regression ✓ |

The other desktop elements (icons, Dock, Search, direction) show no regression. Stage 2 (a
transition frame) is 59.0 % black; stage 1 (BootUI) is 98.7 %.

### 4.5 Verdict

**Invalid.** The clients-file change cannot help because it is downstream of the plug-in load, and
the plug-in is not in the appex. The chain is:

1. `inject.conf` is the delivery version (304 B, only backboardd) → `libcainject` does not enter the
   appex;
2. `HakiosMetal.clients` is checked **inside** the loaded bundle → the clients file is never
   consulted for the appex;
3. the appex never asks for a Metal device (`is shown` only backboardd) → no wire client → no
   wallpaper texture;
4. the appex is still killed as `hanging on launch` within ~9 s (launchd default watchdog);
5. the host's single-client limit did **not** trigger, because the appex never opened a connection.

### 4.6 Restore

Delivery root restored at **09:04** (file mtime 09:04:18), md5
`a4a6e2ce50e6d0324e26a49ea20fbd84`; kernel `f62c3d4b852d467f22c4703b8fc620c9` (09:04/09:05 checks).
Variant #3 stays in `mt-work/exp-d1d2c/` and `/tmp/d1d2c-root.apfs`; delivery and staging were not
modified. **Note:** at 09:08 the next experiment (t68, variant #4 `899bd711…`) installed its image
into the same `userland/root.apfs` slot (delivery `a4a6e2ce` backed up), so the 09:04 restore above
is the t65 state, not a claim about the slot after 09:08.

## 5. What this establishes and what it does not

**Establishes**

* The real wire gate is **`HakiosMetal.clients`**, not `inject.conf`; the delivery list has only
  `metal_frame`, `metal_probe`, `backboardd`, so an appex cannot get the translated device.
* The ~9 s kill is launchd's **default extension watchdog**; the extension-point
  `_LaunchWatchdogTimeOut` in `/xpc/extensions.plist` is not the job's timeout.
* The clients file is evaluated **after** the plug-in is loaded, so changing it alone cannot make
  an appex a wire client; the plug-in must also be injected.
* The host's single-client limit was not exercised; the appex never connected.
* The desktop did not regress (Dock/Search/icons/direction normal).

**Does not establish**

* **Whether the appex would use `MTLCreateSystemDefaultDevice`** if the plug-in were loaded. The
  appex never queried a device in this boot; a pure-CA appex may never do so.
* **Whether a loaded appex would reach the wire within 9 s.** The host is single-client and the
  appex check-in window is ~9 s; both would need attention.
* **A rate.** n=1 boot for variant #3; the launch counts vary widely between boots, and this
  round's 6/34 is not a fix.
* **A clean appex log.** `applog=1 applog_name=…` was in the cmdline, but the serial has only the
  cmdline/launchd boot-args lines, no appex stderr — the applog did not surface the appex output
  (or the appex wrote nothing before dying).
* **A delivery change.** The image is experimental; the `.sig` is not re-signed, and secure CA
  surfaces may behave differently. No V100, no isolation core.

## 6. Next steps

1. **Variant #4 (double lever):** change **both** `/etc/inject.conf` (add the appex names) and
   `/lib/HakiosMetal.clients` (already done), and keep `applog=1`; this is the first configuration
   that can actually exercise the clients gate. Verify the appex's `is shown the device` line before
   looking at wallpaper pixels.
2. **Give the appex room:** either add host multi-client support or reserve a single-appex wire
   slot; the current `mw_wait_for_client()` + 15 s ack cannot serve an appex alongside backboardd.
3. **Extend or bypass the check-in watchdog:** appex `Info.plist` `XPCService._LaunchWatchdogTimeOut`
   (changes launch semantics) or a validated global `ExtensionWatchdog`; the extension-point key is
   not the lever.
4. **If the appex still does not query Metal**, look at the appex's Metal entry (it may render only
   through CA) and the scene-host handshake; if it does query but produces no wallpaper, the
   appex→CA delivery path is the next branch.

## 7. Evidence and hashes

| item | value |
|---|---|
| variant #3 image | `exp-d1d2c/root-d1d2c.apfs` md5 `4567698bb6d118bf7e8c24edbc97d68e`, sha256 `138255b3c2c82f53bcf55c772ada9557bd36e33a5dc9173c550c9959f12d85a2`, 6,234,112 B; copy `/tmp/d1d2c-root.apfs` same md5 |
| clients file before/after | `92482275349dc9df164110f5d24bd9eed4eabcb55f82823c52c495ceb1e1ece7` (35 B) → `4295fcb85ae5f55c78b1c2922d6abca8b44182358ae19ae2777d59e655612868` (154 B) |
| delivery root (t65 restore; slot reused by t68 at 09:08) | md5 `a4a6e2ce50e6d0324e26a49ea20fbd84` |
| t65 report | `/tmp/t65-evidence/T65-REPORT.md` sha256 `50031cf9444d08b43c1f7ca821633e8cc8f4d34be00521ef097c98da5ffbafd4` |
| t65 serial / host / monitor | serial `ef6b858e5811f7bc9ffbad0da9f23dbea6c33fefed15827d2970879ae770bdba` (11.95 MB); host `22536269…`; monitor `2f54e5ae…` |
| t65 captures | stage1 `71a943d7…`, stage2 `20ed7e55…`, stage3 `3c8df10c…`, thumb `cc33fb28…` |
| t63 code references | `kern_exec.c:5710-5759` / `5933-5934`; `init.c:1449-1490`; `ca_inject.m:43-68`; `HakiosMetal.m:6802-6820` / `6876-6910`; `metalwire_host.c:5878-5912`; `mw_wire.h:30`; `kern_exec.c:2659-2720` (applog) |
| kernel / host / glwire / image | kernel `f62c3d4b852d467f22c4703b8fc620c9`; host `ad778a06…`; glwire `5b28249a…`; root `a4a6e2ce…`, system `49ae3cf4…`, libcainject `aa123ccd…`, plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (489 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage51-appex-metal-clients.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows gain the clients-file gate and the inject.conf
  prerequisite.
- `README.md` — the stage-51 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the variant image and the
staging tree live outside the repository.

## 9. Reproduce

```sh
# t65 evidence
grep -a 'is shown the device\|is not shown' mt-work/logs/wp4-d1d2-c/serial.log
grep -a -o 'ca-inject: loaded as pid [0-9]* ([^)]*)' mt-work/logs/wp4-d1d2-c/serial.log | sort -u
grep -a -c 'exec pid=[0-9]* path=.*\.appex' mt-work/logs/wp4-d1d2-c/serial.log    # 19
grep -a -c 'EXTENSION_CHECK_IN_TIMEOUT' mt-work/logs/wp4-d1d2-c/serial.log       # 6
grep -a -c 'hanging on launch' mt-work/logs/wp4-d1d2-c/serial.log                 # 34
grep -a 'hello' /tmp/t65-metalwire-host.log                                      # backboardd only
python3 /tmp/t65-evidence/analyze3.py mt-work/logs/wp4-d1d2-c/serial.log /tmp/t65-metalwire-host.log

# restore (already done)
md5sum mt-work/hakios/userland/root.apfs   # a4a6e2ce50e6d0324e26a49ea20fbd84
```
