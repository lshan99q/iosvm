# S22 extension check-in — 观测清单 + 最小干预剧本（草稿，未 boot）

## 1. 可复用观测清单（grep 串）

目标进程：widget/poster extension（如 `com.apple.Maps.GeneralMapsWidget.*`、
`com.apple.GradientPosterExtension.*`、`BatteriesWidget`）。guest log 为准；
host log 只记 GL/协议，不记 extension 生命周期（当前 host.log 里无这些串）。

| # | 事件 | guest.log grep 串 | 正常/异常 |
|---|---|---|---|
| ① | extension spawn | `xpcproxy spawned with pid`（配合 `grep -E 'Widget\|PosterExtension\|Extension'`） | spawn 后应很快有 ② |
| ② | INIT / CheckIn | `service state: INIT`；成功签到是 `checked in as alive` | `checked in as alive` = 0 即异常 |
| ③ | 崩溃 | `EXC_CORPSE_NOTIFY`（`error code: 5`） | spawn 后 ~0.5s 出现即命中 |
| ④ | 被杀 | `Extension is hanging on launch` / `killing extension hanging on launch` | 出现即 check-in 未完成 |
| ⑤ | 超时码 | `CHECK_IN_TIMEOUT` / `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` | 与 ④ 成对 |

参考基线（`logs/s19o-p2e-c2/`，强制 xpcproxy 那发）：`Extension is hanging`
36 次、`EXC_CORPSE_NOTIFY` 49 次、`CHECK_IN_TIMEOUT` 36 次、`CheckIn` 1 次、
`INIT` 76 次、`spawn` 326 次、`checked in as alive` **0** 次 → CheckIn 从未发出。

一键汇总（每发跑完贴进 report）：

```sh
G=guest.log
for p in 'xpcproxy spawned with pid' 'service state: INIT' 'checked in as alive' \
         'EXC_CORPSE_NOTIFY' 'Extension is hanging' 'CHECK_IN_TIMEOUT'; do
  printf '%-40s %s\n' "$p" "$(grep -ac -- "$p" "$G")"
done
grep -aE '(Widget|PosterExtension).*(spawned|hanging|CHECK_IN)' "$G" | tail -20
```

## 2. 最小干预剧本（草稿，等 code 假设排序后执行）

**原则**：只开 launchd/xpcproxy verbose；**不改** timeout/job/watchdog
（timeout=120、强制 xpcproxy、四类 job、`ExtensionWatchdog=false` 均已排除/有害）。

- Boot：`ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=3600
  UI_GRACE=900 MINSHOT=200000`；
- `XCMD="glwire_local=1 launchd_verbose=1 xpcproxy_verbose=1"`（若 boot-arg 名不同，
  按 code 给的为准；不额外加 timeout/job/watchdog）；
- `HOST_ENV=GLWIRE_NO_PANEL=1`；等待 **900 s**（足够覆盖 ①–⑤ 与一次
  `CHECK_IN_TIMEOUT`，又不拖到 1800s 层树）；
- 归档：`logs/s22-ext/<label>/`（guest.log、host.log、boot 行、shot、本清单汇总）。

**判定**：
- **有根因信号**：`EXC_CORPSE_NOTIFY` 之外出现具体的 dyld/符号/沙箱/权限错误、
  或 `checked in as alive` 变 >0、或 hanging 时间点前有明确的 last syscall/last
  log 行 → 记录并写结论，再决定下一发。
- **没信息**：只有 ③④⑤ 重复（和 s19o-p2e-c2 一样）→ 记为「verbose 无新增信息」，
  不重复同配置；等 code 的下一假设（最多再 2 发）。

## 3. 预算

- 3 发；每发之间**必须先写结论**（`logs/s22-ext/<label>/report.md`）再跑下一发；
- 单发等待 900 s，`one boot at a time`，跑前确认无残留 qemu/host_glwire。

## 4. 状态

- 本文件为**清单 + 剧本草稿**，未 boot；等 code 的假设排序落地后按其推荐执行。
