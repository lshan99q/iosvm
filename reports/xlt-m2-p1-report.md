# xlt M2/P1 accelerator — independent verification report

- Date: 2026-11-28
- Verifier: exp (independent of the implementer)
- Task: t11 [M2/P1 verify], attempt 2 (`25a87c58-4f39-4910-aa2d-76cf263a6cb2`)
- Repo: `/home/lshan/iosvm&o`, branch `feat/xlt-translator`, HEAD `9267e0f`
  (`feat(xlt): add in-tree AArch64-to-x86_64 translator`)
- Verdict: **PASS** — the patch series applies cleanly to an untouched QEMU 8.2.2 tree,
  the tree builds, both P1 fixtures produce byte-for-byte TCG parity under
  `-M xlt -accel xlt`, shutdown exits 0 deterministically, failure paths are clean, and
  the M1 engine/tests/benchmark are unaffected (10.02x, all checksums).

The implementer's (t10) numbers were not reused: every result below comes from a fresh
QEMU tree, a separately built `libxlt.a`, and runs made during this verification.

## 1. Pristine tree, patches and build

The QEMU tarball used by t9 was copied rather than re-downloaded (allowed by the task):

| item | value |
| --- | --- |
| tarball | `/home/lshan/xlt/qemu-8.2.2.tar.xz` (129,398,020 B) |
| sha256 | `847346c1b82c1a54b2c38f6edbd85549edeb17430b7d4d3da12620e2962bc4f3` |
| verify tree | `/home/lshan/xlt/qemu-8.2.2-verify` (118 top-level entries, `VERSION` = 8.2.2) |
| build tree | `/home/lshan/xlt/qemu-8.2.2-verify-build` |
| libxlt archive | `/home/lshan/xlt/verify-libxlt/libxlt.a` (built separately, 15,637,404 B) |

Patch application method (the captain's caveat was addressed):

1. Fresh unpack with its own repository: `git init && git add -A && git commit` in
   `/home/lshan/xlt/qemu-8.2.2-verify` → pristine commit `b7e455a`
   (`QEMU 8.2.2 pristine baseline`). Because that directory now has its own `.git`,
   `git apply` resolves patch paths against the QEMU root and cannot silently skip
   them via the parent `/home/lshan/xlt/.git` prefix rule.
2. `git apply --check` then `git apply` for each patch, in order, from the verify tree
   root. Both clean.
3. Independent cross-check with `patch -p1 --dry-run` from the same tree: both clean.
4. Applied result committed as `686af56` (`xlt P1: accelerator and machine patches`):
   9 files changed, 366 insertions.

| patch | bytes | sha256 |
| --- | --- | --- |
| `xlt/qemu/patches/0001-accel-add-xlt-accelerator.patch` | 4,833 | `00fb9f905972cf3f661c1253c68f6cdb85a152de15847f441cbf44684d6d5307` |
| `xlt/qemu/patches/0002-hw-arm-add-xlt-machine.patch` | 6,818 | `166b02e221279a453107dbb1bcde2c299e0abec52700a4a2e819b6772b67ec9d` |

No fuzz: `git apply` rejects fuzzy application by design, and the independent
`patch -p1 --dry-run` reported clean; no `.rej`/`.orig` files exist in the tree.

`libxlt.a` was rebuilt from the repo's current working-tree `xlt/src`
(`decode.cpp jit.cpp libxlt.cpp libxlt_c.cpp lift.cpp`, `-O2 -g -std=c++17 -fPIC`,
`-I xlt/src`, `llvm-config-18 --cxxflags`) so the QEMU binary under test uses a freshly
compiled bridge, not t10's cached archive.

Configure (exit 0, log `/tmp/qemu-verify-configure.log`):

```sh
../qemu-8.2.2-verify/configure \
  --target-list=aarch64-softmmu --disable-docs --disable-werror --disable-gtk \
  --disable-sdl --disable-vnc --disable-opengl --disable-spice --disable-libssh \
  --disable-guest-agent \
  --extra-cflags="-I/home/lshan/iosvm&o/xlt/src -I/usr/lib/llvm-18/include" \
  --extra-ldflags="-L/home/lshan/xlt/verify-libxlt -lxlt \
    -lLLVM-18 -lcapstone -lpthread -ldl -lz -ltinfo -lstdc++ -lm \
    -Wl,-rpath,/usr/lib/llvm-18/lib"
```

Build: `ninja -j16 qemu-system-aarch64` (log `/tmp/qemu-verify-build.log`), 1,971/1,971
targets, exit 0, real 92.62 s (user 804.22 s, sys 101.34 s), host load 4.02 → 10.35.
The log contains zero `warning:`/`error:` lines. Binary:
`/home/lshan/xlt/qemu-8.2.2-verify-build/qemu-system-aarch64`, 111,654,544 B,
`QEMU emulator version 8.2.2`.

The new machine and accelerator are registered:

```text
$ qemu-system-aarch64 -M help | grep xlt
xlt                  xlt AArch64-to-x86-64 translator (P1)
$ qemu-system-aarch64 -accel help
xlt
```

## 2. TCG vs accelerator parity

Commands (serial on stdio, no reboot after PSCI; each guarded by `timeout 20`):

```sh
# TCG baseline
QEMU -M virt -cpu cortex-a76 -smp 1 -m 256M -accel tcg \
     -kernel <fixture>.elf -display none -serial stdio -monitor none -no-reboot

# xlt accelerator
QEMU -M xlt -cpu cortex-a76 -smp 1 -m 256M -accel xlt \
     -kernel <fixture>.elf -display none -serial stdio -monitor none -no-reboot
```

Fixtures were rebuilt from `xlt/tests/fixtures/p1/build.sh` during this verification:
`fixture1.elf` 66,992 B sha256 `9595e95a37ce062b1eda18e895be19d3dab9bb708d23e9abf7869d4a964fa214`;
`fixture2.elf` 67,384 B sha256 `35d47eac4d4dc35242c7fc252dba3ed302511542d1777296a75dabb2bc48cfec`.
The t9 smoke image was also re-run as extra coverage (`69b57410…`).

| fixture | TCG stdout | xlt stdout | rc | wall (TCG / xlt) |
| --- | --- | --- | --- | --- |
| fixture1 | `xlt-p1-fixture1` | identical | 0 / 0 | 0.05 s / 0.07-0.08 s |
| fixture2 | `xlt-p1-fixture2 fr=0x0000000000000090 ram=0x1122334455667788 el=0x0000000000000004` | identical | 0 / 0 | 0.05 s / 0.08 s |
| qemu-smoke (extra) | `xlt-p1-ok` | identical | 0 / 0 | 0.05 s / 0.05 s |

Byte-for-byte claims were checked with `cmp`/`diff` on captured stdout (and the
hexdump confirms identical bytes, including the trailing newline). Full captures:
`/tmp/t11/logs/{f1,f2,smoke}-{tcg,xlt}.{out,err,rc,wall}`.

Semantics covered by fixture2: PL011 `FR` read-back (`0x90`), RAM store/load of
`0x1122334455667788` at `0x40090000`, `CurrentEL` sysreg read (`0x4`), and PSCI
`SYSTEM_OFF` via `hvc #0`.

## 3. Determinism and shutdown

- Accelerator run twice per fixture: stdout byte-identical (fixture1 and fixture2),
  stderr empty, exit code 0 every time.
- `PSCI SYSTEM_OFF` (`x0 = 0x84000008`, `hvc #0`) always terminates with QEMU exit
  status 0; `-no-reboot` is honoured. No hang in any run (all bounded by `timeout 20`).
- The t9 smoke image gives the same result (exit 0), i.e. the P1 machine/accelerator
  path also covers the earlier semihosting-style payload via the HVC exit.

## 4. Failure behaviour

All under `timeout 20`; none hung.

| case | exit | stderr |
| --- | --- | --- |
| `-M xlt -accel xlt` without `-kernel` | 1 | `qemu-system-aarch64: xlt: -kernel <elf> is required` |
| unknown accelerator (`-accel nope`) | 1 | `qemu-system-aarch64: -accel nope: invalid accelerator nope` |
| `-M virt -accel xlt` (wrong machine) | 1 | `qemu-system-aarch64: -accel xlt: -accel xlt is only supported by -M xlt` then `failed to initialize xlt: Operation not permitted` |

Observations (not failures, but worth fixing in P2 or in the docs):

1. **`-M xlt` without `-accel xlt` also runs the guest successfully.** The machine's
   init starts the guest thread directly (`hw/arm/xltm.c:194` calls
   `xlt_start_guest_thread(xltm_guest_run, s)`), so the selected accel class is not
   actually required by the machine; `-M xlt ...` on the default TCG accel printed
   `xlt-p1-fixture1` and exited 0. This contradicts `xlt/qemu/p1/NOTES.md`
   ("`-M xlt` without `-accel xlt` unsupported"). Either document the real behaviour or
   move the guest-thread start into `xlt-accel`'s init so the accelerator is genuinely
   required; the P1 parity result itself is unaffected.
2. The `-M virt -accel xlt` rejection prints two error lines (the `init_machine`
   message plus QEMU's generic "failed to initialize"), which is acceptable but could
   be a single diagnostic.
3. `-accel xlt` must come with `-M xlt`; there is no virt-machine compatibility in P1
   (by design; see gaps).

## 5. M1 regression

Run in the repo after the QEMU verification, from the same working tree t10 will commit:

- `make -C xlt clean && make -C xlt`: exit 0, 25.15 s; 159 warnings, all from
  `/usr/lib/llvm-18` headers, **0 from `xlt/src`/`xlt/tests`**; binary
  `xlt/build/xlt` 10,047,624 B (grew from the M1-era 9.84 MB due to the libxlt bridge).
- `make -C xlt test`: exit 0, `decode_test` 481 checks, `xlt tests: pass
  (1 test binary(ies))`, 1.35 s.
- Bench ELF rebuilt: 69,224 B, sha256
  `d28c71ecd46ccd5ed9b0a24ddccdf407044900dc45d1618f52afd5e3fd643113` (unchanged).
- `xlt/bench/run.sh`: exit 0, all 5 checksums match, wall 8.28 s. TCG total
  444,113,304 ticks / 7,105.81 ms; xlt total 44,313,763 ticks / 709.02 ms; **10.02x**
  overall. Per test: alu64 1.90x, memcpy 46.78x, hash 9.83x, ptrchase 11.70x,
  indcall 1.51x. (Indcall is the narrowest margin; the acceptance floor of 1.5x is an
  overall-total requirement and is met with an order of magnitude spare.)
- Direct CLI check `xlt run xlt/build/bench/bench.a64.elf --stats`: exit 0, checksums
  5/5, `wall=896,791,201 ns`, `translate=4,052,778 ns`, `opt+jit=184,723,708 ns`,
  `regions=11`, `dispatches=16,000,014`, `insns=343,894,262`, `stats
  blocks-translated=11 blocks-executed=16,000,015`.
- CLI surface unchanged: `xlt --version` → `xlt 0.1` (rc 0), `xlt --help` rc 0,
  `xlt run /nonexistent.elf` rc 1 with `xlt: cannot open /nonexistent.elf`.

Host load during these runs was 5.9-6.4.

## 6. Timing evidence

| measurement | value |
| --- | --- |
| QEMU configure | exit 0, `/tmp/qemu-verify-configure.log` |
| QEMU build (`ninja -j16 qemu-system-aarch64`) | **92.62 s** real, 804.22 s user, 101.34 s sys; load 4.02 → 10.35 |
| M1 build (`make -C xlt`) | 25.15 s |
| M1 tests | 1.35 s |
| accelerator fixture1 startup→exit (wall) | 0.07 s / 0.08 s (TCG 0.05 s) |
| accelerator fixture2 startup→exit (wall) | 0.08 s / 0.08 s (TCG 0.05 s) |
| parity suite `xlt/qemu/p1/run.sh` wall | 0.96 s (verify build), 0.94 s (t10 build) |
| M1 bench run (`run.sh`, TCG + engine) | 8.28 s |

The P1 accelerator is slightly *slower* than TCG for these micro-fixtures (0.07-0.08 s
vs 0.05 s) because each run pays translation/JIT startup for ~a few thousand
instructions; throughput advantage only shows on the long-running benchmark (§5).

## 7. Deviations accepted for P1

- Custom `-M xlt` machine instead of reusing `-M virt`, and no QEMU CPU/CPUState: the
  x86-64 JIT runs as a host-side guest thread under the BQL.
- No MMU/page tables/stage-2, no exceptions, no interrupts/GIC/SMMU, single vCPU.
- MMIO is synchronous through `address_space_rw` (PL011 is the only device).
- `hvc #0` is an exit: PSCI `SYSTEM_OFF`/`SYSTEM_RESET` only; other HVC values abort.
- Host-emulated system registers: `CurrentEL=4`, `CNTFRQ_EL0=62.5 MHz`,
  `CNTVCT_EL0` from QEMU's virtual clock /16, `MPIDR_EL1=0`; writes ignored.
- No firmware-from-flash, no DMA or self-modifying-code invalidation, no semihosting
  beyond the no-op hook.

These are consistent with the P1 scope in `xlt/qemu/INTEGRATION.md` and are not
verification failures.

## 8. Known gaps to P2

- `virt` machine compatibility (use the real ARM virt board rather than a bespoke
  machine), including GIC and the standard device tree.
- Page tables / MMU / exception model; interrupts (GIC, timers) and vCPU state.
- Firmware execution from flash (EDK2/U-Boot-style boot) instead of only `-kernel` ELF.
- Multiple vCPUs and real CPU hotplug/`CPUState` integration; PSCI beyond
  off/reset (`CPU_ON`, `SYSTEM_SUSPEND`, ...).
- DMA/self-modifying-code invalidation, cache maintenance ops, semihosting.
- Make `-accel xlt` genuinely required by `-M xlt` (or document that the machine is
  accel-agnostic, see §4).

## 9. Reproduction in the repo

New: `xlt/qemu/p1/run.sh` (4,516 B, sha256
`9ba707b257fbc2666d948f77eb8f8ba2fa6d9536a6bf77e19a2c4c3df96cb620`,
SPDX MIT, `bash -euo pipefail`, shellcheck clean). It rebuilds the fixtures, runs each
fixture on TCG and on `-M xlt -accel xlt`, compares serial output byte-for-byte, checks
the two failure paths, and prints `run.sh: PASS`; exit 1 on mismatch and 2 on setup
errors. The QEMU binary defaults to the `build-xlt.sh` output and can be overridden with
`QEMU=/path/to/qemu-system-aarch64`.

```sh
# after xlt/qemu/build-xlt.sh
xlt/qemu/p1/run.sh
# or against any independently built patched QEMU
QEMU=/home/lshan/xlt/qemu-8.2.2-verify-build/qemu-system-aarch64 xlt/qemu/p1/run.sh
```

The script was tested against both the verify build and t10's build; both pass. It
uses `mktemp` for all scratch output, so nothing is written into the repo.

## 10. Files and artifacts

- Added to the repo: `reports/xlt-m2-p1-report.md` (this report) and
  `xlt/qemu/p1/run.sh`.
- Not touched: `xlt/src/**`, `xlt/qemu/patches/**`, QEMU sources, t10's other
  working-tree changes. No QEMU tree or build output is inside the repo
  (`xlt/build/` is gitignored).
- Verification artifacts outside the repo:
  `/home/lshan/xlt/qemu-8.2.2-verify`, `/home/lshan/xlt/qemu-8.2.2-verify-build`,
  `/home/lshan/xlt/verify-libxlt`, `/tmp/qemu-verify-configure.log`,
  `/tmp/qemu-verify-build.log`, `/tmp/t11/` (`logs/`, fixture copies, bench logs,
  run.sh logs).

Final verdict: **PASS** for P1. The series is reproducible from a pristine tree,
delivers exact TCG parity for the P1 fixtures with deterministic PSCI shutdown, fails
cleanly on misuse, and leaves the M1 engine, tests and benchmark intact at 10.02x.

---

# Addendum (t16): re-verification after the accel-ownership fix

- Date: 2026-11-28
- Task: t16 [M2/P1 re-verify], attempt 1 (`733935d1-66a8-4e98-8692-418cc93d4eea`)
- Scope: the **updated** patch series, which supersedes the series verified in t11.
  The update moves guest startup from `xltm_init()` to `AccelClass::setup_post`
  (`xlt_setup_post()` → `xlt_machine_start_guest()`) and makes `-M xlt` without
  `-accel xlt` fail with `-M xlt requires -accel xlt`.
- Verdict: **PASS** — updated patches apply cleanly to another untouched QEMU 8.2.2
  tree, build, boot and parity are unchanged, and the new ownership/failure behaviour
  is exactly as specified.

## A1. Patches (final artifact set)

| patch | bytes | sha256 |
| --- | --- | --- |
| `0001-accel-add-xlt-accelerator.patch` | 5,197 | `bdfeb19f7f8d820e04c6ff48408a8717f1ec24dc7b4c34bcac6a28ea7a5713b4` |
| `0002-hw-arm-add-xlt-machine.patch` | 6,927 | `5efbec7540b9a9527c501ff675b807fb0aa32c27e10989329d358b618bdbf78c` |

The t16-verified patch bytes were 5,199/`3c306ef7…` and 7,097/`1fac438e…`; after that
verification the two patches were whitespace-normalized (removed trailing spaces from blank
context lines so `git diff --cached --check` is clean). Their code content is unchanged; both
apply to pristine QEMU 8.2.2 with `patch -p1 --dry-run` (exit 0, no rejects) and the resulting
tree is the one exercised by TCG/xlt parity above.

## A2. Fresh tree, apply checks, build

A new tree was used (not the t11 verify tree and not the working copy):

| item | value |
| --- | --- |
| tarball | `/home/lshan/xlt/qemu-8.2.2.tar.xz`, sha256 `847346c1…bc4f3` |
| tree | `/home/lshan/xlt/qemu-8.2.2-t16` (fresh unpack, 118 entries, `VERSION` 8.2.2) |
| build tree | `/home/lshan/xlt/qemu-8.2.2-t16-build` |
| libxlt archive | `/home/lshan/xlt/t16-libxlt/libxlt.a` (built fresh, 15,637,268 B) |

Apply checks (both requested methods, before applying anything):

- `patch -p1 -d /home/lshan/xlt/qemu-8.2.2-t16 --dry-run --batch --forward < <patch>`
  for both patches: every file `checking file …` succeeded, no rejects.
- from `/home/lshan/xlt` (the parent that contains the stray empty `.git`):
  `git apply --check --directory=qemu-8.2.2 <patch>`: both patches OK. Using
  `--directory` from the parent avoids the subdirectory-prefix trap described by the
  captain; this was confirmed to actually check the paths (unlike a plain
  `git apply --check` run from inside the tree in t11's first attempt).

The patches were then applied with `patch -p1 --batch --forward`; both applied, and a
search for `*.rej`/`*.orig` found none.

Build (logs `/tmp/qemu-t16-configure.log`, `/tmp/qemu-t16-build.log`):

- configure exit 0 with the same flag set as t11 (`--target-list=aarch64-softmmu`,
  docs/werror/gtk/sdl/vnc/opengl/spice/libssh/guest-agent disabled,
  `--extra-cflags`/`--extra-ldflags` pointing at the repo `xlt/src` and the fresh
  `t16-libxlt` archive).
- `ninja -j16 qemu-system-aarch64`: exit 0, **1971/1971 targets**, 92 s wall
  (`start_epoch` 1795878506 → 1795878598), load 4.32 → 12.08, **0 warning/error
  lines**, binary `qemu-system-aarch64` 111,644,464 B.
- `qemu-system-aarch64 --version` = `QEMU emulator version 8.2.2`;
  `-M help` lists `xlt  xlt AArch64-to-x86-64 translator (P1)`; `-accel help`
  lists `xlt`.

## A3. Serial parity and determinism

Each image was run twice on each side. Fixtures come from the repo
(`xlt/tests/fixtures/p1`, `xlt/tests/fixtures/qemu-smoke`); logs in `/tmp/t16/logs/`.

| fixture | TCG run 1/2 | xlt run 1/2 | byte-identical | rc |
| --- | --- | --- | --- | --- |
| qemu-smoke | `xlt-p1-ok` | `xlt-p1-ok` | yes (TCG1=TCG2=xlt1=xlt2) | 0 |
| fixture1 | `xlt-p1-fixture1` | `xlt-p1-fixture1` | yes | 0 |
| fixture2 | `xlt-p1-fixture2 fr=0x0000000000000090 ram=0x1122334455667788 el=0x0000000000000004` | same | yes | 0 |

stderr was empty on every run; walls: TCG 0.05 s, xlt 0.05–0.09 s. PSCI
`SYSTEM_OFF` still exits QEMU with status 0 under both accelerators.

## A4. Failure paths (new ownership behaviour)

| command | rc | stderr |
| --- | --- | --- |
| `-M xlt … -kernel fixture1.elf` (no `-accel`) | 1 | `qemu-system-aarch64: -M xlt requires -accel xlt` |
| `-M xlt -accel xlt` (no `-kernel`) | 1 | `qemu-system-aarch64: xlt: -kernel <elf> is required` |
| `-M xlt -accel nope …` | 1 | `qemu-system-aarch64: -accel nope: invalid accelerator nope` |
| `-M virt -accel xlt …` | 1 | `-accel xlt is only supported by -M xlt` + generic `failed to initialize xlt: Operation not permitted` |

No hang (all exited in ~0.03 s, far below the 20 s timeout). The new
`-M xlt`-without-accel rejection is present and matches the required message exactly.

## A5. M1 regression (working tree)

- `make -C xlt clean && make -C xlt`: exit 0, 25 s, 159 warnings all from
  `/usr/lib/llvm-18` headers, **0 from `xlt/src`/`xlt/tests`**, binary 10,047,624 B
  (see `/tmp/t16/m1-build.log`).
- `make -C xlt test`: exit 0, `decode_test: 481 checks passed`.
- Bench image rebuilt, sha256 `d28c71ec…`; `xlt/bench/run.sh` exit 0,
  total **9.68x** (TCG 433,332,490 ticks / 6933.31 ms vs xlt 44,757,991 / 716.12 ms;
  per test 1.92x, 45.71x, 10.12x, 10.93x, 1.39x), all 5 checksums match.
- CLI unchanged: `xlt 0.1` (rc 0), `--help` rc 0, `xlt run /nonexistent.elf` rc 1
  `xlt: cannot open /nonexistent.elf`.
- Host load during this section: ~7.7–8.5.

## A6. NOTES accuracy

`xlt/qemu/p1/NOTES.md` describes the new ownership correctly in its Components and
P1-limits prose: startup is owned by `AccelClass::setup_post` →
`xlt_machine_start_guest()`, the machine never starts the guest, and `-M xlt` without
`-accel xlt` is rejected during machine init with the exact message and exit 1. The
observed behaviour matches.

One stale sentence remains in the "P1 limits" section: *"the accelerator's
`create_vcpu_thread` starts the libxlt dispatcher directly."* In the updated series
`xlt_create_vcpu_thread()` is a no-op; the dispatcher is started from `setup_post`.
This is a documentation-only mismatch (behaviour is correct) and should be reworded,
e.g. "`create_vcpu_thread` is a no-op; `setup_post` starts the dispatcher".

## A7. Reproducible script update

`xlt/qemu/p1/run.sh` was extended for this series (script-only change, sha256
`75d8befb5b5c83c8955e6869c33688af1d43e2a18fd27af9321628ae3a199f12`):

- runs each of the three images twice on each side and compares the two runs per side
  (determinism) as well as TCG vs xlt (byte-for-byte parity);
- checks the exact expected line for each image;
- adds the new failure paths: `-M xlt` without `-accel xlt` (message
  `-M xlt requires -accel xlt`) and `-accel xlt` on `-M virt`;
- still defaults to the `build-xlt.sh` output and accepts `QEMU=`.

Result against the t16 build: all three fixtures parity/determinism OK, all four
failure checks OK, `run.sh: PASS`, exit 0 (`/tmp/t16-runsh.log`).

## A8. Verdict

**PASS** for the updated M2/P1 series: patches are byte-identical to the announced
artifact, apply cleanly to pristine QEMU 8.2.2 by both independent methods, build with
no warnings into a working `-M xlt`/`-accel xlt` binary (1971/1971 targets, 92 s), keep
exact TCG parity and determinism for all three images, reject misuse cleanly (including
the new `-M xlt`-without-accel path), and leave the M1 engine, tests and benchmark
intact (9.68x, 5/5 checksums). Only the NOTES sentence about `create_vcpu_thread`
needs a wording fix; no patch or source change is required for P1.

## A9. Files and artifacts

- Changed by this re-verification: `reports/xlt-m2-p1-report.md` (this addendum) and
  `xlt/qemu/p1/run.sh` (extended checks). No patch, `xlt/src/**` or QEMU source was
  modified.
- Artifacts outside the repo: `/home/lshan/xlt/qemu-8.2.2-t16{,-build}`,
  `/home/lshan/xlt/t16-libxlt`, `/tmp/qemu-t16-{configure,build}.log`, `/tmp/t16/`.
- Nothing committed; t8 owns the commit.
