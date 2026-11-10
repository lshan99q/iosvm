# Stage 19+20 — delivery self-check

**Date:** 2026-11-10 00:50Z (local 00:50).  **No boot / no new experiment** in this
check.  Goal: confirm the repo can serve as the stage 19+20 delivery baseline.

## 1. GPU / processes

- `qemu-system`, `host_glwire`, `boot-measure`, `four-cell`, `drive-scroll`: **none**.
- Tesla V100-SXM2-16GB: **0 MiB / 16384 MiB** used.

## 2. Images

| item | value |
|---|---|
| `userland/system.apfs` | **6be494f16775402cab1fa2120ca24112**, 2,476,752,896 B, 2026-11-09 23:07:50Z (C1-limits control rebuild; the working C1 baseline) |
| `userland/root.apfs` | **785bcbac82c9a117684b2d40b7375d48**, 5,918,720 B, 2026-11-09 13:24Z (restored pre-build P4-era root; not the build-regenerated root) |
| bad E1 system `15167c8a` | backup `/tmp/system.apfs.bak-s20-bad` **and** durable `/host/iosvm/logs/s20-e1-watchdog/system.apfs.bak-s20-bad` (2,476,752,896 B, md5 `15167c8a01fe2e074caad610686df895`) |

Notes: the old C1 image `8bf5f838` was overwritten by the control rebuild and is
not archived; the P2d root `b59fcf66` is not in the tree and is not archived (see
§4).  The current root is the one restored before the control builds (P4 cell3
root); it is not polluted by the build, but it is not a byte-exact P2d root.

## 3. Build / tests

| command | rc | result |
|---|---|---|
| `make test-open` | **0** | PASS: `provenance-check: clean (235 tracked files)`; `proto: pass` |
| `make test` | **2** | `test-open` passes; `test-gpu` fails: `[error] VK_ICD_FILENAMES names /usr/share/vulkan/icd.d/lvp_icd.json: No such file or directory` (make `Error 4`) |

The `make test` failure is the **environment** (lavapipe ICD not installed/pointed),
not a code failure.  Full logs: `/tmp/delivery-test-open.log`, `/tmp/delivery-test.log`.

## 4. Archives under `logs/`

37 `s19o-*` / `s20-*` entries (36 dirs + the `s20-e1` symlink).  One-line purpose
from each `report.md` headline:

| dir | purpose |
|---|---|
| s19o-p0-3 | s19o-p0-3 — P0 第三发 + P1「90pt 背板」证据（一发双用） |
| s19o-p1-dots | (no report.md) |
| s19o-p1a | s19o-p1a — P1 诊断：GLWIRE_PROGRAM_SKIP=13（标签噪声带来源） |
| s19o-p1b | s19o-p1b — P1 诊断：GLWIRE_TC0_SANITIZE=1（不 skip） |
| s19o-p1c | s19o-p1c — P1 诊断：GLWIRE_ATLAS_DUMP 看 glyph tile 底部 |
| s19o-p1d | s19o-p1d — P1 诊断：GLWIRE_GLYPH_TRIM=6 |
| s19o-p1e | s19o-p1e — P1 诊断：GLWIRE_GLYPH_TRIM=6 GLWIRE_GLYPH_TRIM_FROM=start |
| s19o-p2 | s19o-p2 — P2 壁纸：重建 system.apfs（LaunchAngels → /redwood/LaunchAngels） |
| s19o-p2b | s19o-p2b — SpringBoard 层树诊断（rc.p2diag，不改 system.apfs） |
| s19o-p2c-alt | s19o-p2c-alt — LaunchDaemon + rwtouch 触发 SpringBoard 层树 |
| s19o-p2c-alt2 | s19o-p2c-alt2 — notify 通道验证（unlock + switcher） |
| s19o-p2c-timer | s19o-p2c-timer — `layers_at=900` 定时器层树 |
| s19o-p2d | s19o-p2d — 4 个 stock LaunchDaemon job（profiled / powerlogHelperd / Video / ImageConversion） |
| s19o-p2e-c2 | s19o-p2e-c2 — `launchd_proxyless_spawn=0`（强制 xpcproxy） |
| s19o-p2e | s19o-p2e — launchd limits `ExtensionWatchdogTimeout=120` |
| s19o-p3-fallback | s19o-p3-fallback — fallback 占位验收（第一发，202 白块替换） |
| s19o-p3-fallback2 | s19o-p3-fallback2 — fallback 视觉验收（仅 RECT，无 202） |
| s19o-p3-fallback3 | s19o-p3-fallback3 — host-replay 模式 fallback 视觉验收 |
| s19o-p3-fallback4 | s19o-p3-fallback4 — guest-side placeholder（A1 旧库字符串） |
| s19o-p3-fallback5 | fallback5 — guest-side widget placeholder（A1 库 110,656） |
| s19o-p3-surface-enum | s19o-p3-surface-enum — widget 面到底是几号 surface？（枚举探针） |
| s19o-p3a | s19o-p3a — P3 MVP：surface 3 → opcode 202 → host 合成 |
| s19o-p3b-d1 | s19o-p3b-d1 — CAContext snapshot 探针（`widget_cacontext=auto`） |
| s19o-p3b-d2 | s19o-p3b-d2 — 延迟 `allContexts` + 容错 selector |
| s19o-p3b-d3 | s19o-p3b-d3 — SpringBoard 侧 CAContext 探针 |
| s19o-p4-cells | P4 四格表 — 第二次实跑（容器重启后） |
| s19o-v2 | (no report.md) |
| s19o-v3 | s19o-v3 — P0 标签文字一发（QEMU_SMP=1, ZERO_RAM=1, UI_GRACE=300） |
| s19o-v4 | s19o-v4 — viewport override 一发（QEMU_SMP=1, ZERO_RAM=1, UI_GRACE=300, MINSHOT=200000） |
| s19o-v5 | s19o-v5 — 根因修复验证 + 同帧 diffdump（QEMU_SMP=1, ZERO_RAM=1, UI_GRACE=300, MINSHOT=200000） |
| s19o-v6 | s19o-v6 — P0 验收发（NO_PANEL + panel 修复；QEMU_SMP=1, ZERO_RAM=1, MINSHOT=200000） |
| s19o-v7 | s19o-v7 — 换运行口径拿真 panel（glwire_local=1 + NO_PANEL；QEMU_SMP=1, MINSHOT=200000） |
| s19o-v8 | s19o-v8 — P0 复现发 2（与 v7 完全同配方） |
| s19o-v9 | s19o-v9 — P0 复现发 3（与 v7/v8 完全同配方） |
| s20-e1-control | s20 E1 control — 恢复 C1 limits，验证 `SYSTEM=1` 重建 |
| s20-e1-watchdog | s20 E1 — `ExtensionWatchdog=false`（30 分钟限时） |
| s20-e1 | s20 E1 — `ExtensionWatchdog=false`（30 分钟限时） |

**Gaps / missing:**

- `s19o-p1-dots` has **no report.md** (the analysis is in the sibling `s19o-p1-dots.md`).
- `s19o-v2` has **no report.md** (raw P0 v2 data; no narrative).
- The P2d root `b59fcf66…` is **not archived** (image; only measurement logs are under `logs/`).
- The old C1 system `8bf5f838…` is overwritten by the C1-limits control rebuild `6be494f1…`; the delivery baseline uses `6be494f1`.
- The bad E1 system `15167c8a…` is quarantined in `logs/s20-e1-watchdog/system.apfs.bak-s20-bad` (and `/tmp`).

No other `s19o-*` / `s20-*` directory is missing; `s20-e1` is a symlink to
`s20-e1-watchdog`.  Loose `s19o-*.md` / `*.patch` files at `logs/` top level are
the per-run notes/patches, not separate archives.

## 5. Branches

- `iosvm-exp` == `main` at **f0e54aa** before this self-check commit
  (`git rev-list --left-right --count main...iosvm-exp` = `0 0`).
- Origin: `origin/iosvm-exp` was updated `cac2a73..f0e54aa`; this self-check commit
  is pushed to **both** `main` and `iosvm-exp`, so both are in sync and there are
  no unpushed commits.
- Worktree status: only untracked `host/vkbridge` (submodule content) in `/host/iosvm`
  and `?? .scion` in the `iosvm-exp` agent worktree; no tracked changes.

## 6. Conclusion

**Yes — the repo can serve as the stage 19+20 delivery baseline**, with the
documented gaps: `make test-gpu` needs the lavapipe ICD (environment), the P2d
root `b59fcf66` is not archived, and two log dirs lack a `report.md`.  Images,
archives and branches are otherwise consistent, no experiment process/GPU is
left, and the bad E1 `15167c8a` image is quarantined.
