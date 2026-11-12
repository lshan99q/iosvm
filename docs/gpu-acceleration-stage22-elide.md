# S22 elide xpcproxy: skips the helper, still execs the appex

> **Correction (ext14b):** the earlier reading 「elide = the target never execs」 is
> **wrong**.  launchd's elide path skips the **xpcproxy helper**, then directly
> execs the appex.  The missing `uncorking exec source upfront` line is a logging
> difference of the elide path, not evidence of no exec.

## 1. Two spawn paths

- **xpcproxy path**: launchd forks a child, child execs `/usr/libexec/xpcproxy`;
  logs `xpcproxy spawned with pid N` → `SOURCE_ATTACH` → `uncorking exec source
  upfront` → `created`; xpcproxy then execs the real program.
- **elide path** (`launchd_proxyless_spawn` default for many extension jobs): no
  xpcproxy process; launchd logs `service will attempt to elide xpcproxy`,
  `draining messages`, then **directly execs the appex**.
  - `service spawned with pid N` is the forked child; **after exec its p_comm is
    the real extension name**, not `launchd`.
  - The elide path does **not** print `uncorking exec source upfront`; that line is
    not required for exec.

## 2. ext14b: exec trace proves exec

Kernel `7a4dff5340ad26786e9dc70f26e69016` prints
`hakios: exec pid=%d path=%s`.  ext14b captured:

```
exec pid=45 .../BooksWidgetExtension
exec pid=47 .../PeopleWidget_iOSExtension
exec pid=48 .../WeatherWidget
exec pid=49 .../mobilenotes.WidgetExtension
exec pid=50..53 AppStore/Passbook/FindMy/Sleep
```

- **These extensions did exec.**  The earlier 「widget never execs」 conclusion is
  withdrawn.
- After exec: **no exit/crash** for these pids (still alive),
  `checked in as alive` = 0, whole-log `hakios: host_info` = 0.
- Therefore the stall is **after exec, before host_info / the extension's own
  startup** — not at launchd's spawn gate.

## 3. Why the old reading looked right

- ext9/ext11 showed `service will attempt to elide xpcproxy`, no `xpcproxy spawned`,
  no `uncorking exec` → looked like "no exec".
- ext14b's exec trace shows that sequence is **the elide path's normal logging**;
  the exec happens without xpcproxy and without the uncork line.
- ext9 also had no exit/crash and CheckIn 0; that part still holds.

## 4. Sampling: names change at exec

- Before exec, the child's p_comm is `launchd` / launchd(proxyless); after exec it
  is the real extension name.
- ext12 `ustacks=GCWidgets` and ext13 `ustacks=launchd` both got **0 hits** because
  the names were wrong; ext13's actual launcher is `/sbin/launchd.darwin`
  (p_comm likely `launchd.darwin`).
- **ext15**: sample the true post-exec names (`BooksWidgetExtension`,
  `PeopleWidget_iOSExtension`, `WeatherWidget`, `mobilenotes.WidgetExtension`),
  preferably by pid if the tool supports it.
- Method lesson: confirm the real p_comm from the `exec pid=... path=...` trace;
  do not use log short names.

## 5. What to look for next

- ext15 stack in dyld/libobjc/UIKit/`NSExtensionMain` → extension's own early
  startup, after exec;
- stack in `pthread_init → host_info` → same point ext3 once caught (but host_info
  itself is healthy, ext11);
- stack in `xpc_main`/`bootstrap_check_in` → CheckIn endpoint path;
- no stack / pid gone → exit/crash path.

## 6. Not doing

- **不再用「elide = 不 exec」解释**：ext14b 已证 exec 发生；
- **不再拿 host_info 当瓶颈**：ext11 probe2 全部 kr=0；
- 不再用日志短名做 p_comm 采样目标；
- 不再调 timeout/job/watchdog；不碰宿主 GPU；不启动 virtio-gpu。
