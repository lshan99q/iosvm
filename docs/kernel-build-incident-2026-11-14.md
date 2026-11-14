# 内核构建链事故与恢复方案（2026-11-14）

## 一句话

重建内核时误刷新了 `BUILD/obj/EXPORT_HDRS` 的时间戳，增量构建失效；**预编译对象集散失，本环境无法从零重建**（mig 阶段失败）。**内核源码本身没坏**，坏的是构建状态。

## 事实

| 项 | 状态 |
|---|---|
| `hakios-kernel`（含 exec/exit/srmap 追踪，md5 `203c3df8`） | **丢失**（原先只在 BUILD 链接产物 + payload 里） |
| `qemu/payload.bin` 内的 kernel | `d250469e`＝`hakios-kernel.prebuilt`（2026-11-01，**无追踪**） |
| `hakios-kernel.prebuilt` / `.new-2359` | 在，均可启动，无追踪串 |
| virtio-gpu A1 驱动源码 | 在：`bsd/dev/hakios.virtio_gpu.c`（9.5 KB）+ `bsd/conf/files` + config 已收录 `HAKIOS_VIRTIO_GPU` |
| BUILD/obj 预编译对象 | 散失（多次 mv + 全量重编后只剩约 200 个 .o） |

## 为什么重建不通（逐条实测）

1. **只能在 `iosvm--iosvm-code` 容器里编**：只有该容器同时具备 `/host/iosvm-work` 挂载和 `/usr/bin/clang`；exp/docs/lead 无 clang；host 无 `/host` 路径，而生成的 Makefile 里 2052 个文件用 `/host/...` 绝对路径 → host 侧 make 必然 "No rule to make target"。
2. **环境缺包**（已装）：`uuid-dev`、`bison`、`flex`。
3. **`xnu-deps/host/bin/xnu-clang` 把 SDK Kernel.framework/Headers 插在所有 `-I` 之前** → `<mach/vm_types.h>` 解析到 KPI 版，缺 `vm_tag_t`/`vm_allocation_site_t` → `libkern/OSKextLibPrivate.h`（`#ifdef XNU_KERNEL_PRIVATE` 段）编译失败。
   - 试过把 SDK 移到参数之后：OSKext 通过，但 `libdispatch/os/object.h`、firehose 相关 TU 反炸 → 原顺序是权衡过的，不能简单调换。
4. **mig 阶段失败**（当前卡点）：`build_exporthdrs_mi` 大量 `.defs` 报 “2 errors found”，如 `clock_priv.defs:53`、`host_priv.defs:92`；`upl.defs:78 type 'upl_t' not defined`，而 `upl_t` 在 `osfmk/mach/mach_types.defs` 的 `#ifdef KERNEL_PRIVATE` 段内 → **mig 预处理没拿到 `KERNEL_PRIVATE` 或没找到树内 `mach_types.defs`**。
5. `tools/kernel-rebuild.sh` 的常规路径只做「删链接产物 + 重链」，**从不重编对象** → 这套 BUILD 是随项目分发的预编译产物，环境本不具备全量重建能力。

## 恢复方案（按优先级）

1. **修 mig 包装**（code 正在做）：让 `mig` 先搜 `BUILD/obj/EXPORT_HDRS/osfmk` 并带 `-DKERNEL_PRIVATE -DXNU_KERNEL_PRIVATE`；目标命令
   `cd /host/iosvm-work/hakios-xnu-kernel && CCTOOLS_LD64=/host/iosvm-work/toolchain/cctools/bin/aarch64-apple-darwin11-ld make kernel -j16`
   自检 `__TEXT vmaddr=0xfffffff007004000`、`entry_va≠0`、marker 串。
2. **恢复预编译对象**：从项目分发包/镜像层里找回原始 `BUILD/obj`（含 943+ `.o`、128 `.cpo`）——若有 tarball 或他人工作副本，这是最快的路。
3. **放弃追踪、先保可用**：`hakios-kernel.prebuilt` 可启动；实验（fps、画面、扩展 check-in）照跑，只是内核态追踪不可用。
4. **virtio-gpu 测试**依赖 1 或 2。

## 纪律（写进守则）

- **不要 touch/移动 `BUILD/obj/EXPORT_HDRS`**；要全量重编先备份整个 `BUILD/obj`。
- 内核构建只在 `iosvm--iosvm-code` 容器内、`/host/...` 路径下进行；host 侧 make 永远失败。
- 每次改动 kernel 源码后，先确认 `xnu/files`、`bsd/conf/files`、config 三处一致，再编。
