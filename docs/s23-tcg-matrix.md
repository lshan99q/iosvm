# Stage 23C — TCG 测试矩阵（2026-11-14）

**结论：只有 SMP=1 + 默认 tb-size=1536 能出帧（稳态 2–3.3 fps）；
SMP=2 / SMP=4 / SMP=4+MTTCG / SMP=1+tb-size=4096 全部 10 分钟 0 帧。
23C 在「不改 QEMU 参数」的前提下基本到顶。**

## 矩阵

| 配置 | 结果 | 归档 |
|---|---|---|
| SMP=1 + tb=1536（对照） | **2–3.3 fps**（ext37/38/39） | `logs/s22-ext/ext37–39/` |
| SMP=2 | **0 帧 / 10 min**，panic=0 | `logs/s23-tcg/smp2/` |
| SMP=4 | **0 帧 / 10 min**，panic=0 | `logs/s23-tcg/smp4/` |
| SMP=4 + `-accel tcg,thread=multi`（MTTCG） | **0 帧 / 10 min**，panic=0 | `logs/s23-tcg/mttcg/` |
| SMP=1 + `QEMU_TB_MB=4096` | **0 帧 / 10 min**，panic=0 | `logs/s23-tcg/tb4096/` |

## 参数核实（都真透传到 qemu）

- MTTCG：`run-darwin.sh:114` 的 `"$@"` 接 `QEMU_EXTRA`，cmdline 含
  `-accel tcg,thread=multi`；
- tb4096：cmdline 含 `-accel tcg,tb-size=4096`；
- 多核：cmdline `cpus=2/4` 已确认。

## 内核（所有发一致）

- payload `hakios_kver=2.1.13-hakios-darwin22.5-gae084ae0-dirty`（A1 内核 `ae084ae`）；
- payload kernel 段 md5 `320d048a2f9bb524ad79f32a4088bf00`；
- system `8bf5f838`；root `/tmp/root-w15.apfs`。

## 判断

- 多核（含 MTTCG）在这个 QEMU/TCG 环境下 guest 不出帧；
- tb-size 放大到 4096 反而 0 帧（可能 TCG 初始化/内存路径问题）；
- **SMP=1 + 默认 tb-size 是唯一可用配置**，2–3.3 fps 是当前稳态上限；
- 若要继续提速，需要换路径（virtio-gpu/A1、或减小 guest 渲染工作），
  不是 QEMU accel 参数。

Artifacts: `/host/iosvm/logs/s23-tcg/`（4 个子目录，各含 report/serial/host）。
