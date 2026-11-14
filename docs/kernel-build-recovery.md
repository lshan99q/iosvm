# Kernel build recovery（2026-11-14，未完成）

接手时的状态：`BUILD/obj` 预编译对象散失，`mig` 阶段大量 `.defs` 报 2 errors；`xnu-clang`
包装脚本把 SDK `Kernel.framework/Headers` 插到所有 `-I` 之前，破坏 tree 的 `mach/vm_types.h`。

## 已定位并修好的部分

1. **mig 的 .defs 失败不是 KERNEL_PRIVATE/-I 问题**，而是 `MIGCC` 沿用了 `xnu-clang` 包装脚本：
   - `MakeInc.cmd:196` 默认 `MIGCC := $(CC)`，而 `CC=xnu-clang` 会强制 `-include xnu_clang_compat.h`；
   - mig 的预处理器把这个 C 兼容头当普通头，报 `clock_priv.defs`/`upl.defs` 的连带错误。
   - **修法：`export MIGCC=/usr/bin/clang`**（只用它做 `-E`，不参与普通 C/C++ 编译）。
   加上后 `.defs` 全部通过，不再出现 `upl_t not defined`。
2. **SDK 软链是断的**：`xnu-deps/sdk/iPhoneOS16.5.xnu.sdk` 里大量文件指向 `/home/lshan/theos/...`。
   - 用 `toolchain/theos/sdks/iPhoneOS16.5.sdk` 全量替换后，`TargetConditionals.h`、`Availability.h` 才可用。
3. **`xnu_clang_compat.h`/头顺序**：
   - 不要再把 `Kernel.framework/Headers` 或 `SDK/usr/include` 用前置 `-I` 插进去；会抢 tree 的
     `mach/vm_types.h`、`unistd.h`。
   - `xnu-clang` 应保持 **`-isysroot $XNU_SDK_PATH`**，需要时只加
     **`-idirafter $XNU_SDK_PATH/usr/include`** 和 **`-idirafter $XNU_DEPS/libdispatch`** 兜底。
   - `libdispatch` **不能放成 `-I`**：会把 `os/object.h` 换成用户态版，丢 `fastpath/slowpath`。
4. **容器工具**：
   - `xnu-deps/host/bin/csh` 原是断链；已用 `xnu-deps/tcsh` 源码编出 tcsh 并放到 host/bin。
   - `xnu-deps/host/bin/xnu-clang++` 原指向不存在的 `/usr/bin/clang++`；包装脚本已 fallback 到
     `/usr/bin/clang`（C++ 链接无 libstdc++，内核不用）。
   - 链接 cctools 需要 `LD_LIBRARY_PATH=/host/iosvm-work/toolchain/lib`（`libBlocksRuntime.so`）。
5. **mawk 的 `\(`**：`bsd/kern/makesyscalls.sh` 用 mawk 会生成字面 `\(sy_call_t *\)`；改成
   `(sy_call_t *)` 后才生成合法 `init_sysent.c`。

## 现在的卡点（不是 A1 代码）

执行到 **全部 C/C++ 对象编译完成**，最后 `LD mach.development.bcm2837.unstripped` 失败：

- `ld: library not found for -lfirehose_kernel`（以及 `-lpthread_kext`）；
  `LD_KERNEL_ARCHIVES` 默认要这两个 SDK 库，但 `xnu-deps/sdk/.../usr/local/lib/kernel` 里没有；
- 临时用空归档/force_load 绕过库缺失后，出现真正的未定义：
  - `___firehose_buffer_tracepoint_reserve`、`___firehose_kernel_configuration_valid`、
    `___firehose_merge_updates`、`_pthread_start`（需要真实 firehose/pthread kext 库，不是空 stub 能解决）；
  - `OSMetaClass::_RESERVEDOSMetaClass0`（C++ vtable；本轮 BUILD 里 `OSMetaClass.cpo` 未提供该符号，
    需要原来的预编译 C++ 对象或正确重编 `OSMetaClass.cpp` 的 ABI）。

**结论：构建链已从“mig 全炸”推进到“对象全编完、只差最终链接的两个 SDK 库 + 一个 C++ 符号”，
但 30 分钟内没有产出 `hakios-kernel`。不要把空 stub / force_load 当成修复。**

## 可复现的命令（当前环境）

```bash
cd /host/iosvm-work/hakios-xnu-kernel
PATH=$PWD/xnu-deps/host/bin:$PATH \
MIGCC=/usr/bin/clang \
LD_LIBRARY_PATH=/host/iosvm-work/toolchain/lib \
CCTOOLS_LD64=/host/iosvm-work/toolchain/cctools/bin/aarch64-apple-darwin11-ld \
make kernel -j16
```

`hakios-kernel` 已从 git 的 `prebuilt/hakios-kernel` 恢复（不是这次新建的）。

## 需要的下一步

1. 找回/重建 `libfirehose_kernel.a`、`libpthread_kext.a`（libdispatch/kext 工程产物），或从原始
   BUILD 分发物恢复；
2. 确认 `OSMetaClass.cpo` 的 ABI 与其余 C++ 对象一致（最稳的是恢复整套预编译 `BUILD/obj`，
   或从干净 XNU 源全量重编 C++）；
3. 再按 `tools/kernel-rebuild.sh` 的 `__TEXT vmaddr=0xfffffff007004000`、`entry_va≠0`、
   marker（`virtio-gpu: slot`、`hakios: exec pid=`）验收。

A1 驱动源码与本地 commit `ae084ae`、补丁 `stage23-a1.patch` 未受影响；本次未 boot。

## 第二轮（2026-11-14 08:50）：尝试手工编两个缺失库，仍未闭合

按 lead 指令，在 libdispatch/libpthread 源码上手工编：

```bash
xnu-clang -c -O2 -D KERNEL=1 -D OS_FIREHOSE_SPI=1 -D XNU_KERNEL_PRIVATE \
  -I $XNU_SRC/bsd -I $XNU_SRC/osfmk -I $XNU_SRC/libkern -I $XNU_SRC/EXTERNAL_HEADERS \
  -I $XNU_SRC/BUILD/obj/EXPORT_HDRS/{osfmk,bsd,libkern} -I $XNU_SRC/bsd/sys \
  -idirafter $XNU_DEPS/libdispatch \
  -o /tmp/firehose_buffer.o $XNU_DEPS/libdispatch/src/firehose/firehose_buffer.c
```

结果：`libkern/os/atomic_private_impl.h` 报 `unknown type name 'memory_order'`，
即使显式 `-include stdatomic.h` 也不消失。说明这棵 XNU 的 `os/atomic.h`、libdispatch 的
`src/firehose` 与当前 SDK/clang 组合不是同一套（旧 BUILD 的预编译对象本来就不该重编）。
**不能把 libfirehose/libpthread 当普通用户态库编**。

同时最终链接还缺 `OSMetaClass::_RESERVEDOSMetaClass0`：`OSMetaClass.cpo` 里没有该符号，
不是简单改链接顺序能解的，需恢复整套同 ABI 的 C++ 对象或全量重编。

**结论：当前 BUILD/obj 已能编完所有 C/C++ 对象，但最终链接的两个 SDK 库和 C++ 符号不可在本容器
由当前源凑齐；继续硬编会破坏预编译对象的 ABI。已恢复 `hakios-kernel` 为 git 的 `prebuilt/hakios-kernel`，
未产出新内核，未 boot。**

### 给下一位的最小下一步

1. 从原始分发物/备份恢复 `BUILD/obj/EXPORT_HDRS` 和整套 `BUILD/obj/DEVELOPMENT_ARM64_BCM2837`
   （尤其 `libkern/DEVELOPMENT/*.cpo`、`libdispatch` 的 `libfirehose_kernel.a`、`libpthread_kext.a`）；
2. 或找一台有完整 Apple SDK + xcodebuild 的机器，按 `libdispatch.xcodeproj`/`libpthread.xcodeproj`
   的 `Kernel`/`kext` target 编两个库，再装到 `$(SDKROOT)/usr/local/lib/kernel/`；
3. 然后只跑 `tools/kernel-rebuild.sh` 的“删链接产物 + 重链”，不要重编对象。
