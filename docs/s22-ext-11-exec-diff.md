# ext9 日志差分：exec 成功的 daemon vs 卡住的 widget 扩展

数据源：`logs/s22-ext/ext9/serial.log`（ext9 标准配方、到 UI 660s）。
两条生命周期从 `WILL_SPAWN` 到 `SPAWNED` 逐行并排。

## A. exec 成功的 daemon（notifyd，pid 29）

```
10:45:48.072068 (system/com.apple.notifyd) <Notice>: internal event: WILL_SPAWN, code = 0
10:45:48.072242 (system/com.apple.notifyd) <Notice>: service state: spawn scheduled
10:45:48.072291 (system/com.apple.notifyd) <Notice>: service state: spawning
10:45:48.072972 (system/com.apple.notifyd) <Notice>: launching: event publisher
10:45:48.359004 (system/com.apple.notifyd [29]) <Notice>: xpcproxy spawned with pid 29
10:45:48.359260 (system/com.apple.notifyd [29]) <Notice>: internal event: SPAWNED, code = 0
10:45:48.416520 (system/com.apple.notifyd [29]) <Notice>: internal event: SOURCE_ATTACH, code = 0
```

## B. 卡住的 widget 扩展（BatteriesWidget，pid 49）

```
10:55:57.335406 (pid/32/com.apple.Batteries.BatteriesWidget) <Notice>: created the implicit endpoint upfront
10:55:57.338872 (pid/32/com.apple.Batteries.BatteriesWidget) <Notice>: service will attempt to elide xpcproxy
10:55:57.338938 (pid/32/com.apple.Batteries.BatteriesWidget) <Notice>: draining messages from com.apple.Batteries.BatteriesWidget
10:55:57.338984 (pid/32/com.apple.Batteries.BatteriesWidget) <Notice>: draining messages from com.apple.Batteries.BatteriesWidget.apple-extension-service
10:55:57.395392 (pid/32/com.apple.Batteries.BatteriesWidget.EA…) <Notice>: service will attempt to elide xpcproxy
10:55:57.399281 (pid/32/com.apple.Batteries.BatteriesWidget.EA…) <Notice>: internal event: WILL_SPAWN, code = 0
10:55:57.455928 (pid/32/…EA…) <Notice>: service state: spawn scheduled
10:55:57.456007 (pid/32/…EA…) <Notice>: service state: spawning
10:55:57.456723 (pid/32/…EA…) <Notice>: launching: launch job demand
10:55:57.670028 (pid/32/…EA… [49]) <Notice>: service spawned with pid 49
10:55:57.670242 (pid/32/…EA… [49]) <Notice>: internal event: SPAWNED, code = 0
10:55:57.670302 (pid/32/…EA… [49]) <Notice>: service state: spawned
10:55:57.670503 (pid/32/…EA… [49]) <Notice>: service state: running
10:55:57.670612 (pid/32/…EA… [49]) <Notice>: internal event: INIT, code = 0
10:55:57.670664 (pid/32/…EA… [49]) <Notice>: job state = running
10:55:57.724804 (pid/32/…EA… [49]) <Notice>: Successfully spawned BatteriesWidgetExtension[49] because launch job demand
10:55:57.725378 (pid/32/…EA… [49]) <Notice>: internal event: SOURCE_ATTACH, code = 0
```

## 差异（关键）

| 行 | daemon（A） | widget（B） |
|---|---|---|
| `xpcproxy spawned with pid` | **有**（29） | **无** |
| `service spawned with pid` | 无 | **有**（49，但这是 launchd 的 spawn，不是 xpcproxy） |
| `service will attempt to elide xpcproxy` | **无** | **有**（两次） |
| `draining messages from …` | 无 | **有**（含 `…apple-extension-service`） |
| `uncorking exec source upfront` | **有**（后续行） | **全 log 无 widget 的 uncorking** |
| `internal event: SPAWNED` | 有 | 有 |
| `service state: running` / `INIT` | 有 | 有 |
| `internal event: SOURCE_ATTACH` | 有 | 有 |

## 判读

1. **A 走的是 xpcproxy 真 exec**（`xpcproxy spawned with pid 29` → `uncorking exec`），
   进程是 xpcproxy 拉起的真实程序。
2. **B 被 elide**（`service will attempt to elide xpcproxy` 两次、`draining messages`
   两条），launchd 自己在 pid 49 里代跑，**没有 xpcproxy spawn、没有 uncorking
   exec** → 这正是 ext9「extension 卡在 spawn 握手、exec 未发生」的日志证据。
3. ext9 全 log 里 widget 的 `uncorking exec` = 0；`uncorking exec` 54 次全是
   daemon/系统进程（SpringBoard/notifyd/logd/containermanager 等），与
   `xpcproxy spawned` 一一对应。
4. 结论：**widget 扩展走 elide 路径（不 exec），与 daemon 的 xpcproxy 路径不同**；
   要修 CheckIn，得先让 widget 扩展真正 exec（或修 elide 路径下的 CheckIn）。

## 归档

`/host/iosvm/logs/s22-ext/ext11/exec-diff.md`（本文件）；摘要入库
`docs/s22-ext-11-exec-diff.md`。
