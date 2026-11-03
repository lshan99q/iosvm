<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第九阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第九阶段工程文档

> 建立时间：2026-11-03 22:15（第八阶段 P0' 收尾、P2'/P4'/P1' 排队中写下的）
> 前置阅读：`IOSVM-GPU加速第八阶段工程.md`（目标）、`IOSVM-GPU加速第八阶段.md`（已量到的数字）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状（来自第八阶段已完成的测量）

1. **三族 deadline，一次停顿全打穿**：ticket lock 已经从 0.96 s 抬到 30 s（`tlto_us=30000000`），
   但同一次 >6 s 的停顿还打穿了另外两族——`Spinlock` 的 251,658,240 ticks（≈4 s，
   `lock_panic_timeout`）和 `waitq` 的 240,000,000 ticks（≈3.84 s）。第八阶段 P0' 因此是
   **6 PASS / 8 次、2 次 panic**（界面 420–600 s），「0 panic」没有达成。
2. **停顿至少 6 s、最长量到 10.3 s**，且 panic 栈落在
   `sched_timeshare_maintenance_continue → thread_update_process_threads → hw_lock_lock_contended`
   （GEN3 zone 的堆锁）——**不是某个驱动，是整机**。
3. **画面**：`remote_only` 下客人机显示面 0 像素（A 路判死）；glyph 由 62 个记录槽位之外的入口画，
   第八阶段的 `glslots=1` 正在一次启动里点名（`glDrawRangeElements`/`glDrawArraysInstanced`/
   `glDrawElementsInstanced` 等 12 个候选）。

所以这一阶段只做三件事：**把三族 deadline 一起抬到「停顿够不着」的高度**、**把停顿的来源钉死**、
**把 B 路（glyph 的 GL 入口）从「定位」推进到「接进流里」**。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 三族 deadline：**内核一次改三处**（`lock_panic_timeout`、waitq、ticket）还是只改宿主/参数？ | 只改一半：panic 换个锁继续出现（第八阶段已经演过一遍：修了 ticket，spinlock/waitq 接着炸） | P0'' 的 0 panic，以及所有长测量（P3' 的 20 s 窗口、P1' 的对照） |
| D2 | 停顿的靶子：先做**宿主侧对照**（`ZERO_RAM`、页缓存、I/O 空闲窗口）还是直接进客户机 `ustacks` 采样？ | 直接进客户机：可能查的是宿主 I/O 的影子（第八阶段 `/proc/pressure/io` 20% 的嫌疑） | P1'' 的归因 |
| D3 | 画面：**B 路找到就接进 `spec.def`**，还是先收第八阶段没跑完的 P2'/P3' 数字？ | 先收数字：画面停在 C，W2 仍然不成立 | W2 的结论、P1 验收 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0'' | 三族 deadline 抬够 | 同一套参数（`cpus=4`、`/tmp/root-fixed-net.apfs`）**10 次冷启动 ≥8 次到界面、0 panic**；三个 panic 形态（ticket / `Spinlock[` / `waitq(`）各一次都不再出现；deadline 数值写进 `harness/README.md` | 所有长测量 |
| P1'' | 停顿归因 | 三组对照（`ZERO_RAM=1` vs `0`、宿主 I/O 空闲窗口、`drop_caches` 后）× 各 ≥3 次启动，给出「停顿跟着谁走」的表；若都不成立，给 `ustacks` 采样到的停顿时刻栈 | 9 fps 以上的说法 |
| P2'' | B 路接进流里 | 找到画 glyph 的入口（第八阶段 `glslots=1` 的结果定名字），按 `spec.def` 加进记录器并重放；`remote_only` 截图 **> 400 KB 且有文字/Dock**，记录仍 < 100 KB，≥5 fps，0 panic | W2 的定义 |
| P3'' | 补第八阶段的两张表 | P2' 相位（容差 8 / 95%）与 P3' 四配置各一个数字；缺的写「未测」+ 卡在哪一行 | 结论 |
| P4'' | 文档与提交 | `/home/lshan/dsh.MD/` 与 `iosvm/docs/` 同步；`make test` 通过；每个决定一个提交；**推送到 GitHub**（`lshan99q/iosvm`） | — |

---

## 三、需求详述

### P0'' 三族 deadline 抬够（半天，内核一次改三处 + 10 次启动）

现状（第八阶段的实测）：

| 族 | 位置 | 现在的值 | 等效时间（62.5 MHz） | 被什么打穿 |
|---|---|---|---|---|
| ticket lock | `tlto_us=<µs>`（boot arg） | 30000000 µs | 30 s | 还没被打穿（第八阶段 2 次 panic 都不是它） |
| `Spinlock`（mutex 自旋） | `osfmk/arm/locks_arm.c` 的 `MACHINE_TIMEOUT(lock_panic_timeout, "lock-panic", …)`，端口上实际 0xf000000 | 251,658,240 ticks | **4.03 s** | 第 3 次启动（>10.3 s 停顿） |
| `waitq` 锁 | `osfmk/kern/waitq.c` 的超时（panic 在 `waitq.c:694`） | 240,000,000 ticks | **3.84 s** | 第 7 次启动（6.05 s 停顿） |

做法：

1. `lock_panic_timeout`：**不要**改 `MACHINE_TIMEOUT` 的默认（那是设备树的语义），改成
   `ml_init_lock_timeout()` 里像 `tlto_us` 一样读一个 boot arg（例：`lpto_us=`），并把默认抬到 60 s；
2. waitq 的超时同理（找到 `waitq.c:694` 那条路的常量，抬到 60 s 或 boot arg）；
3. ticket 已经有 `tlto_us`，这一轮把它也写进选定参数（60 s）；
4. 重编内核（`xnu-build.sh`；**注意 `IOSVM_USE_LLD=0`，否则 `-image_base` 不生效**），
   用同一张镜像跑 10 次，记录：界面秒数、panic 形态、停顿数（`stalls.py`）。

**判据**：10 次 ≥8 到界面、0 panic；三个形态各 0 次。若还有 panic，给出**第四族**的名字与栈。
**回退**：三个 boot arg 都默认「抬」，但可以不传（不传 = 旧行为）——这是 A/B 的方法。

### P1'' 停顿归因（1 天，三组对照）

工具已经在第八阶段写好：`harness/stall-boot.sh`（一次启动 + 宿主 I/O 采样 + `stalls.py`）。

三组对照（每组 ≥3 次，一次只改一个变量）：

1. **`ZERO_RAM=1` vs `0`**：重建 `ram.img`（2.4 GB 写入）vs 复用上一轮的；
2. **宿主 I/O 空闲窗口**：`sync; echo 3 > /proc/sys/vm/drop_caches` 后 5 分钟再启动 vs 平时；
3. **客人机侧**：若前两组都不同向，用 `ustacks=<proc> ustacks_at=<秒>`（**不要** `touch.py stacks`）
   在停顿窗口采样。

**判据**：一张表（组别 × 停顿数 / 最长停顿 / 停顿窗内 `io_some` vs 窗外），并给一句结论。

### P2'' B 路接进流里（1–2 天）

1. 第八阶段的 `glslots=1` 一次启动给出入口名（12 个候选之一，或「一个都没有」）；
2. 有名字 → 在 `spec.def` 里按现有形状加一行（`!local` + 指针参数按现有类型），`gen.py` 生成记录器，
   宿主 `host_glwire` 重放；判据：`remote_only` 截图 > 400 KB、有文字/Dock、记录 < 100 KB、0 panic；
3. 没名字 → 结论写「不在 EAGL 表里」，下一阶段转向 CA 的 Metal/软件路径（`CA::Render`）。

---

## 四、不做（已被数据否掉，别重试）

- 不再单独抬一族 deadline（第八阶段：修 ticket，spinlock/waitq 接着炸）。
- 不再用哈希比 panel/picture（两个渲染器不可能逐像素一致）。
- 不再在界面之前驱动触摸（600 s 的触摸 vs 420–660 s 的界面）。
- 不再只改 `src/eagl.c` 就跑（`touch obj/stage0` 才能重打 `system.apfs`）。
- 不再用一次成功/失败的启动判断配置优劣。
- 像素去重、客户机内 GL 加速、只降分辨率、`touch.py stacks`、`pkill -f`、并行第二台虚拟机。

---

## 五、验证环境与操作规范

（与第八阶段相同，重点重复三条）

1. **同一时间一台虚拟机**；跑启动的人持 `/tmp/iosvm-vm.lock`，构建的人等锁消失；
2. 改端口代码：`touch obj/stage0` → `./build.sh root.apfs`；改内核：`xnu-build.sh`（`IOSVM_USE_LLD=0`）；
3. 结论 = 一张日志行 + 一个数字 + 一次可复现的启动参数；「没做到」写卡在哪一行。

---

## 六、里程碑

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M9.0 | 三族 deadline 一起抬 + 10 次验收 | P0'' |
| M9.1 | 停顿归因 | P1'' |
| M9.2 | B 路定位结论（入口名或「不在表里」） | P2'' 的前半 |
| M9.3 | B 路接进流里（若能） | P2'' 的判据 |
| M9.4 | P2'/P3' 补齐 | P3'' |
| M9.5 | 文档 + `make test` + 推送 | P4'' |

---

## 七、诚实的预期

- **能确定的**：三族 deadline 抬到 60 s 之后，6 s / 10 s 的停顿不再 panic——这是算术，不是猜测；
  P0'' 的 10 次启动两小时就能给结论。
- **不确定的**：停顿本身。它可能真是宿主 I/O，也可能是 TCG 下客户机自己停住；两组对照可能都不指向。
- **不能承诺的**：B 路一定找得到（第八阶段只证到「不在记录流里」）；以及超过 9 fps。
