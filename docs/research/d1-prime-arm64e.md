# D1' — does the iOS 26 arm64e user space boot on a kernel we compile?

*Research note, 2026-10-31. No Apple binary was downloaded or opened, and none is described here. Sources read: Apple's open-source XNU at tag `xnu-12377.121.6`, Apple's platform-security guide, Clang's pointer-authentication documentation, dyld's cache-layout document, QEMU's ARM sources, and the two HakiOS repositories.*

Labels: **verified** (read in a primary source), **reported** (a project's or secondary source's own claim), **speculation** (reasoning, not evidence). This note decides nothing on its own: the gate closes in `docs/adr/D1-prime-userland-version.md`, which does not exist yet.

## 1. The question, stated precisely

D1' is three questions whose failures look different:

- **K1 — the kernel starts an arm64e process.** Falsified by `EBADARCH` from `exec`, or an `FPAC` (`ESR_EC_PAC`) on the first return to user space. Apple's `kern_exec.c` branches on the image cpusubtype (**verified**, [kern_exec.c](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/bsd/kern/kern_exec.c)); the sibling gates arm64e on ISA support, refusing with `EBADARCH` (**reported**).
- **U1 — the userland's own daemons start.** dyld slides the cache, `launchd` reaches `xpc_main`, ~24 daemons spawn. Fails with an FPAC *inside* launchd, a missing syscall, a mount that never happens, or a libxpc assertion.
- **S1 — SpringBoard renders.** A graphics question (IOSurface, backboardd, Metal→Vulkan). It can fail while K1 and U1 pass, and no kernel result speaks to it.

Two axes have been conflated. The proven baseline is **arm64 + 16.5**: upstream boots the stock "unmodified iOS 16.5 user space" with its own `xnu-8796.121.2` on a Snapdragon 778G phone and in QEMU `virt` (**reported**, [hakios README](https://github.com/oosexgang/hakios-xnu-kernel), [base README](https://github.com/superboyyy/hakios-arm64e-dada/blob/main/base/README.md)); the 778G has no pointer authentication (**reported**: ARM64E.md records that port as arm64-only). **arm64e + 16.5** was proven separately in QEMU by the sibling (**reported**, [ARM64E.md](https://github.com/superboyyy/hakios-arm64e-dada/blob/main/base/docs/ARM64E.md)). **arm64e + 26** is the unproven intersection.

One inherited detail is unsourced: the "iPhone10,3" device for the 16.5 baseline. Upstream's README names no device and says each user brings their own IPSW and keys (**verified**). Do not repeat it as fact.

## 2. What arm64e actually requires from the kernel

`arm64e` is Apple's ABI, not an ISA mode: "a specific ABI (not yet fully stable) for implementing pointer authentication using PAuth on certain Apple operating systems" (**verified**, [Clang PointerAuthentication](https://clang.llvm.org/docs/PointerAuthentication.html)). What the kernel owes it:

**Keys and their split.** PAC "uses five secret 128-bit values to sign kernel instructions and data, and each user space process has its own B keys" (**verified**, [Apple platform security guide](https://help.apple.com/pdf/security/en_US/apple-platform-security-guide.pdf), Pointer Authentication Codes). A keys are global (v-tables, function pointers), B keys local (return addresses); A keys are shared by every process attached to one shared region, since cache pointers signed with them must authenticate everywhere (**verified**, Clang doc, "arm64e then further subdivides the keys").

**Key programming is the closed part.** In `xnu-12377.121.6`, `SET_JOP_KEY_REGISTERS`, `SET_ROP_KEY_REGISTERS` and `PAC_INIT_KEY_STATE` have bodies only under `HAS_PARAVIRTUALIZED_PAC`, and those bodies are `hvc #0` calls into Apple's hypervisor (**verified**, [pac_asm.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/arm64/pac_asm.h)). So open source gives the interfaces and the key-selection policy, but no hardware path. The policy is open: `exec` may create a task with user JOP disabled and a dedicated non-arm64e key domain (`IMGPF_NOJOP` → `ml_task_set_disable_user_jop`, `ml_non_arm64e_user_jop_pid`) (**verified**, [machine_routines.c](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/arm64/machine_routines.c)).

**The kernel is a signer for the shared cache; dyld is the signer for dylibs.** `vm_shared_region_slide_page_v3()`/`v5()` take the process's `jop_key` and sign relocations carrying `si_ptrauth` (**verified**, [vm_shared_region.c](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/vm/vm_shared_region.c)). That code is gated on `jop_key != 0`, because on Apple hardware 0 means "use the hardware default key". Any backend deriving a real key from a `jop_pid` of 0 must still sign, or launchd FPACs on its first cache pointer (**reported**: the sibling's fix).

**Dylibs vs the cache.** An arm64e cache has `AUTH` and `AUTH_CONST` mappings of pointer-authenticated data, and always uses the Large layout because TEXT→DATA metadata is 32-bit signed (**verified**, [dyld CacheLayout.md](https://github.com/apple-oss-distributions/dyld/blob/main/doc/CacheLayout.md)). A standalone arm64e dylib is signed at load after sliding; on disk it carries chained auth fixups, not seals (**reported** by the sibling's reading of the same sources — I did not decode a cache myself).

**Could not verify:** which discriminator Apple feeds `pmap_sign_user_ptr` per cache relocation (it comes from cache metadata I did not parse); whether any arm64e userland path depends on key registers that standard ARM exposes but QEMU models differently (§4); and the sibling's "hardware PAC" claim for Oryon — Qualcomm publishes nothing user-readable that I found.

## 3. What differs between 16.5 and 26 user space

- **The cache is bigger and split.** Multi-file caches date from 2021 ("All caches prior to the 2021 releases used a single cache file layout") (**verified**, CacheLayout). iOS 26.6.2 on an iPhone18,2 carries 84 subcaches plus `.atlas` and `.symbols` in `Cryptex1,SystemOS` (**reported**, ARM64E.md). More regions means more per-process slide work; the slider interface itself is unchanged (**reported**: the sibling's three signing fixes applied to 12377 with field-name churn only).
- **The userland lives in cryptexes.** Apple moved patchable OS framework content into cryptexes, which are "bootstrapped after the kernel has booted", and whose "measurements … file system seals, and their associated trust caches are all represented in a separate Image4 ticket, which is cryptographically bound to the device" (**verified**, guide, Cryptexes). The cache launchd needs may come from a cryptex image rather than the root volume, and the seal Apple verifies is one we cannot and must not fabricate.
- **Code signing is a build-time switch in Apple's kernel.** `config/MASTER.arm64.iPhoneOS` has `config_enforce_signed_code` and `config_code_signature_reconstitution` in `KERNEL_BASE` (**verified**, [MASTER.arm64.iPhoneOS](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/config/MASTER.arm64.iPhoneOS)). The trust cache is "a trusted record of all the Mach-O binaries … represented by a code directory hash", and a hash found there means mappings "may possess any entitlement and execute without further verification as to the authenticity of the signature" (**verified**, guide, Trust caches). A kernel with neither AMFI nor Image4 must answer this itself.
- **iOS 26 kernels have SPTM in tree.** SPTM is "supported in the A15 or later and M2 or later SOCs and replaces Page Protection Layer" (**verified**, guide). `xnu-12377.121.6` ships `osfmk/arm64/sptm/` with its own pmap and `start_sptm.s` (**verified**, [tree](https://github.com/apple-oss-distributions/xnu/tree/xnu-12377.121.6/osfmk/arm64/sptm)), while iPhoneOS still selects `config_pmap_ppl` (**verified**) — a platform fork, not a userland precondition. Whether 26 userland *queries* SPTM is **unverified**; a test entitlement `correct_sptm_booted` exists (**verified**).
- **launchd/libSystem expectations.** Concretely unverified. Verifiable: 12377 reshuffled kernel-internal interfaces a port touches — `UIO_STACKBUF`, `vm_map_switch_to`, split `vm_*_xnu.h` headers, `CoreEntitlements/V2` (**reported**, ARM64E.md §"Milestone 4c"). Diffing `syscalls.master` answers that (§5, E2).
- **No arm64 fallback.** iOS 26's device list holds only arm64e-capable models, so the arm64 slice that made the 16.5 baseline easy is gone (**speculation**; the sibling states it directly, **reported**). This is the most consequential difference: 16.5 could be booted with PAC out of the picture entirely.

## 4. What the sibling project has actually achieved

From its own files:

| Claim | Evidence |
| --- | --- |
| arm64e `xnu-12377` builds; `hello` built `-arch arm64e` runs as PID 1 under `QEMU_CPU=cortex-a710` | **reported**, ARM64E.md §4b, with the six boot fixes and their fault PCs |
| 16.5 arm64e userland in QEMU: dyld, launchd, ~24 daemons, SpringBoard `job state = running` | **reported**, ARM64E.md §"What milestone 3 took" |
| iOS 26 reaches the home screen, touch works | **reported**, [README](https://github.com/superboyyy/hakios-arm64e-dada) status table — on a Xiaomi 15 |

**It is phone-only for 26.** The same repository's milestone document, in the same import, lists QEMU boot of the iOS 26 user space as outstanding and calls arm64e on real silicon "not started" — while the README, committed a day later, reports the phone reaching the home screen. Take the newer, more specific phone result, and note that **the QEMU result for 26 does not exist**. Citing this repository as proof that "iOS 26 works in QEMU" cites a document that says the opposite.

The kernel-side gap is small; the userland-side one is not. K1 is done twice (16.5, 26), U1 on silicon but not in QEMU, and S1 needed Metal→Vulkan plus a GPU driver. The phone runs with a hand-written AMFI substitute whose own header says "No trust cache contains anything, and there is no signature policy behind it; a process holds the entitlements that the signature of its program lists" (**verified** as written, [kernel patch](https://github.com/superboyyy/hakios-arm64e-dada/blob/main/kernel/xnu-12377.121.6-dada.patch)) — signature verification absent, not defeated.

One environment caveat: QEMU's `cortex-a710` advertises `ID_AA64ISAR1_EL1 = 0x0010111101211052`, APA field FPAC-combined (**verified**, [cpu64.c](https://github.com/qemu/qemu/blob/master/target/arm/tcg/cpu64.c)), but keeps **one** key set, `env->keys`, for EL0 and EL1 alike (**verified**, [cpu.h](https://github.com/qemu/qemu/blob/master/target/arm/cpu.h), `target/arm/tcg/pauth_helper.c`). Apple separates them — its own comment reasons about the "EL0 diversifier" not disturbing "EL1 A key state" (**verified**, `pac_asm.h`) — so a QEMU boot cannot exercise that split (U6).

## 5. The cheapest experiments, in order

**E0 — reproduce arm64e-with-hello (2–4 hours; Linux, QEMU, no Apple content).** Rebuild `xnu-12377` as arm64e with the standard-PAC backend and boot `hello` as PID 1 under `QEMU_CPU=cortex-a710`. Answers K1 in *our* tree and validates the loader's chained-fixup signing and `TCR_EL1` match. Fails informatively: `EBADARCH`, FPAC at the first `retab`, or auth chains left encoded.

**E1 — dyld and launchd from the contributor's own iOS 26 arm64e cache (1 day; needs an image they extracted themselves).** Stop before SpringBoard; the goal is the first FPAC PC, or launchd reaching `xpc_main`. This is milestone 3 redone on 26 and the only experiment that answers U1. Its three informative failures: cache-pointer FPAC (`jop_key != 0`, or the discriminator scheme), a missing syscall, or a mount that never appears. Do not build a trust-cache bypass to get past it (§6).

**E2 — static ABI diff, `xnu-8796.121.2` vs `xnu-12377.121.6` (half a day; nothing from Apple).** Diff `syscalls.master`, `kern_exec.c`'s arch and platform paths, and launchd-visible sysctls. Counts the features 26 userland needs that 16.5 did not — a finite list before a day goes into E1. Fails by producing a number, which is the point.

**E3 — mount the OS cryptex read-only so the cache is reachable (2–3 days; APFS reader, the user's image).** Answers whether launchd can find its filesystem at all; fails at APFS feature checks (seal, compression, graft points), cheap to learn early.

**E4 — SpringBoard (weeks; needs a GPU path).** Not a D1' experiment: the phone needed metal2vulkan, Turnip and a GPU driver. Gating D1' on S1 conflates porting with graphics.

## 6. Decision criteria for D1'

**Port it** if E1 ends with launchd running and its remaining failures are a finite, feature-shaped list (syscalls, vnodes, a mount, a mach service) E2 already enumerated — and none of it is a cryptographic verification we cannot honestly perform.

**Stay on 16.5, treat 26 as research** if E1's failure surface is dominated by things only silicon or only Apple's closed kexts provide — SPTM, SEP, per-CPU key registers, Image4 — or if launchd's first failure is authenticating something we cannot derive.

**Stop** if the only path forward requires a check to answer "valid" without deriving validity from bytes we loaded: trust-cache entries for cdhashes not computed from the image we mapped; patching a signature or a signature-checking routine; forging an Image4 or cryptex ticket; defeating activation or device attestation. Two things that are *not* that, and belong in our docs as such: not implementing a check at all (the `miniamfi` position), and a trust cache of our own over hashes we computed. The line is code that claims verification it did not do.

## 7. Open unknowns, one name each

- **U1-QEMU26** — does the iOS 26 arm64e userland boot in QEMU at all? Sibling evidence is silicon-only.
- **U2-SUBCACHE-SCALE** — how many cache regions can we slide? 84 subcaches plus atlas on 26 vs the 16.5 shape.
- **U3-CRYPTEX-MOUNT** — which mount points must exist (`/System/Cryptexes/OS`, `/private/preboot/Cryptexes/*`, the graft) before dyld maps the cache.
- **U4-NOJOP-SEMANTICS** — what `IMGPF_NOJOP` buys; whether any 26 binary is built to use it.
- **U5-SPTM-QUERY** — does 26 userland detect SPTM? The `correct_sptm_booted` entitlement suggests something checks.
- **U6-QEMU-KEYS** — how far QEMU's single key set misleads; any result depending on EL0/EL1 separation must be re-run on hardware.
- **U7-ENTITLEMENTS-SOURCE** — where entitlements come from with no AMFI, and whether 26 userland trusts anything beyond the signature blob.
- **U8-FIRST-FAULT-MAP** — the ordered list of launchd's first N faults on 26, with PCs and symbols. This is E1's deliverable, not a verdict.
- **U9-JOB-STATE** — what `job state = running` proves: that launchd thinks the job started, not that a frame was drawn. Keep the two apart in every status claim.

---

## Addendum, 2026-10-31 — what the sibling's own document settles, and where it stops

Read `superboyyy/hakios-arm64e-dada` at `main` (2026-10-31), `base/docs/ARM64E.md`, 434 lines. It
answers more of D1' than the earlier note could, and it also names the wall precisely.

**Settled: the iOS 26 kernel boots, arm64e, in QEMU.** Their milestone 4b: the arm64e
`xnu-12377` kernel boots `hello` as PID 1 under `QEMU_CPU=cortex-a710` with no PAC fault, then
panics on initproc exit — the same terminal state as the arm64 bring-up. Six boot fixes are named
individually (kstackptr PAC-DA gating, trust-cache init, `_unsafe_kernel_text_initialized`,
`iomediaClass`, `mi_base` width, and the `fleh_dispatch64` `pacia` ordering). The loader's chained
fixup signer needed no retarget: `CHAINED_PTR_ARM64E_KERNEL` is unchanged between the tags. That
matches E2's finding that the ptrauth surface is frozen.

**Settled: the kernel side of the iOS 26 user space is ready.** Their milestone 4c resolves the 17
deferred userland/filesystem rejects and states that the kernel "is now ready for the full iOS 26
arm64e user space (launchd + daemons + SpringBoard)". So the kernel is not the blocker.

**Not settled, and named as data rather than code:** the extracted iOS 26 userland is not on disk.
Their words: re-acquiring it means downloading the iPhone 17 Pro Max iOS 26.6.2 IPSW (~12 GB),
then "AEA-decrypting the root/cryptex (`/usr/bin/aea` + the FCS key from Apple)", re-extracting the
84-subcache arm64e dyld shared cache, writing an `ios26/` selection config, and pointing
`qemu/run-ios.sh` at the xnu-12377 kernel (it hardcodes the xnu-8796 path today).

### The two blockers this project has to decide about, not engineer around

1. **A build host.** Their kernel build uses Xcode's `iig` (the `iig` shim "runs the real Xcode
   iig", falling back to SDK headers) and the iphoneos SDK, and `MakeInc.cmd` is left as-is
   "since the absolute paths exist and the build succeeds on macOS". **Nothing in this repository
   can produce an arm64e xnu-12377 kernel on Linux today**, and the sibling publishes no binaries.
   Unlike the 16.5 path — which builds on Linux with the theos toolchain — the iOS 26 kernel build
   currently assumes a Mac.
   Requirement, from Apple's own table (<https://developer.apple.com/support/xcode>): the iOS 26 SDK
   ships in **Xcode 26.0.1–26.3**, which runs on **macOS Sequoia 15.6 or later**; Xcode 26.4+ wants
   macOS Tahoe 26.2+. Cross-compiling arm64e needs no Apple Silicon — any Mac on Sequoia 15.6+ with
   Xcode 26 runs the build. `github.com/theos/sdks` publishes SDKs only up to `iPhoneOS16.5`, which is
   exactly why the 16.5 kernel builds on Linux and the 26 one does not.
2. **The userland data is behind an encrypted container.** iOS 26 ships the root and cryptex
   filesystems as `.aea` (Apple Encrypted Archive), and their recipe decrypts them with
   `/usr/bin/aea` and "the FCS key from Apple". **This project's rule is not to defeat a technical
   measure** (PROVENANCE.md, ADR-006). Whether that step is ordinary extraction (a key published in
   the IPSW or its build manifest) or a key obtained from Apple's servers / derived from a device
   identity decides whether it is in bounds at all — and that question must be answered **before**
   any data is fetched, not after.

### So: what "can we run iOS 26" means today

| Layer | State | Evidence |
|---|---|---|
| Kernel boots arm64e processes | **proven in QEMU** | sibling milestone 4b |
| Kernel is ready for iOS 26 user space | **proven** | sibling milestone 4c |
| iOS 26 user space boots in QEMU | **not done anywhere public** | sibling names it as their remaining work |
| SpringBoard renders (Metal→Vulkan) | **not done** | their phone does it through Turnip on real Adreno |

The honest summary: **iOS 26 is not a research question any more at the kernel level; it is a data
and build-host question.** That is a much better position than the project was in this morning, and
it is also why D1' cannot close from this machine.
