# stage 55: the multi-client host — infrastructure ready, but the appex dies before it can connect

> Status: **measured; the multi-client host works, and it is not the bottleneck.** t76 rewrote
> `metalwire_host` to serve several clients at once with per-connection state and fast acks, and
> committed it as `2d3f325` (host md5 `b0bf8f76…`; 17/17 tests PASS; no performance regression).
> t77 then booted the new host with **variant #4 root + delivery system**: h1 (1,800 s) was too
> short — **poster appex 0 exec**; h2 (3,600 s) has the poster appexes exec and load `libcainject`
> (`pid 112 GradientPosterExtension`, `pid 113 CollectionsPoster`), but **launchd kills both ~5 s
> after spawn** (112: 07:18:20.97 → 07:18:25.98 = **5.01 s**) — before any `is shown the device`,
> before a second host client, and before any wallpaper texture. The host served only backboardd
> (1 client, ack 11,765, 0 wire errors), the desktop is **65.5 %** main-area black (h1 stage 2) with
> **100 % white** widget bboxes and normal Dock/Search/direction, and h2's missing report lists one
> blurred pipeline (`PBGRA_Tsb3Xf_Isrc` + `single_pass_blur_3_lpf`, 1 skipped draw). The conclusion:
> **multi-client host is necessary but not sufficient — the binding blocker is still the launchd
> extension watchdog (~5 s)**. Delivery root `a4a6e2ce…`/system `49ae3cf4…` were restored at 15:54
> and checked; kernel `f62c3d4b`. Branch `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage54-appex-regression.md`](stage54-appex-regression.md).

## 1. The question

Stage 54 closed the watchdog direction and left one infrastructure gap: the host served **one**
client (`mw_wait_for_client()`), so even an appex that survived the check-in would queue behind
backboardd and wait for a 15 s ack. t76 fixed the host; t77 asks whether the appex can now connect
inside its ~5 s window and produce a wallpaper.

## 2. The multi-client host (t76)

### 2.1 Implementation

* **Per-connection state**: a `MW_CLIENT_FIELDS` macro + `struct mw_client` moves every
  connection-owned field out of `mw_server` (client_fd, peer_version, frame, frames_done,
  pending_status, payload, buffers/images/pipelines/samplers/libraries, replaces, stats,
  completions, frames_ok, done); `mw_client_enter/leave` swaps it around message handling so the
  handlers are unchanged (`s->field` still works) and the single-client path is the old behavior.
* **Multi-client I/O loop** (`mw_run_server()`): `poll()` on the listener plus all clients;
  non-blocking accept (backlog 16, up to 8 clients); each client has its own incremental input
  buffer (non-blocking recv, EAGAIN returns); complete messages are processed in that client's
  context; `mw_send_all` polls `POLLOUT` on EAGAIN with a 10 s cap.
* **Resource isolation and release**: each client has its own buffers/images/pipelines/samplers/
  libraries (the same id 1 in two clients does not collide); `mw_client_release` frees a client's
  objects on disconnect; the **last client's state is adopted** for the exit summary/missing-pipeline
  report, so the single-client behavior is unchanged.
* **Fast handshake**: hello/frame acks are handled as soon as the client is ready; the guest's
  `MW_DEFAULT_ACK_MS = 15000` is unchanged (its own timeout); the host only waits if the socket is
  temporarily unwritable.

### 2.2 Tests

`make clean && make test` is **exit 0 (8.6 s)**: the existing 16 tests all PASS (frames/csv/shm/
files/fallback/format115/rg8/blur/labels/draw-dump/material/dual-buffer/leak-check/chain/
orientation) plus the new multi-client fixture:

```
PASS multi-client: two connections at once, each ack ok in 0.001 s, same ids stayed apart
```

The fixture holds the first client connected after its frame, then connects a second client with
the same buffer 1/texture 1 and a different image; both acks are `status=ok/crc_ok=1` (resource
isolation) and the second ack latency is **0.001 s**. The old single-client host would block the
second client until the first disconnects, so the fixture fails there.

### 2.3 Performance A/B

10 frames at 750×1624 on lavapipe:

| | round-trip mean (2 runs) | generator fps (2 runs) |
|---|---|---|
| old host | 69.268 / 72.854 ms | 14.4 / 13.7 |
| new host | 68.698 / 67.527 ms | 14.5 / 14.8 |

No regression; the difference is inside the noise.

### 2.4 Commit, md5, compatibility

* Commit `2d3f325` (`stage54: metalwire_host serves several clients at once`), 2 files,
  +479/−106 (`host/metalwire/metalwire_host.c` +460/−106, `host/metalwire/metalwire_test.py` +125).
* New `metalwire_host` md5 **`b0bf8f767d816299a864f23785dee03b`**; the old source rebuilt with the
  same flags is `d7e8192b7ae4f43b7ed677e60c4b76ad` (the old binary was removed by `make clean`).
* Wire format, message types, `struct mw_ack`, hello/ack semantics and v2–v4 handling are unchanged;
  only the host's internal state organization and I/O loop changed. Single-client clients behave as
  before.
* Limits: `--frames N` is a **global** target (all clients together); the summary/missing report
  describes the **last served** client in a multi-client run (single-client unchanged); at most 8
  concurrent clients, excess connections are refused and logged.

## 3. The verification boots (t77)

### 3.1 h1 — 1,800 s: window too short

Tag `wp4-d1d2-h`, 14:11:43 → 14:41:43 (1,800 s), 0 panic, **4,707 frames**. **Poster appex exec 0**
(the 30-minute window is shorter than t68's ~53-minute first poster exec); host has **1 client**
(backboardd); no wallpaper texture. No appex was available to test the multi-client path.

### 3.2 h2 — 3,600 s: the appex execs, is injected, and is killed in 5 s

Tag `wp4-d1d2-h2`, 14:54:03 → 15:54:03 (3,600 s), 0 panic, **11,765 frames**.

| observation | h2 |
|---|---|
| appex exec | **31** (poster 2: GradientPosterExtension 112, CollectionsPoster 113) |
| `ca-inject: loaded` | backboardd ×2 + **pid 112 + pid 113** |
| appex `is shown the device` | **0** |
| host clients | **1 (backboardd)** — no appex client |
| wallpaper texture (1125×2436 / ≥1000×2000) | **0** |
| hanging / timeouts | **60 / 20** |
| missing pipeline | **1** (`PBGRA_Tsb3Xf_Isrc` + `single_pass_blur_3_lpf`, 1 skipped draw) |

**The 5 s timeline (h2):** GradientPosterExtension[112] spawn 07:18:20.970 → `ca-inject: loaded as
pid 112` → `Extension is hanging on launch. Killing.` 07:18:25.978 (**5.01 s**) → exit.
CollectionsPoster[113] spawn 07:18:27.875 → `ca-inject: loaded as pid 113` → kill 07:18:32.882
(**5.01 s**). Both are injected; both die before a Metal device query or a wire connect.

### 3.3 Host and desktop

* Host: **1 client** (`backboardd`), `client connected (1 client(s))`, ack 11,765, 0 wire errors;
  the multi-client loop is active (backlog 16) but no appex ever connects.
* Desktop: **65.5 %** main-area black on the h1 stage-2 signature frame, widget bboxes **100 %
  white**, Dock 66.3 / Search 13.6 / direction normal. The h2 desktop PNG measures 58.9 % black
  (the t77 report lists 65.5 % for it, which matches the h1 steady frame; the h2 capture is not a
  like-for-like signature frame). Either way, the wallpaper/widget content is unchanged.
* Missing report: one blurred pipeline (`com.apple.coreanimation.draw.PBGRA_Tsb3Xf_Isrc`,
  vertex `downsample_blur_vert_lpf`, fragment `single_pass_blur_3_lpf`, 1 skipped draw) — a
  blur-family item for a later stage, not the D1/D2 cause.

### 3.4 Verdict

**Multi-client host: works. D1/D2: still blocked.** The host can serve backboardd and an appex
concurrently, but the appex never reaches the connect: launchd kills it **5.01 s** after spawn,
before `is shown the device`. The host is **necessary but not sufficient**; the remaining blocker
is the launchd extension watchdog (~5 s), not the wire.

## 4. What this establishes and what it does not

**Establishes**

* The host now has **per-client state, resource isolation and fast acks**, with 17/17 tests PASS
  and no performance regression; it is ready for a second client.
* The stage-54 combination (variant #4 root + delivery system) still lets the poster appexes exec
  and load `libcainject` (pid 112/113), so the injection/clients path is stable when the system is
  the delivery image.
* The appex is killed **5.01 s** after spawn — a tighter measurement of the watchdog than t68's
  5.63 s — and never queries a device or connects the wire.
* The desktop did not regress (65.5 % black, widgets white, Dock/Search normal).

**Does not establish**

* **That the appex would connect if it survived.** The host's multi-client path has not been
  exercised by a real appex; only the fixture proves it.
* **Why launchd's extension watchdog is ~5 s** or how to change it. The previous levers
  (extension-point key, appex Info.plist `XPCService`) are closed.
* **A rate.** h1 is too short; h2 is one boot; the poster exec/kill counts vary between boots.
* **A content fix.** The wallpaper is still black and the widgets still white; no wallpaper
  texture ever reaches the host.
* **A delivery change.** The image is experimental (variant #4 root), the host commit is on the
  experiment branch, and no V100/isolation core is involved.

## 5. Next steps

1. **Investigate the launchd extension-watchdog source** — where the ~5 s value comes from and
   whether it can be changed without breaking the extension job (launchd code/plist, or a job
   attribute that is actually read). The two previous plist levers are closed; this needs a
   different mechanism.
2. **Alternative: accept the boundary.** If the watchdog cannot be changed safely, keep the
   multi-client host and treat D1/D2 as blocked by launchd's extension lifecycle, not by Metal;
   document the boundary and stop spending variants on it.
3. **If an appex survives**, verify the second `hello` (program = appex), then the wallpaper
   texture; the multi-client host is already in place.
4. **Keep the missing-pipeline item** (`PBGRA_Tsb3Xf_Isrc` / `single_pass_blur_3_lpf`) for a
   blur-family stage; it is not the D1/D2 blocker.

## 6. Evidence and hashes

| item | value |
|---|---|
| host commit | `2d3f325` (2 files, +479/−106); new host md5 `b0bf8f767d816299a864f23785dee03b`; old source rebuild `d7e8192b7ae4f43b7ed677e60c4b76ad` |
| t77 images | variant #4 root `899bd711…` + delivery system `49ae3cf4…`; delivery root restored `a4a6e2ce…`; kernel `f62c3d4b…` |
| t77 report | `/tmp/t77-evidence/T77-REPORT.md` sha256 `80966561252f13c42631df5d3fec40e138ffc428c5d9aca710f7a4b60e8a0597` |
| serial-h1 / h2 | `ef8c89055a98097fdafc5e617e04772d4d74f5954a14342b790551a57d63f62a`; `ff1905afb26e52319b7d4927e036baa5fc64b3ec20921aa4e5902c508a2b44ac` (30.7 MB) |
| host-h1 / h2 | `2396e755…` / `cc50144c…` |
| missing-h2 | `b193c9ba68e2724ec58bb0cb5f6485d04675af9566dbaa70a1def28da9688d72` |
| monitor-h1 / h2 | `e2544fc8…` / `74ebb05e…` |
| captures | stage2-h1 `edb6e93c…`, stage3-h1 `16e88e19…`, desktop-h2 `cc53aa17…` + thumb `08e0a1d8…` |
| variant / delivery | root variant `899bd711…`; delivery system `49ae3cf4…`, root `a4a6e2ce…`; kernel `f62c3d4b…` |

### 6.1 Repository gates

`make test` is green at this tree: provenance clean (493 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. The host suite is 17/17 PASS
(16 existing + multi-client). `make style` keeps the pre-existing failures (check-links/check-spdx);
this record adds no new failure.

## 7. Files changed in this record

- `docs/stage55-multi-client-host.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows gain the multi-client host result and the watchdog
  boundary.
- `README.md` — the stage-55 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the host code commit
`2d3f325` is already on the branch, and the variant images live outside the repository.

## 8. Reproduce

```sh
# t77 h2 poster timeline
f=mt-work/logs/wp4-d1d2-h2/serial.log
grep -a 'spawned with pid 112\|spawned with pid 113\|ca-inject: loaded as pid 112\|ca-inject: loaded as pid 113\|hanging on launch' $f | head
# host: one client (backboardd)
grep -a 'client connected\|hello' /tmp/t77-h2-metalwire-host.log
# missing pipeline
cat /tmp/t77-h2-missing.txt
# restore (already done)
md5sum mt-work/hakios/userland/root.apfs   # a4a6e2ce50e6d0324e26a49ea20fbd84
```
