# stage 24 records 体积与去重空间（离线）

数据：`logs/stage24/noread/host.log`（3381 帧）、`logs/stage24/full-patch/host.log`（6531 帧），逐帧取 `frame ... records B` 与 `replay ... opcodes` 直方图。③④ 缺逐条顺序，用与 18928 B 9-draw 帧字节一致的存档流 `logs/s22-ext/ext28/records/r1late-100-750x1624-23765.records`（去掉 201 后 23765-4837=18928 B，89 条）判定；该帧型占两发 27.1%/28.1%，9-draw 帧合计占 58.2%/60.3%。

| 指标 | noread | full-patch |
|---|---:|---:|
| 帧数 | 3381 | 6531 |
| records/帧 均值 | 12650 B | 12672 B |
| records/帧 p95 | 19204 B | 19204 B |
| draw/帧 均值 | 6.32 | 6.33 |
| draw/帧 p95 | 9 | 9 |
| 9-draw 帧占比 | 58.2% | 60.3% |
| ③ 连续重复 state（18928B 帧） | 0/37 = 0% | 0/37 = 0% |
| ③ 重复 state 占全部 records | 0/89 = 0% | 0/89 = 0% |
| ④ 重复绑 texture（同 unit） | 0/10 = 0% | 0/10 = 0% |
| ④ 重复绑 texture（同 name） | 2/10 = 20%（unit0/unit1 解绑） | 同 |
| ④ 重复绑 program（同帧） | 1/5 = 20% | 1/5 = 20% |
| ④ program 重复折算全帧（按 9-draw 占比外推） | 0.58 次/帧，14.7% | 0.60 次/帧，15.5% |

去重最多能省：records 全删的上限也只有 0.0126 MB/帧（picture 4.87 MB 的 0.26%），按 copy 46.8 ms 折算 ≤0.12 ms/帧；实测重复 state 0%、program 仅 9-draw 帧重绑 1 次（20% 的 program 绑定）、texture 重复都是解绑，去重实际收益≈0。
