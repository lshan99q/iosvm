<!-- 第十九阶段的运行环境基线。不是实验记录，不替代 docs/gpu-acceleration-stage19-p0-runbook.md。 -->

# IOSVM 第十九阶段运行环境与恢复手册

> 建立时间：2026-11-07 15:58
> 适用：`iosvm-exp` / `iosvm-code` 容器在镜像升级、重启或重建后的环境核对与恢复；也用于判断一次「跑不起来」到底是配方问题还是环境问题。
> 基线：lead 容器实测（2026-11-07 15:5x）+ 本手册写作容器的对照观测。
> 本文只回答：正常容器必须有什么、坏了长什么样、怎么逐项查、按什么顺序恢复。

---

## 零、边界

1. **只在编排侧恢复挂载与设备。** 容器内 `CapEff=0000000000000000`，不能 `mount`、不能 `mknod`；在容器里尝试这些命令只会得到 `Operation not permitted`，不会修好环境。
2. **本文只写文档，不跑 qemu、不提交日志/截图/镜像。** 检查命令里唯一会启动的进程是 `host_glwire` 的 5 秒冒烟（不连 qemu、不读客人机），见 §3.9。
3. **不声称 P0 通过。** 环境恢复只证明「能开始实验」；A1/v3 的结论仍按 `docs/gpu-acceleration-stage19-p0-runbook.md` §三/§四 判。
4. **不要把「容器起来了」当成「环境好了」。** 15:5x 的 exp/code 新容器是「全裸」：`/host`、GPU 节点、EGL 库/头文件全缺；lead 容器有 `/host`、GPU、PIL，但缺 ImageMagick/numpy；本手册写作容器有 `/host` 和 GPU，却缺 `libEGL.so.1`、PIL、ImageMagick、numpy。**必须逐项过清单，不能只看一项。**

---

## 一、正常容器必须有什么

### 1.1 总表（lead 容器实测）

| 类别 | 必须项 | 正常值 / 期望 |
|---|---|---|
| 挂载 | `/host/iosvm` | **rw**；对外仓库（`docs/`、`tools/glwire/`）；`git rev-parse --short HEAD` = `f2035c6` 或其后 |
| 挂载 | `/host/iosvm-work` | **rw**；含 `hakios-xnu-kernel/qemu/efi64.fd`、`vars.fd`、`userland/root.apfs`、`userland/system.apfs` |
| 挂载 | `/host/ipsw` | **ro**；IPSW 归档，仓库不带 |
| GPU | `/dev/nvidia0`、`/dev/nvidiactl`、`/dev/nvidia-uvm` | 字符设备；本机另有 `nvidia1`、`nvidia-modeset`、`nvidia-uvm-tools`、`/dev/dri/renderD129` |
| GPU | `nvidia-smi` | `Tesla V100-SXM2-16GB`，驱动 `580.178.04`，空闲时 `0 MiB` |
| 库 | `libEGL.so.1`、`libGL.so.1` | GLVND/NVIDIA；`ldconfig -p` 能查到 |
| 库 | `libEGL_nvidia.so.0` | NVIDIA EGL vendor；`/usr/share/glvnd/egl_vendor.d/10_nvidia.json` 存在 |
| 构建 | `/usr/include/EGL/egl.h`、`/usr/include/EGL/eglext.h`、`libEGL.so`、`libGL.so` | 没有头文件时 `make host_glwire` 会报 `EGL/egl.h: No such file` |
| 工具 | `python3` + PIL | Python 3.x；PIL **11.1.0**（lead 容器已临时装 `python3-pil`，未进仓库） |
| 工具 | ImageMagick | `magick -version` → **7.1.1**；**便利工具，不是 P0 硬依赖**（lead 当前未装） |
| 工具 | `make`、`cc` | 能编译 `tools/glwire`（≥C99） |
| **P0 C4 硬依赖** | numpy | `diffdump.py`/`bgra2png.py`/`frame_match.py`/`panel_probe.py` 都 import；**没有 numpy 时同帧 diff 只能记 `n/a`，P0 不能判通过** |
| 容器能力 | `CapEff` | 通常为 `0000000000000000`；**不能靠容器内 mount/mknod 恢复** |

### 1.2 三个挂载分别装什么

| 路径 | 内容 | 本机实测 |
|---|---|---|
| `/host/iosvm` | 对外仓库（文档 + `tools/glwire`），**rw** | `ext4 rw`，HEAD `7625f46`（含 `f2035c6`） |
| `/host/iosvm-work` | 虚拟机工程（内核、qemu、userland），**rw** | `ext4 rw`；`root.apfs` 5,902,336 B；`system.apfs` 2,476,748,800 B |
| `/host/ipsw` | IPSW 归档，**ro** | `ext4 ro`；`iPhone10,3,...16.5...ipsw`、`iPhone18,2,...26.6.2...ipsw` |

为什么 `/host/iosvm-work` 必须 rw：`boot-measure.sh` 会删/重建 `qemu/ram.img`，`run-darwin.sh` 会写 `qemu/payload.bin`；只读挂载会在启动前就失败。
为什么 `/host/ipsw` 只读：仓库不带 Apple 固件，容器只借用；任何构建都不应该写它。

### 1.3 GPU 节点

正常（lead 容器）：

```text
/dev/nvidia0
/dev/nvidiactl
/dev/nvidia-uvm
```

本机额外有（正常变体，不是故障）：

```text
/dev/nvidia1
/dev/nvidia-modeset
/dev/nvidia-uvm-tools
/dev/dri/renderD129
```

**编号不是判据，能力才是**：`nvidia-smi -L` 能列出 V100，且 `host_glwire` 冒烟能到 `listening on`，就说明节点可用。
本手册写作容器就是 `/dev/nvidia1` 而不是 `nvidia0`，`nvidia-smi` 仍正常——所以清单里遇到编号不同，先看 `nvidia-smi -L`，不要直接判环境坏。

### 1.4 图形库与 GLVND 注入

正常镜像自带：

```text
libEGL.so.1
libGL.so.1
libEGL_nvidia.so.0
/usr/share/glvnd/egl_vendor.d/10_nvidia.json
```

如果镜像**不自带** GLVND，但宿主/镜像里有一份解开的 GLVND，可把运行库目录挂进容器，例如 A1 用的：

```text
/tmp/glvnd/usr/lib/x86_64-linux-gnu
```

注入时必须写进 `HOST_ENV`（`boot-measure.sh` 用 `env ${HOST_ENV} ./host_glwire` 启动宿主 half）：

```sh
HOST_ENV="LD_LIBRARY_PATH=/tmp/glvnd/usr/lib/x86_64-linux-gnu GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19p0-1/label GLWIRE_DRAW_LOG=1"
```

**不能只 `export LD_LIBRARY_PATH` 就以为够了**：`export` 只影响当前 shell；`HOST_ENV` 才是 harness 传给 `host_glwire` 的那一份。外层 `export` 可以同时保留（qemu 侧或 wrapper 可能需要），但 `HOST_ENV` 里必须有。

GLVND 只解决**运行**。要 `make host_glwire` 仍然需要 `EGL/egl.h`、`EGL/eglext.h` 与 `libEGL.so`/`libGL.so` 链接名；头文件缺失时 `LD_LIBRARY_PATH` 不会让编译通过，必须换带 `libegl-dev`/`libgl-dev` 的镜像，或直接使用仓库里已编译且与 HEAD 匹配的 `host_glwire`（并核对 build marker）。

### 1.5 工具：PIL、ImageMagick、numpy

| 工具 | 检查 | 期望 | 用途 / 失败后果 |
|---|---|---|---|
| Python + PIL | `python3 -c 'import PIL; print(PIL.__version__)'` | `11.1.0` | 截图/像素脚本；lead 容器已临时装 `python3-pil`（未进仓库） |
| ImageMagick | `magick -version \| head -1` | `ImageMagick 7.1.1-...` | 截图裁剪/转换；**便利工具，不是 P0 硬依赖**；lead 当前未装 |
| numpy | `python3 -c 'import numpy; print(numpy.__version__)'` | **不保证，但 P0 C4 必须有** | `diffdump.py`、`bgra2png.py`、`frame_match.py`、`panel_probe.py` 都需要；缺失时同帧 diff 记 `n/a`，P0 不能判通过 |

**numpy 是 P0 C4 的硬依赖，不是「可选优化」**：本文的检查脚本不依赖 numpy；但既有 harness 里 `diffdump.py`、`bgra2png.py`、`frame_match.py`、`panel_probe.py` 都 `import numpy`。P0 的 C4（同帧 `diffdump --tolerance 8`）直接依赖 `diffdump.py`。如果容器没有 numpy，**C4 不能执行、P0 不能判通过**；证据表只能记 `n/a`，不能把「没装 numpy」写成「diff 通过」。恢复时要么装 numpy（记录版本），要么换带 numpy 的镜像；不要改脚本去绕过。

### 1.6 两种 diff 不要混：截图回归 vs P0 同帧

环境手册只记录口径，不改变 P0 的判据。两个“diff”回答的是不同问题（两张输入图分别在 `logs/s19n-repro/` 与 `logs/s19o-v2/`，都不进 git）：

| 名称 | 比较对象 | 用途 | 实测 / 期望 |
|---|---|---|---|
| `imgdiff`（**截图回归**，lead 提供） | `s19n-repro-shot.png` vs `s19o-v2-shot.png` 的整张截图 | 判断改动前后界面是否稳定、有没有明显回归 | 全图 tolerance 8：**99.73% within、0.27% different**，差异 bbox **(88,80,769,2056)**；Search crop `(250,2000,650,300)`：**99.88% within** |
| `diffdump --tolerance 8`（**P0 同帧**） | 同一发的 panel 与 picture | P0 的 C4 判据：`within 8` **≥95%** | 本轮未跑；缺 numpy/环境未恢复时只能记 `n/a` |

三点必须写清：

1. 上面的 99.73% / 0.27% 是 **s19n-repro vs s19o-v2 的截图回归**，说明两张界面截图很接近；**它不是 P0 的同帧 diff**，不能拿来充当 C4。
2. `imgdiff` 的差异 bbox 是截图坐标（按工具的 `(x0,y0,x1,y1)` 输出）；0.27% 的差异集中在那个框里，读图时先看框内。
3. P0 的 C4 必须在**同一发**里同时有 `panel` 与 `picture`，由 `diffdump.py` 算 `within 8`；没有 numpy 就没有 C4，没有 C4 就没有 P0 通过。

---

## 二、故障症状与归属（2026-11-07 15:5x 实测）

### 2.1 故障容器长什么样

| 症状 | 实测 | 含义 |
|---|---|---|
| `/host` 不存在 | `ls: cannot access '/host'` | 三个挂载全丢，仓库、镜像、IPSW 都看不到 |
| GPU 节点为空 | `ls /dev/nvidia* /dev/dri` 无输出 | 设备没有透传，EGL 无法初始化 |
| EGL 库为空 | `ldconfig -p \| grep libEGL` 无输出 | 镜像里没有 GLVND 运行库 |
| 编译失败 | `make host_glwire` → `EGL/egl.h: No such file or directory` | 镜像里没有开发头文件，不能重建宿主 half |
| 容器能力 | `CapEff=0000000000000000` | 容器内不能 `mknod`、不能 `mount`，**无法自救** |

### 2.2 为什么容器内修不了

`CapEff=0` 时：

```sh
mount --bind /host/iosvm /host/iosvm     # Operation not permitted
mknod /dev/nvidia0 c 195 0               # Operation not permitted
```

这两条**不要作为恢复命令**。它们只在编排侧（创建容器时）有意义。容器的 `CapBnd` 里即使有某些位，`CapEff=0` 也意味着当前进程没有有效能力。

### 2.3 症状 → 原因 → 恢复动作（一一对应）

| 症状 | 先查 | 原因 | 恢复动作（谁做） |
|---|---|---|---|
| `/host/iosvm` 不见 | `ls -ld /host/iosvm` | 编排漏了 bind | 编排侧重挂 `/home/lshan/DSH-desktop/iosvm` → `/host/iosvm`，rw |
| `/host/iosvm-work` 不见 / 只读 | `findmnt -T /host/iosvm-work -o OPTIONS` | 漏挂或挂成 ro | 重挂为 rw（`ram.img`/`payload.bin` 要写） |
| `/host/ipsw` 不见 | `ls -ld /host/ipsw` | 漏挂 | 重挂为 ro；仓库不补固件 |
| `root.apfs`/`efi64.fd`/`vars.fd` 缺 | `ls -l $KERNEL/...` | 挂载不完整或文件名变了 | 编排侧修挂载；不要从仓库复制（不在仓库里） |
| `/dev/nvidia*` 空 | `ls -l /dev/nvidia*` | 设备没透传 | 编排侧加 `--device`（至少 nvidiaN/ctl/uvm） |
| `nvidia-smi` 报错/无设备 | `nvidia-smi -L` | 驱动/运行时未挂或权限不对 | 编排侧修 NVIDIA runtime；容器内不 mknod |
| `ldconfig` 无 libEGL | `ldconfig -p \| grep libEGL` | 镜像缺 GLVND | 换镜像，或挂 `/tmp/glvnd/...` 并在 `HOST_ENV` 注入 |
| `make` 报 `EGL/egl.h` | `test -f /usr/include/EGL/egl.h` | 镜像缺 dev 头 | 换带 `libegl-dev`/`libgl-dev` 的镜像；或使用匹配 HEAD 的已编译二进制 |
| `host_glwire` 报 `libEGL.so.1: not found` | `ldd tools/glwire/host_glwire \| grep 'not found'` | 运行库缺失 | 同 GLVND 注入；或换镜像 |
| PIL 缺失 | `python3 -c 'import PIL'` | 镜像缺 Python 图像库 | 编排侧修镜像/装包；不要用假数据绕过截图检查 |
| ImageMagick 缺失 | `magick -version` | 镜像缺工具 | 同上；没有它仍可读 PNG，但裁剪/转换步骤要改口径并记录 |
| numpy 缺失 | `python3 -c 'import numpy'` | 镜像未装 | 装 numpy（§3.10），或明确记录 **C4 不可执行、P0 不能判通过**；不能写成通过 |
| `boot-measure.sh` 用了宿主默认路径 | `grep -n '^IOSVM=\|^GLWIRE='` | 没传 `IOSVM`/`GLWIRE` | 按 §3.10 用 env 覆盖到 `/host` 两棵树 |

---

## 三、恢复检查清单（命令 + 期望）

> 以下命令都在容器内执行；除 §3.9 的 5 秒 `host_glwire` 冒烟外，全部只读。建议按顺序跑，任一项 FAIL 就停在原地，不要继续启动 qemu。

### 3.1 先确认没有残留进程

```sh
ps -eo pid,comm | grep -E 'qemu-system|host_glwire' || echo clean
```

**期望**：输出 `clean`。若有 PID，先 `kill -9`，等 1 秒再继续；不要带着残留 qemu 重挂或重跑。

### 3.2 三个 `/host` 路径存在且权限正确

```sh
ls -ld /host/iosvm /host/iosvm-work /host/ipsw
findmnt -T /host/iosvm      -o TARGET,SOURCE,FSTYPE,OPTIONS
findmnt -T /host/iosvm-work -o TARGET,SOURCE,FSTYPE,OPTIONS
findmnt -T /host/ipsw       -o TARGET,SOURCE,FSTYPE,OPTIONS
```

**期望**：

```text
/host/iosvm       ... rw,...
/host/iosvm-work  ... rw,...
/host/ipsw        ... ro,...
```

`/host/iosvm` 与 `/host/iosvm-work` 必须是 `rw`；`/host/ipsw` 是 `ro` 才算正常。若 `findmnt` 直接报 “not a mountpoint”，说明挂载丢了。

### 3.3 仓库与内核树可读、可写

```sh
cd /host/iosvm && git rev-parse --short HEAD
test -w /host/iosvm && echo "repo-rw-ok"
test -w /host/iosvm-work && echo "work-rw-ok"
test -r /host/ipsw && echo "ipsw-ro-ok"
```

**期望**：

```text
f2035c6          # 或包含它的后续提交
repo-rw-ok
work-rw-ok
ipsw-ro-ok
```

如果 `repo-rw-ok` 不出现，提交/构建都会失败；先修挂载，不要继续。

### 3.4 四个关键文件存在且非空

```sh
KERNEL=/host/iosvm-work/hakios-xnu-kernel
for f in \
  "$KERNEL/qemu/efi64.fd" \
  "$KERNEL/qemu/vars.fd" \
  "$KERNEL/userland/root.apfs" \
  "$KERNEL/userland/system.apfs"; do
  if [ -s "$f" ]; then printf 'PASS %12s  %s\n' "$(stat -c %s "$f")" "$f"; else echo "FAIL missing/empty $f"; fi
done
```

**期望**（本机实测；大小会随镜像重建变化，**非空**才是硬判据）：

```text
PASS     67108864  .../qemu/efi64.fd
PASS     67108864  .../qemu/vars.fd
PASS      5902336  .../userland/root.apfs
PASS   2476748800  .../userland/system.apfs
```

### 3.5 GPU 节点

```sh
ls -l /dev/nvidia* /dev/dri/renderD129 2>&1
```

**期望**（lead 容器）：

```text
crw-rw-rw- 1 root root 195,   0 ... /dev/nvidia0
crw-rw-rw- 1 root root 195, 255 ... /dev/nvidiactl
crw-rw-rw- 1 root root 510,   0 ... /dev/nvidia-uvm
```

外加本机可有的 `nvidia1`、`nvidia-modeset`、`nvidia-uvm-tools`、`renderD129`。
**判定**：`nvidiactl` + `nvidia-uvm` + 至少一个 `nvidiaN` 存在；编号不同不算失败，继续看 §3.6。

### 3.6 `nvidia-smi` 能看到卡且空闲

```sh
nvidia-smi -L
nvidia-smi --query-gpu=index,name,driver_version,memory.used --format=csv,noheader
```

**期望**：

```text
GPU 0: Tesla V100-SXM2-16GB (UUID: GPU-...)
0, Tesla V100-SXM2-16GB, 580.178.04, 0 MiB
```

`memory.used` 不是 0 时，先查是谁占着（另一个实验/推理任务），**不要**在 GPU 忙时启动 A1/v3；等它回到 0 MiB。

### 3.7 EGL/GL 运行库

```sh
ldconfig -p | grep -E 'libEGL\.so|libGL\.so'
ls -l /usr/share/glvnd/egl_vendor.d/10_nvidia.json 2>&1
# 仅当使用 GLVND 注入时，确认挂进来的那份真的在：
ls -l /tmp/glvnd/usr/lib/x86_64-linux-gnu/libEGL.so.1 2>&1
```

**期望**：

```text
libEGL.so.1 => /lib/x86_64-linux-gnu/libEGL.so.1
libGL.so.1  => /lib/x86_64-linux-gnu/libGL.so.1
libEGL_nvidia.so.0 => /lib/x86_64-linux-gnu/libEGL_nvidia.so.0
... /usr/share/glvnd/egl_vendor.d/10_nvidia.json
```

只有 `libEGL_nvidia.so.0`、没有 `libEGL.so.1` 时，`host_glwire` 仍会 `not found`——这正是写作容器当前的状态。

### 3.8 编译前置（要 `make` 才查）

```sh
test -f /usr/include/EGL/egl.h && test -f /usr/include/EGL/eglext.h && echo headers-ok
ls -l /usr/lib/x86_64-linux-gnu/libEGL.so /usr/lib/x86_64-linux-gnu/libGL.so 2>&1
make -C /host/iosvm/tools/glwire host_glwire
```

**期望**：

```text
headers-ok
... libEGL.so -> libEGL.so.1
... libGL.so  -> libGL.so.1
cc ... -o host_glwire ...
```

若报 `EGL/egl.h: No such file or directory`，**不要**在容器里找头文件硬塞；换带 `libegl-dev`/`libgl-dev` 的镜像，或使用仓库里已编译且与 HEAD 匹配的二进制（见 §3.10 的 marker 检查）。

### 3.9 宿主 half 的 5 秒冒烟（不连 qemu）

```sh
truncate -s 16M /tmp/glwire-smoke.img
( cd /host/iosvm/tools/glwire && timeout 5 ./host_glwire 19110 /tmp/glwire-smoke.img ); echo "exit=$?"
```

**期望**：

```text
host_glwire: /tmp/glwire-smoke.img mapped, 16777216 bytes, physical base 0x40000000
host_glwire: build marker uniform-ring-v2
host_glwire: listening on 19110
exit=124
```

`exit=124` 是 `timeout` 杀掉的正常结果，不是失败。若在 `listening` 之前退出，看错误：
- `error while loading shared libraries: libEGL.so.1` → §3.7 的库没恢复；
- `host_glwire: no GL configuration` → GPU/EGL 初始化失败，回到 §3.5–§3.7；
- `cannot listen on 19110` → 端口被占，换一个端口（如 `19111`）再试。

### 3.10 工具与 harness 覆盖

```sh
python3 --version
python3 -c 'import PIL; print("PIL", PIL.__version__)'
magick -version | head -1
python3 -c 'import numpy; print("numpy", numpy.__version__)' 2>&1 || true

cd /host/iosvm
git rev-parse --short HEAD
grep -n '^IOSVM=\|^GLWIRE=' tools/glwire/harness/boot-measure.sh
strings tools/glwire/host_glwire | grep -c GLWIRE_TEXMAT_NAN_TO_IDENTITY
```

**期望**：

```text
Python 3.x              # 本机 3.13.5
PIL 11.1.0
ImageMagick 7.1.1-...  # 便利工具；缺失可跳过
numpy <版本>            # C4 硬依赖；缺失时必须记 n/a
f2035c6                 # 或其后
31:IOSVM=${IOSVM:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}
32:GLWIRE=${GLWIRE:-/home/lshan/DSH-desktop/iosvm/tools/glwire}
1
```

**numpy 是 P0 C4 的硬依赖。** 缺失时启动仍可继续，但 `diffdump.py` 会 `ModuleNotFoundError`；此时 C4 只能记 `n/a`，**P0 不能判通过**。恢复候选（容器内临时装，不进仓库）：

```sh
# 若镜像源可用（有 sudo 用 sudo；容器 root 可去掉 sudo）
sudo apt-get update && sudo apt-get install -y python3-numpy
# 或者镜像已有 pip 且允许联网
python3 -m pip install --user numpy
# 装完必须复验
python3 -c 'import numpy; print("numpy", numpy.__version__)'
```

若 apt/pip 源不可用，**不要硬试**；在证据表里写「numpy missing → C4 n/a → P0 不能判通过」，不要用截图回归或别的发的 diff 顶替。

`strings` 结果为 0 表示跑的是旧二进制（没有本阶段开关），先 `make` 或换二进制。

---

## 四、恢复顺序

### 4.1 编排侧：先把容器重新造对

容器内没有能力修，顺序必须是「先编排、后检查」：

1. **停掉所有 qemu/host_glwire**（宿主或编排侧执行；不要在重挂时还让进程映射 `ram.img`）。
2. 重新创建/重挂容器，至少满足 §1.1 的三类资源：
   - bind：`/host/iosvm`（rw）、`/host/iosvm-work`（rw）、`/host/ipsw`（ro）；
   - device：`nvidiaN`、`nvidiactl`、`nvidia-uvm`（本机另有 `nvidia1`/`modeset`/`uvm-tools`/`renderD129`）；
   - 镜像：带 GLVND 运行库；若要 `make`，还要 `libegl-dev`/`libgl-dev` 与 `cc`/`make`。
3. 不需要给容器 `CAP_SYS_ADMIN`/`CAP_MKNOD`：设备由编排侧透传，容器只使用。

### 4.2 容器内：按清单检查，不要跳项

1. §3.1 无残留 qemu；
2. §3.2–§3.4 三个挂载与四个文件；
3. §3.5–§3.7 GPU 与 EGL；
4. §3.8–§3.9 编译（需要时）与冒烟；
5. §3.10 工具与 `IOSVM`/`GLWIRE` 覆盖。

任何一项 FAIL：回到 §4.1，把对应资源补上；**不要**在容器里 `mount`/`mknod`，也不要先跑 qemu 再回头修。

### 4.3 复跑 A1/v3 之前的最后确认

```sh
ps -eo pid,comm | grep -E 'qemu-system|host_glwire' || echo no-vm
nvidia-smi --query-gpu=memory.used --format=csv,noheader
```

**期望**：`no-vm` 且 `0 MiB`。然后再按 `docs/gpu-acceleration-stage19-p0-runbook.md` §2.3 启动：

```sh
cd /host/iosvm
export REPO=/host/iosvm
export KERNEL=/host/iosvm-work/hakios-xnu-kernel
export GLWIRE=$REPO/tools/glwire
export ROOT=$KERNEL/userland/root.apfs

ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 \
HOST_ENV="GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19p0-1/label GLWIRE_DRAW_LOG=1" \
IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
tools/glwire/harness/boot-measure.sh s19p0-1 "$ROOT" 1500
```

如果镜像不自带 GLVND，在 `HOST_ENV` 最前面加 `LD_LIBRARY_PATH=/tmp/glvnd/usr/lib/x86_64-linux-gnu`（§1.4）。
若 `run.sh` 报 `no UEFI firmware found`，把固件指到挂载里的文件（这两个文件不在仓库里）：

```sh
export QEMU_EFI="$KERNEL/qemu/efi64.fd"
export QEMU_VARS="$KERNEL/qemu/vars.fd"
```

---

## 五、不要误判 / 不要做

1. **不要把 `CapEff=0` 当成唯一故障**：正常容器也可能是 0；它说明的是「容器不能自救」，不是「挂载一定丢了」。
2. **不要用 `/host` 是否存在来判断整个环境**：写作容器 `/host` 在、GPU 在，但 `libEGL.so.1`、PIL、ImageMagick、numpy 都缺。
3. **不要在容器里 `mount --bind` 或 `mknod`**：没有 `CapEff`，只会失败；恢复动作在编排侧。
4. **不要把「有截图」当成「环境对了」**：截图 245 KB 也可能只是旧 `ram.img` 的残留；`ZERO_RAM=1` 与无残留 qemu 是前提。
5. **不要在没有 numpy 时声称 C4 通过**：numpy 是 P0 C4 的硬依赖，`diffdump.py` 会直接 ImportError；证据表必须写 `n/a`，P0 不能判通过。
6. **不要把本手册当成 P0 结论**：环境恢复只回答「能不能开始实验」，P0 的判据在 runbook §三。
7. **不要提交** `/host/iosvm/logs/`、`/host/ipsw`、`root.apfs`、`system.apfs`、`ram.img` 或任何截图；仓库只允许 `docs/demo/*.png|gif` 证据图。

---

## 附录 A：本次实测快照（2026-11-07 15:5x）

| 项 | lead 容器（有部分工具） | 15:5x exp/code 新容器（全裸） | 本手册写作容器 |
|---|---|---|---|
| `/host` 三挂载 | 有，rw/rw/ro | **无** | 有，rw/rw/ro |
| `/dev/nvidia*` | nvidia0/ctl/uvm + 额外 | **空** | nvidia1/ctl/uvm/modeset/uvm-tools + renderD129 |
| `nvidia-smi` | V100，0 MiB | 不可用 | V100，0 MiB |
| `libEGL.so.1` | 有 | **无** | **无**（只有 `libEGL_nvidia.so.0`） |
| EGL/GL 头文件 | 有 | 无 | **无** |
| PIL | **有 11.1.0**（临时装 `python3-pil`，未进仓库） | 无 | **无** |
| ImageMagick | **无** | 无 | **无** |
| numpy | **无** | 无 | **无** |
| `CapEff` | 0（容器常态） | **0** | **0**（本手册实测） |
| 能否做实验 | 能启动；**C4 因缺 numpy 不可判** | 不能 | 不能（缺库/工具） |

**结论**：15:5x 的 exp/code 新容器是「挂载 + 设备 + 镜像库 + 工具」全缺；lead 容器是「有挂载/GPU/PIL，但缺 ImageMagick/numpy」；写作容器是「有挂载/GPU，缺 EGL 库与 PIL/ImageMagick/numpy」。三者都必须走 §3 全清单，不能只看一项；尤其不要把 lead 容器能启动误当成 C4 能判。

## 附录 B：相关文件

| 文件 | 用途 |
|---|---|
| `docs/gpu-acceleration-stage19-p0-runbook.md` | P0 配方、checklist、证据表；环境好了之后按它跑 |
| `tools/glwire/harness/boot-measure.sh` | 启动/取样；第 31/32 行支持 `IOSVM`/`GLWIRE` 覆盖（`f2035c6`） |
| `tools/glwire/host_glwire` | 宿主 half；§3.9 冒烟与 build marker |
| `logs/s19n-repro/s19n-repro-meta.txt` | A1 的容器特例实证：`LD_LIBRARY_PATH=/tmp/glvnd/...`、`QEMU_EFI`/`QEMU_VARS` |
| `/host/iosvm-work/hakios-xnu-kernel/userland/{root,system}.apfs` | 客人机镜像；不在 git |
| `/host/ipsw/*.ipsw` | Apple 固件归档；不在 git |
