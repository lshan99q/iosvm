# Stage 20 rawpipe — 原始 TCP 吞吐探测（guest net_test → host 64MB）

**Verdict: net_test 通道实测 64.6 MB / 95.31 s ≈ **0.68 MB/s**（连接 12.82 s），
比 glwire 自己的 4.5 MB/s 还低。所以「4.5 MB/s 是 wire 上限」不成立——wire 至少
能跑 4.5 MB/s（glwire 实测）；net_test 的 0.68 是它 userspace recv 路径更慢。
按 owner 口径：原始吞吐不是 ≥20，也不到 ~5 的干净上限；**glwire 4.5 MB/s 已接近
这条通道的可用水平，下一步应靠压缩（RLE 13.5x）而不是换传输。** **

## Recipe（config-only，不改代码）

- root `/tmp/root-w15-rawpipe.apfs` `d813c858…`（785bcbac + `rc.vtop` 里临时加
  `/redwood/bin/net_test tcp 10.0.2.2 9999 &`，测完已还原）；
- host python 9999 监听发 `/tmp/64mb.bin`（64 MiB 零）；
- guest `net_test tcp 10.0.2.2 9999`；system `8bf5f838`；`glwire_local=0`。

## Numbers

| 项 | 值 |
|---|---|
| guest 连接 | 12.82 s |
| guest recv | **64,640,920 B in 95.31 s ≈ 0.68 MB/s** |
| host 发送 | `sent 67108864 bytes` |
| glwire 对照 | 2.44 MB in ~547 ms ≈ **4.5 MB/s** |

## 判断

- 原始 net_test 只有 0.68 MB/s：低于 glwire 的 4.5，说明 4.5 不是 wire 天花板；
  net_test 的 recv 路径（userspace、无优化）更慢。
- 4.5 MB/s 已接近这条 QEMU slirp 通道的可用水平 → **要 ≥3 fps 端到端，必须靠
  压缩（RLE 13.5x 实测 10.96 MB→810 KB）或降分辨率**，而不是继续换传输。
- 连接 12.82 s 提示 guest 网络栈启动慢，与本问题无关。

Artifacts: `/host/iosvm/logs/s20-rawpipe/`；rc.vtop 已还原（备份 `/tmp/rc.vtop.rawpipe.bak`）。
