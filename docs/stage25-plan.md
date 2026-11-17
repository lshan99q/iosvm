# stage 25 计划：从 13.9 fps（75 ms）到 15 fps（≤67 ms）

2026-11-17 18:05 起。前置：stage 24 已收口（13.9 fps / 75 ms，画面逐字节相同，见 `stage24-blitfast.md`）。

## 阶段树

```
IOSVM
└─ stage 24 帧节拍 ≤91 ms（✅ 2026-11-17 收口，13.9 fps / 75 ms）
   ├─ 24.1 wire 往返：异步回包 / 双缓冲 / records 去重 → 判死（wait 1 µs，records 仅 12.65 KB/帧）
   ├─ 24.2 ack-only：copy 46.8 → 9.2 ms，但 CA 71 → 144 ms（有效负结果，docs/stage24-ackonly3.md）
   ├─ 24.3 gkbf：records 90 KB → 4–19 KB/帧、宿主 41 → 1.4 ms，fps 不动（有效负结果，docs/stage24-gkbf.md）
   ├─ 24.4 display 整屏 blit：fb_noblit 62 ms → 24 µs（docs/stage24-blitfast.md）✅
   ├─ 24.5 fb_noblit 自动识别（boot-args 有 glwire_ack_only=1 即跳过）✅
   └─ 24.6 交付：成对测 13.9 fps / 75 ms + /dev/shm 画面 sha256 与基线相同 ✅

└─ stage 25 冲 15 fps（≤67 ms/帧）— 现在
   ├─ 25.1 E1：fbscale=1（显示面 375x812，CA 光栅像素 ÷4）        [exp 跑着]
   ├─ 25.2 E2：CA 逐函数时间 census（若无 E1 不够，按数据砍）       [code 待命]
   ├─ 25.3 E3：依据归因再砍（CA 记账 / 跳帧合并 / 半分辨率资源）
   └─ 25.4 成对测 + 画面复核 + 收口文档（README、handover、demo）
```

## 当前账（13.9 fps 时，CPU/帧 75–81 ms）
| 项 | 值 |
|---|---|
| display 整屏放大 blit | 24–48 µs（已废） |
| 宿主回放 | ~1.4 ms/帧（records 4–19 KB） |
| 客机 CA 记录 + SpringBoard/backboardd TCG | 剩下的全部，约 75 ms |
| 端点 | 单 vCPU 100% 核；墙钟 fps 随宿主 load 线性掉 |

## E1 判据（已在跑）
- 配置：`fbscale=1`（其余同现役：`gkbf=1 glwire_ack_only=1`，`fb_noblit` 自动），一发，600 帧后停。
- 通过：墙钟中位 ≥15 fps 或 CPU/帧 ≤67 ms，且画面仍满足 Dock 4 图标 / Search 居中 / 下半屏不缺。
- 不通过：桌面/文字崩（375x812 缺 2x 资源）则退 `fbscale=2`，转 E2 归因。

## 测法纪律
- 只看墙钟：`samples.csv` 帧数增量/秒（10 s 采样，含 loadavg）。客机时间戳会低估，不能用来算 fps。
- 单发绝对值不可比：必须成对/同 load 窗口；`/tmp/s24-pair.py A B`。
- 报告必带：system md5 `49ae3cf4`、lib `40463969`、root/kernel md5、日志路径。
