# stage 24：帧节拍的真大头 = 客机每次 swap 的整屏放大 blit（62–70 ms），`fb_noblit=1` 拿掉

2026-11-17 16:20Z 实测。同一发配置：remote-only + 全屏补丁 + host `GLWIRE_ACK_ONLY=1` + guest `glwire_ack_only=1 gkbf=1`。

## 定性（日志实证）
- 驱动自报：`display 750x1624 on a panel of 1125x2436 (4500 bytes per row)`。
- 每次 swap 的源/目标是 `from 0,0 750x1624 to 0,0 750x1624`，映射到面板 **1125x2436 = 2.74 M 像素**；
  `fb_filter` 默认 1 → 每帧在 TCG 里做 **274 万像素的逐像素混合放大**，驱动自报 **62–70 ms/swap**。
- remote-only 下客机根本不读面板（宿主靠 records 重放，`GLWIRE_ACK_ONLY` 不回写），这一整块是纯浪费。
- 这也就是交接单里被叫作「CA 71.0 ms」的那一块：它其实是 `MiniIOMobileFramebuffer::drawLayer()` 的整屏 blit，
  `gkbf=1` 前后一模一样（见 `docs/stage24-gkbf.md`）。

## 改动（客机内核 display 驱动，`xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`）
1. `fb_noblit=1`（默认 0）：几何全屏 swap（`sx=sy=0、sw/sh=fWidth/fHeight、px0/py0=0、px1/py1=fPanelWidth/Height、4 字节/像素`）时整段跳过，不写 `fBase`。
2. 保留 `filter==0 && bytesPerRow==fRowBytes` 时的整块 `memcpy` 快路径；其余仍走原路径。
3. 5 秒汇总行末尾加 `N not blitted`，用于确认真的生效。

## 数字
| 指标 | 对照（gkbf=1，无 noblit） | `fb_noblit=1` |
|---|---:|---:|
| 驱动自报 `us each in the driver` | 61 000–70 000 µs/swap | **24–35 µs/swap** |
| `not blitted` 计数 | 0 | 581–624（每 5 s 窗口） |
| 墙钟帧率（成对测：`s24-pair-plain` vs `s24-pair-noblit`） | 10.2 fps 中位（8 窗，load 6.2–9.9） | **13.9 fps 中位（8 窗，load 5.5–7.9）** |
| 同 load 匹配的 4 对窗口 | 12.8 fps 中位 | **14.8 fps 中位** |
| 客机 CPU/帧（qemu utime+stime） | 105 ms | **75 ms** |
| 画面（`/dev/shm` 快照） | — | 与 ackonly3 基线 **sha256 完全相同** |

md5：root `7c7da1a5e4dc5cb6044e4e813f001ae3`（blitfast4）、kernel `c153053e3b6e374ae9c5f05c6ca1f508`、
system `49ae3cf4fd53334b475a921eb9915008`、lib `40463969cfb9cb7be14a20ded6042a05`。
日志：`logs/s24-ab-noblit/`（有效）、`logs/s24-noblit3/`、`logs/s24-noblit2/`（判据写错的两发，作废）。
注意：客机时钟在空闲时走得比墙钟慢，用 `hakios-gl: f N at X s` 的 guest 时间戳算 fps 会低估。
墙钟必须用帧计数/秒（monitor 采样）算。

## 判读
- 62 ms 是**客机 CPU**（单 vCPU、TCG 上 100% 核），不是等待；拿掉后 CPU/帧 105 → 75 ms，帧率 10.2 → 13.9 fps（中位）。
- **≤91 ms/帧（11 fps）达成**：`fb_noblit=1` 的稳态中位 75 ms / 13.9 fps（成对测，同负荷窗口对比 14.8 vs 12.8 fps）。
- 墙钟 fps 仍随宿主负载大幅波动（隔壁 win2022-gpu 占 3.4 核；load 6→10 时 fps 掉一半），
  单发绝对值不可比，必须成对测或看 CPU/帧。
- 若哪天必须让客机自己的屏也对，`fb_filter=0` 是无需重建的退路（最近邻放大，视觉更硬）。

证据：`grep -a 'us each in the driver' logs/s24-pair-noblit/serial.log`、`logs/s24-pair-plain/samples.csv`、
`logs/s24-pair-noblit/samples.csv`（10 s 采样：帧数、qemu/host CPU、loadavg）。
