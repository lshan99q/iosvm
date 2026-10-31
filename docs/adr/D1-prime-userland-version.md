# D1' — the demonstrable target is the Darwin 16.5 user space, not iOS 26

- Status: Accepted (closes decision gate D1')
- Date: 2026-10-31
- Supersedes: —
- Superseded by: —
- Related: `docs/research/d1-prime-arm64e.md`, `docs/research/e2-kernel-port-surface.md`, ADR-008

## Background

The project's headline goal was "iOS 26 arm64e user space rendering SpringBoard". ADR-008 chose a
from-scratch kernel, which left one open question, D1': *does the iOS 26 user space boot the way
Darwin 16.5 does?*

Three things now answer it, and none of them is "we could not make it work".

## Evidence

**1. The kernel is not the obstacle, for either version.**

- This repository booted a self-built XNU (upstream `prebuilt/hakios-kernel`, `xnu-8796.121.2`) to
  BSD initialisation in 6.3 s of emulated time, five asserted markers, reproducible with
  `make qemu-open-check` (`docs/m0-host-report.md`).
- The sibling project booted an **arm64e `xnu-12377`** (the iOS 26 kernel) to `hello` as PID 1 under
  `QEMU_CPU=cortex-a710` with no PAC fault, and then resolved the last 17 userland/filesystem
  rejects and states the kernel is ready for the full iOS 26 user space
  (`hakios-arm64e-dada`, `base/docs/ARM64E.md` milestones 4b and 4c).
- E2 confirms the port surface is small: **0 syscalls renumbered, 0 removed**, the ptrauth key
  surface byte-identical between the tags.

**2. The Darwin 16.5 user space data is ordinary, unencrypted content.**

The upstream extraction path treats the 16.5 images as plain APFS containers: `sysimage/unpack.sh`
extracts `096-04088-077.dmg` (root), `096-04666-078.dmg` (OS cryptex) and `096-03536-083.dmg` (App
cryptex) with 7-Zip and its own LZBITMAP helper. There is no key management anywhere in that path.

Verified here on the real archive, not just read: downloaded
`iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw` — **6,165,698,198 bytes, exactly the size upstream's
script refuses to run against anything else** — and read its manifest:

| Component | Path | Form |
|---|---|---|
| `OS` | `096-04088-077.dmg` | plain, 4.7 GB, APFS volume `SydneyF20F66.D22D221OS.apfs` |
| `Cryptex1,SystemOS` | `096-04666-078.dmg` | plain, 2.7 GB |
| `Cryptex1,AppOS` | `096-03536-083.dmg` | plain, 10 MB |
| `RestoreKernelCache` | `kernelcache.release.iphone10b` | 18 MB |

`tools/unpack-ipsw.sh --apply` extracted the whole archive in 51 s: 48 MB of firmware and 7.2 GB of
disk images, no key, no macOS, no failure. 7-Zip 23.01 opens the root image and reports the APFS
volume by name. That is the pipeline this project is going to stand on, demonstrated end to end.

**3. The iOS 26 user space data is behind Apple's key service.** *This is the deciding fact.*

Downloaded `iPhone18,2_26.6.2_23G90_Restore.ipsw` (11,439,109,684 bytes, sha256
`45c6942e20d8a4a40fe83084a23a9534fa038e266e74f38f4176d9f95f0f21aa`, matching Apple's published
size and the known digest) and read its `BuildManifest.plist`:

| Component | Path | Form |
|---|---|---|
| `OS` (the system volume) | `094-95330-090.dmg.aea` | **AEA**, 7.8 GiB |
| `Cryptex1,SystemOS` (holds the 84-subcache dyld cache) | `094-97204-090.dmg.aea` | **AEA**, 1.87 GiB |
| `Cryptex1,AppOS` | `094-96441-094.dmg` | plain, 14 MB |
| `RestoreRamDisk` | `094-95379-094.dmg` | plain, 232 MB |

The two we would need are the two that are encrypted. The AEA header (observed directly, no key
material read) carries:

```
AEA1
com.apple.wkms.url          https://wkms.sd.apple.com
com.apple.wkms.auth-data    <opaque, per-archive>
com.apple.wkms.fcs-response {"enc-request": …, "wrapped-key": …}
com.apple.wkms.fcs-key-url  wkms-public.apple.com/fcs-keys/<id>
```

A wrapped content key plus a key-fetch URL and per-archive authentication data is a **key delivery
service**, not file encryption with a published passphrase. Obtaining the content key means asking
Apple's service for it.

## Decision

**Target the Darwin 16.5 user space. iOS 26 arm64e is deferred indefinitely, blocked on licensing
rather than engineering.**

This project's rules are unambiguous and were written before this finding: no code that impersonates
a device to Apple's servers (`PROVENANCE.md`, `docs/adr/ADR-006-no-credentials.md`), and the boot
path must never defeat a check. Fetching a content key from Apple's key management service to open
an OS image is exactly the class of act those rules exist to exclude. So the 26 path stops here —
**not** because it is hard, but because the lawful version of it does not exist yet.

## Options weighed

| Option | Good | Bad |
|---|---|---|
| 16.5 user space (chosen) | data is plain APFS; upstream tooling already handles it; the kernel is proven here; every claim we make can be reproduced by someone else with a legal IPSW | not the newest UI; a Metal translation target from 2023 rather than 2026 |
| iOS 26 user space, obtaining the key | the headline goal | requires acting against Apple's key management service; the project would lose the legal position its entire design rests on |
| iOS 26 kernel only, no user space | pure open source, no legal question | boots a kernel that can run `hello` and nothing else; no userland target, no SpringBoard, no deliverable |
| An intermediate version (17/18) if its images are plain | newer userland, same legal footing | **unknown** — nobody has checked where AEA delivery began; a cheap question (see follow-up) |
| Corellium | works today | commercial, and it is not this project |

## Consequences

**Positive**

- The road from here is legally clean end to end. Every contributor can reproduce every result with
  an IPSW they downloaded themselves, and no step of it needs an entitlement, a key service or a
  signature bypass.
- **M3 and M4 get materially smaller.** The iOS 16.5 UI predates Liquid Glass: no real-time blur and
  refraction shader work, and far less compute-shader pressure on `MTLComputeCommandEncoder`. Risk
  R2 (shader coverage) narrows to the CoreAnimation subset of that era. The second pivot point loses
  much of its teeth.
- The target device becomes unambiguous: `iPhone10,3` / `iPhone10,6` (iPhone X), the device the
  upstream `unpack.sh` and its `20F66` shared cache are built around — not the `iPhone17,3` the
  earlier documents guessed.

**Negative**

- The project no longer promises "iOS 26". Anything already written that says so has been retargeted
  (see the docs changed with this ADR); the README now states 16.5 as the target and 26 as deferred.
- arm64e work is deferred with it. The kernel-side arm64e knowledge (E2, ADR-008's evidence) stays
  documented but is not on the v1.0 path.
- A future move to 26 would need the whole data pipeline redone under whatever Apple's delivery
  looks like then, and a legal opinion.

## Reopen conditions

This ADR should be revisited if **any** of the following becomes true, and the first one is a
question for a lawyer, not for an engineer:

1. A legal opinion says obtaining and using that key for an IPSW one has lawfully acquired is not
   circumvention in the relevant jurisdiction. Record the opinion, do not paraphrase it.
2. Apple ships the OS and System cryptex images unencrypted again, or publishes the key material
   through a channel meant for general use.
3. A user supplies a userland image they obtained lawfully, and can say how.
4. *Answered on 2026-10-31, and it does not change the decision — see the addendum below.*

## Follow-up

- **Cheap, worth doing once:** determine which iOS release first shipped `OS` as `.dmg.aea`. If 18.x
  is plain, this project's ceiling rises two major versions at no legal cost. The check is the same
  one used here: read the `BuildManifest.plist` of that release and look at the `OS` path.
- Retarget the phase plan, the tooling examples and the README (done in the commits that follow this
  ADR).
- Keep `docs/research/d1-prime-arm64e.md` and `docs/research/e2-kernel-port-surface.md` as the record of what the
  26 path would take, so the work is not lost if the gate reopens.

## Addendum, 2026-10-31 — where the encryption actually starts (reopen condition 4)

Read the `BuildManifest.plist` of each release straight out of the archive with HTTP range requests —
the central directory first, then only that one entry, a few hundred KiB per release instead of a
multi-gigabyte download:

| Release | `OS` | `Cryptex1,SystemOS` | Form |
|---|---|---|---|
| iOS 16.6.1 (`20G81`) | `096-56967-082.dmg` | `096-56849-083.dmg` | plain |
| iOS 17.7 (`21H16`) | `044-17482-018.dmg` | `044-17955-018.dmg` | **plain** |
| iOS 18.6.2 (`22G100`) | `043-18757-102.dmg.aea` | `043-18359-102.dmg.aea` | **AEA** |

So the encryption did not arrive with iOS 26: **it arrived with iOS 18.** iOS 17 still ships ordinary
APFS containers. That is the answer to reopen condition 4, and it is better news than expected —
there is a lawful version two major releases above 16.5.

It does not move the target, because of a different fact: **iOS 17 dropped every arm64 device.**
A11 (iPhone X, `iPhone10,3`) ends at iOS 16.7; iOS 17 requires A12 and up, all of which run arm64e
userland. So the upgrade from 16.x to 17.x is not a data problem at all — it is the arm64e kernel
work (standard-PAC backend, arm64e userland, `docs/research/d1-prime-arm64e.md`) that this release
defers anyway.

The practical ladder is therefore:

| Ceiling | Data | Kernel cost |
|---|---|---|
| **16.x — chosen** | plain | arm64, works today |
| 17.x | plain, lawful | arm64e: the whole HAS_STD_PAC port |
| 18.x and later | AEA, behind Apple's key service | arm64e *and* a licensing question |

So D1' stands, and its successor question is written down: *do we want arm64e badly enough to pay for
it?* If the answer ever becomes yes, the userland is available lawfully in the 17.x line, and this
ADR's cost estimate for the upgrade is the kernel work only.
