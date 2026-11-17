# 交接单：IOSVM stage 24（2026-11-17 17:30）

> 读完这一页就能接手，不需要翻历史聊天。所有数字都是实测。

## 一、主线与目标
- 项目：IOSVM —— 在 QEMU/TCG 的 aarch64 客机里跑 Apple iOS 16.5 用户态；客机把 CoreAnimation 的
  GL 调用记录后送到宿主，宿主用真 GL（V100）回放并合成画面。
- **本周目标**：客机帧节拍 **≤91 ms（11 fps）**，且画面完整（Dock 4 图标 + Search 居中 + 下半屏不缺）。
- **状态：已达成**（2026-11-17 17:17）——稳态中位 **13.9 fps / 75 ms**，画面与基线逐字节相同。见第五节。

## 二、当前最好成绩（可直接引用）
- **长跑交付用单核：13.9 fps / 75 ms**（SMP=1，历史无 panic）；**爆发模式**：`SMP=2` 绑核 15.6–18.3 fps，但约每小时一次 Ticket spinlock panic（`_pset0+0x100` 调度锁，见 docs/stage25-multicore.md 第十一节），只能短跑/演示。`tools/glwire/run-pinned.sh` 起栈，
  配置 = remote-only + 全屏补丁 + host `GLWIRE_ACK_ONLY=1` + guest `glwire_ack_only=1 gkbf=1` + auto `fb_noblit`
  + `QEMU_SMP=2 cpus=2` + vCPU 绑 `taskset` 独占核。多核净收益 +60%（同负载 SMP=1 中位 9.9 fps）。
- 单核（旧现役）：13.9 fps，75 ms/帧；成对测同负荷窗口 14.8 vs 12.8 fps（对照 = 不加 `fb_noblit`）。
- **测量发必须 `GLWIRE_PATCH_MIN_COVERAGE=2`（补丁常开）**：门限打开时回放残缺帧会跳过补丁，画面变成黑+绿块（21:53 引入、23:29 踩到，见 docs/stage25-multicore.md 第九节）。
- **多核必须绑核**：不绑核 `-smp 2` 会在起机阶段 `Ticket spinlock timeout` panic（docs/stage25-multicore.md）。
- 关键两刀：`gkbf=1`（CA 记录量 90 KB/帧 → 4–19 KB，宿主 41 → 1.4 ms/帧）+ **`fb_noblit=1`**
  （拿掉客机每帧 1125×2436 = 274 万像素的整屏放大 blit，驱动自报 62 ms → 24 µs）。
- md5：root `7c7da1a5e4dc5cb6044e4e813f001ae3`、kernel `c153053e3b6e374ae9c5f05c6ca1f508`、
  system `49ae3cf4fd53334b475a921eb9915008`、lib `40463969cfb9cb7be14a20ded6042a05`。
- 注意：墙钟 fps 随宿主负载剧烈波动（隔壁 win2022-gpu 常占 3.4 核）。单发绝对值不可比，
  **必须成对测 + 看 CPU/帧**（采样脚本见第七节）。

## 三、已验证的负结果（别重做）
| 尝试 | 结果 |
|---|---|
| 异步回包 / 双缓冲 | **死**：wait = 1 µs，客机根本没等宿主 |
| records 去重 | **死**：records 仅 12.65 KB/帧（图 4.87 MB 的 0.26%），去重 <0.12 ms |
| `GLWIRE_NOREAD` / `upload_fast` / `upload_scale=2` / `nopanel_up` | 全部空转（不作用于 in-place 路径） |
| `GLWIRE_ACK_ONLY` + `glwire_ack_only` | copy 46.8 → 9.2 ms 成功，但 CA 71 → 144.2 ms，net 152 ms（`docs/stage24-ackonly3.md`） |
| **`gkbf=1`** | **有效负结果**：records/宿主成本砍到 1/5–1/30，**fps 不动**（6.86 vs 8.04）——帧节拍不是 CA（`docs/stage24-gkbf.md`） |
| `fb_noblit` 判据写错的两发 | 作废（把 sw 跟 fPanelWidth 比），日志 `logs/s24-noblit1/2/` |
| virtio-gpu 传输/脏矩形 | **死**：A/B 1.80 → 1.85 fps（噪声内），push 7.3–8.3 ms 只占 3% |
| Metal 直通 | 搁置：隔壁 dada 项目实测对象图 1158+711 方法，未开始 |
| RLE / 壁纸 / 文字虚线 | 死（输入黑 / 无位图源 / 已冻结） |

**归因更正**：交接单以前说的「CA 71 ms」实测是 `MiniIOMobileFramebuffer::drawLayer()` 的整屏 blit
（`display 750x1624 on a panel of 1125x2436`，逐像素 `fb_filter=1` 混合放大），不是 CoreAnimation。
`gkbf` 把它前后的驱动耗时都量成 62–79 ms/swap，一模一样，这就是为什么砍 CA 不动 fps。

## 四、当前进行中
- 无。上一发 `logs/s24-pair-noblit/` 已于 17:17 正常收尾；现在没有 qemu / host_glwire 在跑。

## 五、下一步优先级
1. **定稿交付**：把 13.9 fps / 75 ms + 画面（`docs/stage24-blitfast.md`）写进 `README.md` 与演示页。
2. 把 `fb_noblit=1` 变成默认（remote-only 下自动开），避免手写 boot-arg 漏掉。
3. 想要更高 fps：现在 CPU/帧 75 ms 里，客机 CA 记录只剩 4–19 KB/帧，剩下的是 SpringBoard 自身
   TCG 时间；下一步该测 `fbscale`/分辨率与 CA 子阶段细分，而不是继续砍 wire。
4. 想在负载机器上拿到可复现数字：用 `chrt`/`taskset` 给 qemu 与 host_glwire 分核。


## 六、运维配方（每次 boot 都要照做）
- 机器：`/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`；boot 前把 `qemu/vars.fd` 重写成 64MB `0xFF`。
- **一次只跑一发**；boot 前 `pkill -f 'qemu-syste[m]-aarch64'`（注意别把自己的 shell 也 match 掉，用脚本文件跑 pkill）。
- 现成脚本（本会话留在 `/tmp`，机器重启会没）：`/tmp/s24-run.sh TAG "CMDLINE"`（host_glwire ack-only + 静态补丁 + qemu，
  45 min 守护）、`/tmp/s24-mon3.sh TAG`（10 s 采样帧数/qemu CPU/host CPU/loadavg 到 `samples.csv`）、
  `/tmp/s24-sup3.sh TAG 600`（到 600 帧再停）、`/tmp/s24-pair.py A B`（成对比较 fps/CPU 每帧/load 匹配）。
- 现役启动参数：
  `CMDLINE="virtio_gpu=2 virtio_sound=1 virtio_net=1 cpus=1 fb_beat=0 fbscale=2 metal=1 gkbf=1 glwire_ack_only=1 fb_noblit=1 launchd_disable_all_timeouts=1"`，
  host 侧 `GLWIRE_NO_PANEL=1 GLWIRE_STATIC_PATCH=1 GLWIRE_STATIC_PATCH_FILE=/tmp/patch-full.bgra GLWIRE_ACK_ONLY=1 GLWIRE_SHM=<logdir>/shm.bin`。
- 快画面网页：`python3 tools/glwire/webview.py --port 6083 --crop 1624`（宿主回放帧 → JPEG 流 + QMP 点按）。
- 首帧窗口约 500 s，但负载高时会到 12–16 min；boot 参数给 2700 s 以上。日志落 `logs/<实验名>/`。
- 看帧率只看**墙钟**：`samples.csv` 的帧数增量/秒（`hakios-gl: f N at X s` 是客机时间，空闲时会低估）。
- 侧写：`/dev/shm` 快照转 PNG 用 `/tmp/s24-shot.py <shm.bin> <out.png>`（40 字节头 + BGRA）。
- 报告必带 system md5 + lib md5 + kernel md5；工作基线 system `8bf5f838`。
- scion 命令必须在 `/home/lshan/DSH-desktop/iosvm` 下执行；`scion message` 用 `--body-file`（heredoc 直接跟参数会报错）。

## 七、省 token 规则（主人明确要求撑到本周日）
1. 每轮最多 2 条 dispatch，单条 ≤400 字，只派一个可量化实验。
2. 成员报告 ≤10 行，只要数字和文件路径；日志写文件，不贴。
3. 不读大文件、不复述历史；git 只看 `git log --oneline -1`。
4. 同一方向失败 ≥3 次进判死清单；但「参数空转」不算方向失败。
5. 看上下文水位：`scion look <agent> | grep -o 'ctx [0-9]*%'`；>300k 就让其压缩状态后少派活。

## 八、关键路径
| 用途 | 路径 |
|---|---|
| 项目仓库 | `/home/lshan/DSH-desktop/iosvm` |
| 内核/客机仓库 | `/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel` |
| 宿主回放 | `tools/glwire/host_glwire.c`（`publish_picture` 写 `/dev/shm/glwire-frame`） |
| 网页快画面 | `tools/glwire/webview.py` + `webview.html` |
| 客机 GL 侧 | `userland/src/eagl_remote.c`（census / up​load / ack 路径都在这里） |
| 实验日志 | `logs/stage24/*` |
| 运维流水账 | `/home/lshan/dsh.MD/Scion-巡检定时任务.md`（每轮追加一段） |
| 演示页 | `demo-web/index.html`（8091）、快画面 6083 |

## 九、接手第一件事
1. 读 `/home/lshan/dsh.MD/Scion-巡检定时任务.md` 最后两段，确认 exp/code 没有在跑的活。
2. 用 `schedule_edit` 把每 5 小时的巡检任务目标改成**新会话自己**（`target: this_session`）。
3. 继续第四节的实验；有结论再报主人，不要空转。
