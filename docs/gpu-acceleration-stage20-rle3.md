# Stage 20 rle3 — RLE 算法修复（unpack <150ms 达成）

**Verdict: unpack 算法修复生效。** guest `upload-rle ... packed 51,202 B (1%),
unpack 84–105 ms`（上一发 396–451 ms，降到目标 <150 ms）；host `bgra rle
... pack 0.9–1.0 ms`；`options raw=... parsed ... rle=1` 已确认开关生效。
但 guest 仍 3 帧/10.2 s、host 3 帧 —— 还有别的瓶颈（host 停帧/guest 饥饿）。

## Recipe

- root `/tmp/root-w15-rle3.apfs` `66aea479…`（785bcbac + lib `86468b11…`）；
- host `d80f7ca3…`（`rgb565=0, rle=1`）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_RLE=1 GLWIRE_UPLOAD_565=0"`。

## Numbers

| 项 | 值 |
|---|---|
| init 自报 | `options raw="..." parsed ... rle=1` |
| host pack | 51,202 B (1%)，`pack 0.9–1.0 ms` |
| guest unpack | **84–105 ms**（目标 <150 达成） |
| guest 帧 | 3 帧/10.2 s（0.29 fps，仍饥饿） |
| host 帧 | 3（停） |
| 截图 | 无 |

## 判断

- **unpack 算法修复有效**：396–451 ms → 84–105 ms，RLE 的 guest 端解包不再是主瓶颈。
- 但 guest 0.29 fps、host 3 帧仍在 → 需看 RLE 后的其他环节（host 停帧是
  独立问题）。
- 目标（host ≥3 fps、guest ≥5 fps）仍未达；下一步应由 code 查 host 3 帧停。

## 系列

| boot | RLE | pack | unpack | guest | host |
|---|---|---|---|---|---|
| rle-run | 未生效（工具） | — | — | 4/494 | 3 |
| rle2 | 生效 | 1.0 ms | 396–451 ms | 4/507 | 3 |
| **rle3（算法修复）** | 生效 | 0.9–1.0 ms | **84–105 ms** | 3/10.2 | 3 |

Artifacts: `/host/iosvm/logs/s20-rle3/`.
