# stage 54: the appex regression — the system Info.plist is the cause, and the watchdog direction is closed

> Status: **measured; the stage-53 regression is localized and the watchdog direction is closed.**
> The control boots separate the three variables: **A** (variant system `fd1d6fc7…` + delivery root
> `a4a6e2ce…`) has **poster appex exec = 0** at 1,800 s and again at 3,600 s; **B** (delivery system
> `49ae3cf4…` + variant root `899bd711…` = t68) has the poster appexes exec normally
> (GradientPosterExtension pid 129 + CollectionsPoster pid 130, ~53 min after boot). With t71
> (variant system + variant root) also at 0, **the cause is the six appex Info.plist
> `XPCService` changes in the variant system image**; the root changes and boot-window variance are
> excluded. The watchdog direction is therefore **closed**: `XPCService._LaunchWatchdogTimeOut` did
> not extend the appex lifetime and made launchd stop spawning the poster appexes. The next path is
> **host multi-client + a fast handshake** (`mw_wait_for_client()` serves one client; the hello ack
> is 15 s; the appex window is ~5.6 s). Delivery system/root were restored at 13:35 and checked;
> kernel `f62c3d4b`. Branch `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage53-appex-watchdog.md`](stage53-appex-watchdog.md).

## 1. The question

Stage 53's Info.plist watchdog variant was invalid and raised a regression warning: the poster
appexes went from "exec + killed after 5.63 s" (t68) to "no exec at all" (t71). Was that caused by
the six `XPCService` additions in the **system** image, by the **root** changes (inject.conf,
clients, dedup), or by the shorter/longer boot window? t74 ran the control A/B and a
window-aligned repeat.

## 2. The warning being tested (t71 recap)

* Variant system + variant root, 3,600 s: CollectionsPoster/GradientPosterExtension **0 exec**;
  widget appexes 27 exec but no `ca-inject`; no appex device line; host one `hello`; no 1125×2436
  texture; desktop 63.2 % black, widgets 100 % white; hanging 46 / timeouts 11.
* The system image differs from delivery only by six appex Info.plists that add
  `XPCService = {_LaunchWatchdogTimeOut = 120}` (plus the reused variant #4 root).

## 3. Control A/B (t74)

### 3.1 A — variant system + delivery root, 1,800 s

Tag `wp4-d1d2-f`, 11:59:09 → 12:29:09 (1,800 s), 0 panic, **4,858 frames**. **Poster appex exec 0**
(CollectionsPoster/GradientPosterExtension appear only in cmdline/inject.conf text), all appex exec
6, `ca-inject` only pid 20 (backboardd) ×2, host `hello` 1, hanging 10 / timeouts 1. PosterBoard
itself execs and uncorks; the widget appexes are the only ones that run.

### 3.2 A2 — same combination, 3,600 s window-aligned

Tag `wp4-d1d2-f2`, 12:30:35 → 13:30:35 (3,600 s), 0 panic, **11,192 frames** (the t74 report lists
10,242, an earlier count; the final host log's `client disconnected after 11192 frame(s)` is used
here). **Poster appex exec 0** at the full 60-minute window, all appex exec 33, `ca-inject` only
pid 20 ×2, host `hello` 1, hanging 42 / timeouts 9. The window is now identical to t68/t71, so
"too short" is excluded.

### 3.3 B — delivery system + variant root (= t68), 3,600 s

Tag `wp4-d1d2-d`, 09:09:23 → 10:10:03 (3,600 s), 0 panic, **3,686 frames**. Poster appexes **do
exec**: GradientPosterExtension pid 129 and CollectionsPoster pid 130 at ~53 min after boot;
GradientPosterExtension loads `libcainject` (pid 129) and is killed 5.63 s later; all appex exec 54,
`ca-inject` pid 20 ×2 + pid 129, host `hello` 1, hanging 80 / timeouts 21.

### 3.4 The three-way table

| | **A** (1,800 s) | **A2** (3,600 s) | **B = t68** (3,600 s) |
|---|---|---|---|
| system / root | **variant `fd1d6fc7`** / delivery `a4a6e2ce` | **variant `fd1d6fc7`** / delivery `a4a6e2ce` | delivery `49ae3cf4` / **variant `899bd711`** |
| 0 panic / frames | 0 / 4,858 | 0 / 11,192 | 0 / 3,686 |
| **poster appex exec** | **0** | **0** | **✓** (pid 129/130, ~53 min) |
| all appex exec | 6 | 33 | 54 |
| `ca-inject: loaded` | backboardd ×2 | backboardd ×2 | backboardd ×2 + pid 129 |
| host `hello` | 1 | 1 | 1 |
| hanging / timeouts | 10 / 1 | 42 / 9 | 80 / 21 |

### 3.5 Conclusion

1. **Regression cause = A**: whenever the system is the variant (six Info.plist `XPCService`
   additions), the poster appexes **never exec** — A, A2 and t71 are three independent runs with
   the same result. With the delivery system (B = t68), the poster appexes exec normally.
2. **Root excluded**: B uses the same variant root as t68 and the poster appexes still exec; the
   inject.conf/clients/dedup changes do not affect poster spawn.
3. **Variance excluded**: A2 runs the same combination for the full 3,600 s window and still sees
   **0** poster exec, while B sees them at ~53 min.
4. **Watchdog direction closed**: adding `XPCService._LaunchWatchdogTimeOut` to the appex
   Info.plist did not extend the lifetime (the poster appexes never started) and broke launchd's
   spawn path. There is no point in further Info.plist watchdog variants.

The exact mechanism is not proven; the only system-side difference is the six plists, and the
likely candidates are launchd's extension-job parsing/validation and the invalidated
`_CodeSignature/CodeResources`. The control result is enough to stop using this lever.

### 3.6 Screenshots (A)

| item | A | baseline | target |
|---|---|---|---|
| main-area black (desktop signature frame, stage 2) | **61.8 %** | 64.9 % | <20 % ❌ |
| widget bbox1/bbox2 | black 0 % / **white 100 %** | pure white | not pure white ❌ |
| Dock / Search / orientation | 66.3 / 13.6 / normal ✓ | same | no regression ✓ |

Stage 3 (steady) is 59.0 % black (screen-off/dim); stage 1 (BootUI) is 98.7 %. A2 did not run the
watcher/screenshot; A and A2 have the same combination and result, so A represents the group.

## 4. What this establishes and what it does not

**Establishes**

* The stage-53 poster-exec regression is caused by the **system image's six appex Info.plist
  `XPCService` changes**, not by the root changes and not by the boot window.
* The **watchdog direction is closed**: the job-level `XPCService._LaunchWatchdogTimeOut` neither
  extends the appex lifetime nor keeps launchd spawning the poster appexes.
* The variant #4 root (inject.conf + clients + dedup) is **compatible with the delivery system**:
  with the delivery system, the poster appexes exec and GradientPosterExtension is injected.
* The desktop did not regress in A (61.8 % black, widgets white, Dock/Search normal).

**Does not establish**

* **The exact mechanism** of the launch failure (launchd parsing vs code signature vs another
  plist semantic). It is enough to abandon the lever, not to explain it.
* **That a multi-client host will fix D1/D2.** The appex still needs to check in within ~5.6 s,
  query a Metal device, and connect the wire; none of that has been shown yet.
* **A rate.** A/A2 are two boots of the same combination plus t71; B is t68 (one boot).
* **A2's final frame count** beyond the host log (the report's 10,242 vs the final 11,192 is noted,
  not hidden).
* **A delivery change.** The variant system is experimental; `CodeResources` was not re-signed,
  secure surfaces may differ, and no V100/isolation core is involved.

## 5. Next: host multi-client + fast handshake

The remaining blocker is the wire's single-client design:

* `metalwire_host.c:5878-5912 mw_wait_for_client()` accepts **one** client and serves it until it
  disconnects; backboardd holds the wire after boot.
* The appex's connect/hello waits for `MW_DEFAULT_ACK_MS = 15000` (`mw_wire.h:30`), while the
  launchd extension watchdog kills the appex in **~5.6 s**.
* Even with the variant #4 root (inject.conf + clients) and the delivery system, the appex would
  die while queued behind backboardd.

The plan (t76 code, t77 experiment):

1. **Host multi-client** (or a dedicated single-appex slot): accept backboardd and an appex
   concurrently, without making either wait for the other's ack.
2. **Fast handshake**: shorten the appex's connect/hello path so it can complete inside the ~5.6 s
   window.
3. Keep the **variant #4 root + delivery system** combination (the poster appexes exec there and
   GradientPosterExtension is injected); do **not** use the variant system.
4. **Acceptance gate unchanged**: a second host `hello` whose program is the appex, before looking
   at wallpaper pixels.

## 6. Evidence and hashes

| item | value |
|---|---|
| A / A2 / B images | A: variant system `fd1d6fc7…` + delivery root `a4a6e2ce…`; A2: same; B: delivery system `49ae3cf4…` + variant root `899bd711…` |
| variant system / root | `fd1d6fc7037e66b5407c5176cd1e19ea` (sha256 `04efb3c8…`) / `899bd71181b8cec1847181a7658cb42a` (sha256 `44b1ed74…`) |
| delivery system / root (restored) | `49ae3cf4fd53334b475a921eb9915008` / `a4a6e2ce50e6d0324e26a49ea20fbd84` |
| kernel | `f62c3d4b852d467f22c4703b8fc620c9` |
| t74 report | `/tmp/t74-evidence/T74-REPORT.md` sha256 `0d467f1e3ec2f4cddd3db136f77b17ba702fbaf0406ef786e4b3036f80f8aa8a` |
| serial-A / A2 | `d0558810a59920bfd5ad109be5de1ab20e469fcd798e0cf603172f8255449fd4` (10.9 MB); `a4477df9e5bb44560236e11bf06c111ea28d6132c31dc32190b2aeb3096cfc9a` (23.7 MB) |
| host-A / A2 | `502a8ea4…` / `80a7e99e…` |
| monitor-A / A2 | `70beee04…` / `7f31d4d9…` |
| captures | stage1 `71a943d7…`, stage2 `5b38af71…`, stage3 `7300dee1…`, thumb `52f3ebbb…` |
| t68 / t71 reference | t68 serial `d06556b0…`, report `fb3354b1…`; t71 serial `3d0d3914…`, report `e171bcc2…` |
| host / glwire | `ad778a06…` / `5b28249a…` |

### 6.1 Repository gates

`make test` is green at this tree: provenance clean (492 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 7. Files changed in this record

- `docs/stage54-appex-regression.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows gain the regression localization and the watchdog
  closure.
- `README.md` — the stage-54 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the variant images and the
staging tree live outside the repository.

## 8. Reproduce

```sh
# A / A2 poster appexes: 0
for tag in f f2; do
  grep -a -c 'exec pid=.*CollectionsPoster\|exec pid=.*GradientPosterExtension' \
    mt-work/logs/wp4-d1d2-$tag/serial.log
done
# B (t68) poster appexes: pid 129/130
grep -a 'exec pid=129 path=.*GradientPosterExtension\|exec pid=130 path=.*CollectionsPoster' \
  mt-work/logs/wp4-d1d2-d/serial.log
# A/A2 host: one hello, backboardd
grep -a 'hello' /tmp/t74-f-metalwire-host.log /tmp/t74-f2-metalwire-host.log
# restore (already done)
md5sum mt-work/hakios/userland/system.apfs   # 49ae3cf4fd53334b475a921eb9915008
md5sum mt-work/hakios/userland/root.apfs     # a4a6e2ce50e6d0324e26a49ea20fbd84
```
