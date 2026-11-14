# Kernel build recovery（终局，2026-11-14）

> **结论：本环境不具备内核全量重建能力。**
> 内核冻结在 **`prebuilt/hakios-kernel`**（git prebuilt，2026-11-01；**无 virtio-gpu、无追踪**）。
> Stage 23 的 A1 驱动不靠重建内核推进，已作为补丁保留：
> `docs/patches/0001-stage23-A1-virtio-gpu-discovery-controlq-GET_DISPLAY.patch`。
> 本文是终局边界，不是“再试一次”的清单。

## 1. 事故经过

1. 重建时刷新了 `BUILD/obj/EXPORT_HDRS` 的时间戳，增量构建失效；
   **预编译对象集散失**（多次 mv + 全量重编后只剩约 200 个 `.o`）。
2. 带追踪的内核 **`203c3df8` 丢失**（原先只在 BUILD 链接产物 + payload 里）；
   `qemu/payload.bin` 里是 `d250469e` = `hakios-kernel.prebuilt`（11-01）。
3. 仓库里的 **`hakios-kernel` 是 git prebuilt（11-01）**：
   能启动，但**没有 virtio-gpu、没有 `hakios: exec`/hostport/host_info 追踪**。
4. A1 驱动源码本身在（`bsd/dev/hakios.virtio_gpu.c` + `bsd/conf/files` + config），
   只是无法在这套环境里链进新内核。

## 2. 已经修过的部分（对象能编，但链不出来）

- **mig**：`MIGCC=/usr/bin/clang`；不再让 `xnu-clang` 的兼容头污染 `.defs` 预处理。
- **SDK 软链**：用 `toolchain/theos/sdks/iPhoneOS16.5.sdk` 替换断链。
- **头顺序**：`Kernel.framework/Headers` 不能前置；保持 `-isysroot` + `-idirafter`，
  `libdispatch` 不能当 `-I`。
- **容器工具**：补 `csh`（tcsh 源码）、`xnu-clang++` fallback 到 clang、
  `LD_LIBRARY_PATH=.../toolchain/lib`（BlocksRuntime）。
- 结果：**全部 C/C++ 对象能编译完成**；卡在最终链接，不是 A1 源码错误。

## 3. 硬限制（为什么不能在本环境闭合）

### 3.1 最终链接缺三样，当前源凑不齐

- `libfirehose_kernel.a`：SDK 目录没有；libdispatch 的 `src/firehose` 与这棵 XNU 的
  `os/atomic.h` 不匹配（`memory_order` 未定义，`-include stdatomic.h` 也不消失），
  **不能把 firehose 当普通用户态库重编**。
- `libpthread_kext.a`：没有直接构建 target；`_pthread_start` 等符号需要它。
- `OSMetaClass::_RESERVEDOSMetaClass0`：`OSMetaClass.cpo` 不提供该符号；
  需同 ABI 的预编译 C++ 对象或全量重编，不能靠链接顺序/空 stub 解决。
- 空归档/force_load 只能把错误推迟，**不是修复**。

### 3.2 上游内核仓只读

- `hakios-xnu-kernel` 上游对本环境 **只读**：写操作返回 **403 / Permission denied**。
- **等权限没有意义**；不要把它列为可重试项。

### 3.3 没有完整 Kernel SDK / xcodebuild

- 本环境没有完整 Apple Kernel SDK，也没有可用的 `xcodebuild`；
- 缺匹配的**预编译产物**（整套 `BUILD/obj`、`libfirehose_kernel.a`、
  `libpthread_kext.a`、`OSMetaClass.cpo`）。

## 4. 要推进 23A，必须先满足其一（硬前置）

1. **匹配的预编译产物**：`libfirehose_kernel.a`、`libpthread_kext.a`、
   `OSMetaClass.cpo`（同 ABI）；
2. **整套 `BUILD/obj`**（含 `EXPORT_HDRS` 与 `DEVELOPMENT_ARM64_BCM2837`）；
3. **有 Kernel SDK 的 xcodebuild 环境**（能按 libdispatch/libpthread 的
   `Kernel`/`kext` target 编出上述库与 C++ 对象）。

以上任一不满足时，**不要尝试重编/重链内核**；`tools/kernel-rebuild.sh`
只做“删链接产物 + 重链”，**不会重建对象**，也不能补这三个缺口。

## 5. 冻结状态

- **可启动内核**：`prebuilt/hakios-kernel`（11-01，无 virtio-gpu、无追踪）。
- **A1 驱动**：只以补丁保留，路径见文首；不在本环境链入内核。
- **内核仓**：只读，不 push、不改。
- **不再有**“修一下 mig / 换头顺序 / 手工编库”的下一轮；这些都已试过，
  结论是环境边界，不是待办。

## 已解决（2026-11-14 13:30，owner + code 验证）

owner 修好了构建链，关键项：

1. `xnu-deps/sdk/.../usr/local/lib/kernel/` 缺的两个库：
   - `libpthread_kext.a` 用 `xnu-deps/build/pthread/*.o` 补出；
   - `libfirehose_kernel.a` 用 `xnu-deps/build/firehose/firehose_buffer.o` 补出。
2. `EXTERNAL_HEADERS/AvailabilityInternal.h` 第 23339–23341 行被截断，补回
   `#define __API_AVAILABLE5/6/7(...)` 前缀。
3. SDK `Kernel.framework/.../libkern/c++/OSMetaClass.h` 缺 `XNU_KERNEL_PRIVATE` 分支，
   导致 PADDING=1 与 IOService.cpp 的 0 不一致；补同树分支 PADDING=0，解决
   `_RESERVEDIOService0..47`。
4. `xnu-deps/host/bin` 的 `libtool/strip/nm/ar/ranlib/otool/lipo` 死链改到
   `toolchain/cctools/bin/aarch64-apple-darwin11-*`，并加 `libtool` 包装把 `-ca` 翻译成 `-static`。
5. 构建必须在 `iosvm--iosvm-code` 容器、`source ./xnu-env.sh`、`BUILD_DSYM=0`，并显式传
   `STRIP/NM/AR/RANLIB`；`DSYMUTIL=/bin/true` 可跳过 dSYM 的 `--minimize`（dSYM 步骤仍可能报
   `mv` 失败，但 `mach.development.bcm2837.unstripped` 已完整，可直接作为 `hakios-kernel`）。

**本机产出**：

```text
hakios-kernel md5 79b3ad1e7be8998d6bcfa07fe0cc5557
__TEXT vmaddr 0xfffffff007004000
entry         0xfffffff00745d4a8
markers       "virtio-gpu: slot" x4, "hakios: exec pid=", "hakios: exit pid="
```

## A1 响应解析修复（2026-11-14）

ext42 的 `GET_DISPLAY_INFO response 0` 不是 host 没回，而是 **controlq 双描述符没设
`VIRTQ_DESC_F_NEXT`**：

- `desc[0]`（request）`flags` 原本是 0，`next=1` 不会被设备跟随；
- 设备只看到 request 描述符，responses 描述符从未写入，`used->idx` 仍前进；
- 所以读到的是 KMA_ZERO 的 0。

修法：

```c
#define DESC_F_NEXT 1
desc[0].flags = DESC_F_NEXT;   /* request -> response */
desc[1].flags = DESC_F_WRITE;
```

并在读到 `used->idx != 0` 后再加一次 `dmb(0x9)`，确保 response 在 idx 之前可见。
补丁：`docs/patches/0002-stage23-A1-fix-response-descriptor.patch`。

下一发判据：`virtio-gpu: display 0: 750x1624 enabled=1`。
