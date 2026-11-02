# IOSVM GPU 加速 —— 工程交接文档

> 交接时间：2026-11-02
> 接手目标：把界面渲染从「客户机软件光栅化」换成「宿主 V100 渲染」，目标 24 fps
> 当前状态：**链路已打通，但尚未提速**（界面仍是 1.4 fps）

---

## 一、一句话现状

绘制命令已经从虚拟机送到宿主、V100 画完再送回虚拟机，**整条链跑通并逐段实测过**；
但每帧要传约 10 MB 像素数据，实际只有 0.22 fps，**比本地渲染（1.4 fps）还慢**。

**瓶颈是管道带宽，不是 GPU。** 宿主单帧只要 6–14 ms，24 fps 的预算是 41.7 ms。

---

## 二、关键数字（全部实测，可直接引用）

| 项目 | 数值 | 测量方式 |
|---|---|---|
| 界面当前帧率 | **1.4 fps** | 滚动设置列表 20 s，驱动日志 `frames in N ms` |
| 宿主 V100 画一整帧（含回读） | **1.9 ms** | `tools/host-glquad/glquad.c` |
| 宿主原地渲染（内存直读直写） | **6–14 ms**（稳态） | `tools/host-glquad/gpu_host_ram.c` |
| 端到端每帧开销 | **4.4–4.5 s** | 宿主日志 `interval` 字段 |
| 每帧记录体积 | **约 10 MB** | 宿主日志 `records B` 字段 |
| 画面体积（750×1624 BGRA） | 4,872,000 B | — |
| 24 fps 预算 | 41.7 ms | — |

宿主的余量：41.7 ÷ 10 ≈ **4 倍**。

---

## 三、架构与数据流

```
CoreAnimation (客户机 SpringBoard/backboardd)
      │  GLES 调用（62 个函数）
      ▼
EAGL 分发表（eagl.c 里 void *table[SLOTS]）
      │  这些槽被换成 remote_* 记录器
      ▼
记录器 remote_inc（生成代码）→ 二进制记录流
      │  u32 opcode + u32 length + payload
      ▼
eagl_remote.c：攒一帧 → TCP 发到 10.0.2.2:9110（QEMU user-net）
      ▼
宿主 host_glwire（Linux, 映射 qemu/ram.img）
      │  重放每个 GL 调用到真实 GL 上下文（Tesla V100）
      │  glReadPixels → 直接写进客户机内存（按魔数定位）
      ▼
客户机 eagl.c：把画面 memcpy 进 IOSurface → 驱动呈现
```

**双通道设计**：命令走 TCP，画面数据走共享内存（`qemu/ram.img`，QEMU 以 `share=on` 映射）。

---

## 四、已经完成并验证的部分

### 4.1 宿主侧（成熟，可直接复用）

| 文件 | 作用 | 验证 |
|---|---|---|
| `tools/host-glquad/glquad.c` | EGL 无窗口上下文 + 整帧渲染基准 | V100 上 1.9 ms/帧 |
| `tools/host-glquad/gpu_host.c` | TCP 渲染服务（收图层、画、回传） | 25–31 ms/帧（含全量拷贝），像素正确 |
| `tools/host-glquad/gpu_host_ram.c` | **原地渲染**：按魔数在 ram.img 里找图层，画完原地写回 | **6–14 ms/帧，线路上零字节** |
| `tools/host-glquad/rammap.c` | 验证宿主能读客户机内存 | 12 GB 中定位到客户机内核 |
| `tools/glwire/host_glwire.c` | GL 命令重放器（57 个 opcode） | 收 `glClearColor/glClear/glViewport` 正常执行 |

### 4.2 客户机侧（可用，但有一处必须注意）

| 文件 | 作用 | 状态 |
|---|---|---|
| `userland/src/eagl_direct.h` | **内部 GL 直连 OSMesa 的 49 个函数指针** | ✅ 必须保留，见「五、坑」 |
| `userland/src/eagl_remote.c` | 记录缓冲、TCP 传输、取回画面、配置开关 | ✅ 工作 |
| `userland/src/remote.inc` | 记录器（57 个 trampoline） | ✅ 由 `spec.def` 生成 |
| `userland/src/remote_table.inc` | 把 EAGL 表槽指向记录器（50 个槽） | ✅ |
| `userland/src/eagl.c` | 接入点：`eagl_remote_install()` + `frame_end()` 里 flush | ✅ |

### 4.3 链路验证证据（真实日志）

客户机：
```
hakios-remote: installing the recorders (off)
hakios-remote: after install, on=1
hakios-remote: end of frame, on=1 images=1 first=750x1624
hakios-remote: flush -> 1
```

宿主：
```
host_glwire: frame 2, 750x1624, 9959384 records B, 4872000 picture B, 0.03 ms, interval 4.49 s
```

---

## 五、走过的坑（**不要重走**）

### 5.1 内部 GL 必须绕过 EAGL 表（最关键）
`eagl.c` 自己也调 GL（把 IOSurface 绑成纹理）。若这些调用也走分发表，簿记会被送去宿主，
本地永远没有纹理 → **一帧都画不出来**，日志表现为 `attachImage: ... not done`。
解法：`eagl_direct.h` 用 `OSMesaGetProcAddress` 拿 49 个函数的直达指针并宏替换，构造期加载。

### 5.2 `share=off` 是死路
QEMU 默认 `memory-backend-file,...,share=off`（私有映射），宿主读 `ram.img` **看不到任何实时数据**。
已改为默认 `share=on`（`qemu/run-darwin.sh` 的 `QEMU_RAM_SHARE`）。

### 5.3 去重无效（已试过，别再试）
按纹理名分桶的像素哈希去重已实现、编译、实测：记录仍是 9.96 MB、4.49 s。
CoreAnimation 每帧整屏重合成（一个走动的时钟就够），字节确实在变。

### 5.4 其它已排除的路线（有数据）
- **客户机内启用 GL「加速渲染器」**：Mesa softpipe 在 TCG 里 7 分钟出不了第一帧，比软件渲染更慢
- **llvmpipe**：需要 arm64-iOS 的 libLLVM，本机只有 x86_64，且对界面无效（Mesa 不在 CA 合成链上）
- **`-cpu max` + `tb-size=4G`**：7 分钟无画面（SVE 在 TCG 下极慢）
- **只降分辨率**：`fbscale=3→1`（9 倍像素）只换来 2 倍帧率（1.3→2–4 fps），说明瓶颈是模拟执行而非像素

### 5.5 工具参数顺序
`host_glwire <端口> <ram.img>`（端口在前，别写反，否则绑到随机端口且静默）

---

## 六、当前瓶颈的精确定位

每帧记录 ≈ 10 MB，来源：CA 每帧上传两张 4.8 MB 纹理（`glTexImage2D` / `glTexSubImage2D`），
像素被**内联在记录流里**，经 TCP 逐字节序列化穿过 TCG（这是 4.4 s 的来源）。

宿主拿到记录后只需 **0.03 ms** 分发、6–14 ms 绘制 —— 时间全花在传输上。

---

## 七、下一步实施方案（推荐顺序）

### 方案 A：竞技场偏移（**不需要内核改动，先做这个**）

**思路**：客户机把纹理拷进一块「宿主能找到的固定区域」（已有魔数竞技场，扩到 16 MB），
记录里只带**竞技场内的偏移 + 尺寸**，宿主直接读。

**为什么可行**：现在慢是因为 10 MB 要经 TCG 逐字节序列化进 socket。改成 memcpy 到内存，
TCG 下拷贝约 200–500 MB/s，9.6 MB ≈ 20–50 ms，加宿主 6–14 ms → **预计 30–50 ms/帧（20–30 fps）**。

**改动点**：
1. `userland/src/eagl_remote.c`：`REMOTE_ARENA_BYTES` 从 4 MB 扩到 16 MB
2. `userland/src/remote.inc`：`remote_glTexImage2D` / `remote_glTexSubImage2D` 不再 `gw_*` 内联像素，
   改为 memcpy 进竞技场 + 记录 `(offset, size, 格式参数)`
3. `tools/glwire/spec.def`：为这两个调用新增一个「按偏移上传」的 opcode（或改造现有编码）
4. `tools/glwire/host_glwire.c`：处理该 opcode 时，用 `ram + arena_offset` 直接当 `glTexImage2D` 的像素源

**验收**：宿主日志 `records B` 从 ~10 MB 掉到 < 100 KB，`interval` 从 4.49 s 掉到 < 60 ms。

### 方案 B：真正零拷贝（A 成功后再做）

给客户机加一个「用户虚拟地址 → 物理偏移」的查询接口（内核侧小改动），记录只带地址，
宿主直接从 `ram.img` 读 —— 连 memcpy 都省掉。

### 顺序建议
先 A（当天可验证），再 B（锦上添花）。A 若达到 20–30 fps，界面就已经可用了。

---

## 八、环境与操作手册

### 8.1 目录
- 虚拟机工程（内核 + userland）：`/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`
- 对外仓库（文档 + 工具）：`/home/lshan/DSH-desktop/iosvm`（github.com/lshan99q/iosvm）

### 8.2 交叉编译工具链
```bash
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=<repo>/xnu-deps/host/bin:/tmp/cctools/bin:$PATH
```

### 8.3 构建镜像（只重建 4 MB 的小镜像，约 1–2 分钟）
```bash
cd userland
GL=1 RC=rc.netonly CONFIGD=1 \
  EXTRA="/tmp/guestbin/fill_panel /tmp/guestbin/glremote_on /tmp/nc/netcfg" \
  ./build.sh root.apfs
```

### 8.4 启动（**必须以 `metal=1` 让 CA 走 GL 路径**）
```bash
cd <repo>
setsid nohup env TRIES=4 MAX_WAIT=3600 NO_FRAMES_BY=300 KEEP=1 QEMU_NET=1 \
  CMDLINE_EXTRA="fbscale=2 metal=1" \
  bash qemu/wait-for-ui.sh /tmp/gpu.png >/tmp/gpuboot.log 2>&1 &
```

### 8.5 启动宿主重放器（**必须在 QEMU 起来之后**，因为要映射 ram.img）
```bash
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
./host_glwire 9110 /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel/qemu/ram.img
```

### 8.6 看日志
```bash
log=$(ls -t /tmp/tmp.* | head -1)          # 客户机串口日志
grep -a "hakios-remote:" $log              # 我们的打点
grep -a "frames in " $log                  # 驱动帧率
grep -a "interval" /tmp/glwire*.log        # 宿主侧帧间隔
```

### 8.7 截图
```bash
bash qemu/screendump.sh /tmp/shot.png
```

### 8.8 重要注意事项
- **启动约 6–9 分钟**，约 40% 概率卡死（无帧），杀掉 qemu 重试即可
- 杀进程只按进程名：`ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'`
  **绝对不要** `pkill -f` 带脚本名（会杀掉自己的 shell）
- `payload.bin` 与 `ram.img` 是共享的：**同一时间只能有一台虚拟机在跑**

---

## 九、配置开关速查

| 开关 | 作用 |
|---|---|
| `metal=1` | 让 CoreAnimation 使用 GL 路径（**必需**，否则 CA 走软件渲染器，记录器收不到东西） |
| `fbscale=2` | 渲染分辨率 750×1624（3 是默认 1125×2436，1 是 375×812） |
| `/redwood/bin/glremote_on`（文件存在即开） | 启用远程渲染 |
| `QEMU_RAM_SHARE=on` | 默认已开，宿主才能读客户机内存 |
| `GL=1`（构建时） | 把 `libhakios.gl.dylib` + `libOSMesa` 放进镜像 |

---

## 十、验收标准

1. 客户机日志出现 `hakios-remote: flush -> 1`（每帧）
2. 宿主日志 `records B` < 100 KB（方案 A 完成后）
3. 宿主日志 `interval` < 60 ms
4. 驱动日志 `frames in 5000 ms (N a second)`，**N ≥ 20**
5. 截图与本地渲染一致（无黑屏、无花屏）

---

## 十一、相关文档索引

| 文档 | 内容 |
|---|---|
| `iosvm/tools/glwire/README.md` | GL 线协议、生成器、宿主重放、瓶颈分析 |
| `iosvm/tools/host-glquad/README.md` | 宿主渲染各阶段的实测数据 |
| `iosvm/docs/research/linux-kernel-build.md` 附录 33–35a | 渲染瓶颈的完整调查（含被否掉的路线） |
| `iosvm/docs/research/linux-kernel-build.md` 附录 36–36a | 外网修复、双代理共用一棵树的 ROOT 规则 |
| `/home/lshan/dsh.MD/IOSVM.md` | 项目总体结论 |

---

## 十二、诚实的状态声明

- **已完成**：宿主渲染能力（6–14 ms/帧）、端到端链路、内部 GL 直连修复、全部实测数据
- **未完成**：界面帧率**没有提升**，仍是 1.4 fps；远程路径当前 0.22 fps（比本地慢）
- **原因**：10 MB/帧 的传输，方案 A 可解，但**尚未实施**
- **不能声称**：GPU 加速已经完成。它只完成了「能跑通」，没完成「更快」
