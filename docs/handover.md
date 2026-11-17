# 交接单：IOSVM stage 24（2026-11-17 12:55）

> 读完这一页就能接手，不需要翻历史聊天。所有数字都是实测。

## 一、主线与目标
- 项目：IOSVM —— 在 QEMU/TCG 的 aarch64 客机里跑 Apple iOS 16.5 用户态；客机把 CoreAnimation 的
  GL 调用记录后送到宿主，宿主用真 GL（V100）回放并合成画面。
- **本周目标**：客机帧节拍 **≤91 ms（11 fps）**，且画面完整（Dock 4 图标 + Search 居中 + 下半屏不缺）。

## 二、当前最好成绩（可直接引用）
- **9.09 fps**，完整画面。单帧 117.1 ms = **CA 71.0 ms + copy 46.8 ms + wait 1 µs**。
- 宿主侧成本 41 ms/帧（panel 35.8 + 其余 ≈5），**宿主不是瓶颈，能到 ~25 fps**。
- main：`432afbc`（wire_reset before ack-only/noread early returns）。

## 三、已验证的负结果（别重做）
| 尝试 | 结果 |
|---|---|
| 异步回包 / 双缓冲 | **死**：wait = 1 µs，客机根本没等宿主 |
| records 去重 | **死**：records 仅 12.65 KB/帧（图 4.87 MB 的 0.26%），去重 <0.12 ms |
| `GLWIRE_NOREAD` / `upload_fast` / `upload_scale=2` / `nopanel_up` | 全部空转（不作用于 in-place 路径） |
| `GLWIRE_ACK_ONLY` + `glwire_ack_only` | copy 46.8 → **9.2 ms** 成功，但 **CA 71 → 144.2 ms**，net 152 ms / 8.04 fps（`docs/stage24-ackonly3.md`） |
| virtio-gpu 传输/脏矩形 | **死**：A/B 1.80 → 1.85 fps（噪声内），push 7.3–8.3 ms 只占 3% |
| Metal 直通 | 搁置：隔壁 dada 项目实测对象图 1158+711 方法，未开始 |
| RLE / 壁纸 / 文字虚线 | 死（输入黑 / 无位图源 / 已冻结） |

## 四、当前进行中
- exp 正在测：**ack-only + 客机 `gkbf=1`**，看 CA 能否从 144 压回去。
- 判据：CA 回到 ~75 ms → 75+9.2 ≈ 85 ms ≈ **11.8 fps，目标达成**；
  CA 压不动 → 收口在 **9.09 fps**，把结论写清楚，不修饰。

## 五、下一步优先级
1. CA 压缩（`gkbf=1`，及 census 的 CA 子阶段细分）——唯一还没动的 71 ms 大块。
2. 若 ack-only 的 copy 收益拿不回来，就固定 9.09 fps 方案（remote-only + 全屏静态补丁）当交付。
3. 收尾：把最终数字写进 `README.md` 与 `docs/stage24-*`，别留未证结论。

## 六、运维配方（每次 boot 都要照做）
- 机器：`/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`；boot 前把 `qemu/vars.fd` 重写成 64MB `0xFF`。
- **一次只跑一发**；boot 前 `pkill -f 'qemu-syste[m]-aarch64'`（注意别把自己的 shell 也 match 掉，用脚本文件跑 pkill）。
- 启动脚本：`/tmp/stack-start.sh`（host_glwire + qemu）与 `/tmp/stack-stop.sh`（全停）。
- 快画面网页：`python3 tools/glwire/webview.py --port 6083 --crop 1624`（宿主回放帧 → JPEG 流 + QMP 点按）。
- 首帧窗口约 500 s，boot 参数给 7200；日志落 `logs/stage24/<实验名>/`。
- 报告必带 system md5 + lib md5；工作基线 system `8bf5f838`。
- scion 命令必须在 `/home/lshan/DSH-desktop/iosvm` 下执行；`scion message` 用 heredoc。

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
