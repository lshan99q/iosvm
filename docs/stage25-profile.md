# stage 25 剖析：一帧 75 ms 花在哪、哪些能搬给宿主、多核现在用几颗

2026-11-17 20:45。数据全部来自本机实测（`logs/s24-*`），配置：remote-only + 全屏补丁 +
`glwire_ack_only=1 gkbf=1` + `fb_noblit` 自动。

## 一、一帧的时间分布（fbscale=2，墙钟 72–81 ms）

| 段 | ms/帧 | 来源 | 备注 |
|---|---:|---|---|
| display 整屏放大 blit（750×1624 → 1125×2436，274 万像素） | **~0**（24–48 µs） | 驱动自报 `us each in the driver` | 原 62 ms，`fb_noblit` 砍掉 |
| GL 插桩路径合计 | **13.8** | 逐函数 census | 见下表 |
| ├ glFlush ×200 | 13.3（单次 66 µs） | census frame 400 | 唯一热点 |
| ├ glBindTexture ×1230 | 0.29 | | |
| └ 其余（fence/delete/uniform/…） | 0.2 | | |
| 表面/纹理/getImage 拷贝 + 光栅相关 | **约 31–37**（差分推断） | E1：`fbscale=1` 时 CPU/帧 75→44 ms | 见下方注 |
| CA 布局/图层树/backboardd/窗口提交/显示链路 + TCG 开销 | **约 25–30** | 余量（未插桩） | 下一步要测 |
| 宿主侧（不在客机预算内） | 1.4 | host.log/replay | records 4–19 KB/帧 |

注：`fbscale=1`（显示面 375×812，像素 ÷4）把 CPU/帧从 75–81 压到 44 ms，但画面崩（Dock 只剩 2 图标、
文字糊、右侧列缺失，SHM 输出掉到 375×812）→ 判废；这 31–37 ms 的差值就是「像素量/表面拷贝」的代价上限。

## 二、CA 内部的具体分布：现在是盲区，怎么补
- 已插桩的是**客机 GL 调用**（`eagl_enter/leave` 按 function id 计时）。CA 自己的 ObjC/C 代码、
  backboardd 的窗口合成、SpringBoard 的布局逻辑都不经过 GL 插桩，所以上面那 ~25–30 ms 目前只有余量，没有归属。
- 补法（E3，一次改动）：在内核/用户态加**线程级采样器**——每 200 帧（或每帧）记录 SpringBoard、
  backboardd、CA render、eagl 这几个线程的 user+sys CPU 时间增量，打一行 top 线程；再叠加现有的
  `hb=N` heartbeat（programs/runnable）。有了它就能把 ~25–30 ms 分到具体进程/线程。
- 另一条低成本线索：把 `fbscale=2` 固定，只改 `glframes`/`gkbf`/`fb_noblit` 的开合做差分（已有基础设施）。

## 三、哪些部分可以「发指令→宿主算」（按预期收益排序）

| 候选 | 做法 | 预期省 | 风险/条件 |
|---|---|---:|---|
| 1. 表面/纹理零拷贝 | qemu 的 `ram.img` 本来就是 `share=on` 的共享 mmap；宿主可直接读客机 RAM 里的 IOSurface，客机只发「surface id + 地址/stride/格式 + 版本」。省掉客机 4.87 MB 拷贝 | 最高 ~30 ms（E1 上限） | 需要地址/租约协议、生命周期防改；先测 refill 频次 |
| 2. 脏矩形（surface 级） | 只在变化区域刷/送，不再整面 memcpy；records 已经是 4–19 KB/帧，缺的正是表面拷贝这份 | 10–25 ms | 需要可靠 damage 源（CA 有 damage，但要接出来） |
| 3. glFlush 13.3 ms | 先拆 66 µs 花在哪（`clear_now` / `glFlush()` / socket 写）；再把能延后的延后。**注意：把 `frame_end` 合并会饿死帧**（本日实测 27 帧后停摆，已判废） | 5–12 ms | 不能动帧边界语义 |
| 4. 静态内容缓存 | 同一 surface 未变就不 refill；壁纸/图标已有静态补丁兜底 | 3–10 ms | 版本判定要准 |
| 5. 宿主合成/放大 | 已经在宿主做了（`fb_noblit` 后客机完全不碰面板） | — | 已完成 |

## 四、不降分辨率、不影响画面的提速手段（综合排序）
1. **多核（见第五节）**——结构性，最可能一次拿到 1.3–1.8×。
2. **表面零拷贝**（第三节 1）——纯粹搬工作，不碰画面。
3. **脏矩形**（第三节 2）——不碰分辨率。
4. **glFlush 拆解后再砍**（第三节 3）。
5. 减少格式转换（BGRA/RGBA swizzle）与 IOSurface 数量、复用纹理。
6. CPU 亲和性/优先级：把 qemu vCPU 与 host_glwire 分核、`chrt` 提优先级——只影响稳定性与噪声，不改变单帧算法成本。

## 五、CPU 核心：现在用几颗、多核能不能提速
- **现在：客机 1 个 vCPU**（`-smp 1`，`QEMU_SMP` 默认 1）。宿主侧实测 qemu 进程 100–104%（= 1 个核打满），
  `host_glwire` 0.1–0.5 核；整台机器 32 核、隔壁 win2022-gpu 常占 3.4 核。
- **能不能多核**：能。`qemu/run.sh` 用 `QEMU_SMP` 控制 vCPU 数；MTTCG 对 aarch64/virt 可用；
  显示驱动本身有 `fb_threads=3` 的绘制线程，CA/backboardd 也是多线程。
- **怎么测**（正在做）：同一 root 与参数，`QEMU_SMP=2`（下一发 4），只看墙钟 fps 与「全部 vCPU 的 CPU/帧」。
  判据：墙钟中位 ≥ 当前 12–14 fps 的 1.3 倍（≥16 fps）算有效。
- **预期与坑**：帧提交（CA commit → 显示）始终串行，Amdahl 上限在 1.5–2×；MTTCG 的原子/内存序会比单核慢，
  iOS 的锁竞争可能吃掉一部分；核数越多，TCG tb 缓存与内存带宽越容易打满。

## 六、多核实测：判死（2026-11-17 21:16，补强 23C）
- `s24-smp2b`：`QEMU_SMP=2` + `CMDLINE="cpus=2 …"`，guest 打印 `GenericARMPE: 2 of 2 processors are running`（真 SMP），
  起机后仅 5 帧。
- **panic(cpu 1 caller 0xfffffff007aedb18): Ticket spinlock[0xfffffff007b6c880] timeout after 60000047 ticks;
  cticket 0xf4, nticket 0xf6, waiting for 0xf5, current owner on CPU 0 @lock_ticket.c:386**，panicked task pid 29 `logd_helper`。
- 这与 stage21 的 4 核 panic 同一失败模式；23C（SMP=2/4 全 `hostframes=0`）当时的「只判死、没给 cpus=」疑点至此补齐：
  **即便正确传 `cpus=2`，当前 ack-only/remote-only 路径下 2 vCPU 也会 Ticket spinlock 超时 panic**。
- 判死（第三次同方向失败：stage21 panic / 23C hostframes=0 / 本次 panic），不再试 `-smp 4`（更激进只会更糟）。
  11-02 那次 `SMP=4` + `cpus=4` 能跑，属旧 in-place 配置，不能外推。
- 影响：多核这条「结构性 1.3–1.8×」拿不到，15 fps 只能靠把客机工作搬走/砍掉（25.2/25.3/25.4/25.5）。
``
