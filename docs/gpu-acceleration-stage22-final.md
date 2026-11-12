# Stage 22 final: extensions exec, then stall before CheckIn; host_info is healthy

**Status: closed to the evidence boundary.**
ext14b's kernel exec trace proves the extensions **do exec**; they then stall in
their own startup before `host_info`/CheckIn.  The earlier 「elide = never exec」
reading was wrong.

## 1. Effective experiments

| run | configuration | observation | evidence |
|---|---|---|---|
| **ext2** | `launchd_disable_all_timeouts=1`; no serial; >1000 s | 5 s hanging/CHECK_IN_TIMEOUT/killing **0**; widgets spawn; `checked in as alive` **0** | `logs/s22-ext/ext2/` |
| **ext3** | timeout disabled + `ustacks=PodcastsWidget` 90 samples | stack in `pthread_init → host_info → mach_msg2`; pid alive; no CheckIn | `logs/s22-ext/ext3/`; `docs/s22-ext-03-symbolized.txt` |
| **ext8** | kernel `1f8c489f…`, default hostport/host_info traces; system processes | hostport **35**（id 200×6 / 205×23 / 206×6）；host_info **15**，全部 ret=0 | `logs/s22-ext/ext8/` |
| **ext9** | standard recipe; UI 660 s; timeouts off | GCWidgets[48]/BatteriesWidget[49] spawn, 70 INIT lines, `running`; **no `uncorking exec source upfront`**; hanging 0; CheckIn 0; hostport/host_info 0 | `logs/s22-ext/ext9/` |
| **ext11** | probe2 before exec; kernel `1f8c489f…` | BASIC `kr=0 ncpu=1`; LOAD `kr=0`; `host_statistics64 kr=0`; crash at **empty-header `mach_msg`** (no reply) | `logs/s22-ext/ext11/` |
| **ext14b** | kernel **`7a4dff5340ad26786e9dc70f26e69016`** with `hakios: exec pid=%d path=%s` | exec pid=45…53: Books/People/Weather/mobilenotes + AppStore/Passbook/FindMy/Sleep; all **no exit/crash**; CheckIn 0; host_info 0 | `logs/s22-ext/ext14b/` |

(ext4 trace 0 is superseded; ext5 sampled the wrong name and is not counted.)

## 1.1 ext11: `host_info` is completely normal

probe2's 10 lines:

```
probe2: after host_info basic kr=0 count=12 ncpu=1 memory=2147483648 max_mem=6442450944
probe2: after host_info load kr=0 count=5 avenrun0=1
probe2: after host_statistics64 kr=0 count=38 free=233352
probe2: before empty send (header only, no reply)
```

- **BASIC / LOAD / host_statistics64 all `kr=0`** → host port MIG and kernel
  `host_info` work for a plain guest executable.
- **「扩展卡在 host_info」彻底排除**；ext3 的栈是那个 boot 的瞬时状态，不能当
  普遍根因。
- 崩点是一条**空 header、无 reply 的 `mach_msg`**：只有 `before`、没有 `after`。
  这是 probe 的边界 bug/已知异常，不是 host_info 阻塞。

## 1.2 ext14b: extensions DO exec; elide only skips xpcproxy

Kernel `7a4dff5340ad26786e9dc70f26e69016` prints `hakios: exec pid=%d path=%s`;
ext14b captured:

```
exec pid=45 .../BooksWidgetExtension
exec pid=47 .../PeopleWidget_iOSExtension
exec pid=48 .../WeatherWidget
exec pid=49 .../mobilenotes.WidgetExtension
exec pid=50..53 AppStore/Passbook/FindMy/Sleep
```

- launchd's `service will attempt to elide xpcproxy` **only skips the xpcproxy
  helper**; launchd still directly executes the appex.
- The missing `uncorking exec source upfront` in ext9/ext11 is just because the
  elide path does not print that line — **not evidence that exec did not happen**.
- The earlier 「widget never execs」 conclusion is **withdrawn**.
- After exec: **no exit/crash** for these pids (still alive), `checked in as alive` = 0,
  and whole-log `hakios: host_info` = 0.
- Therefore the stall is **after exec, before host_info / the extension's own
  startup**.

## 1.3 ext8 and ext9 side observations

- ext8: host port/MIG + host_info healthy for the system processes it observed
  (35 hostport; 15 host_info, all ret=0).  It predates the extension, so it is a
  **positive control**, not an extension observation.
- `com.iosvm.reg [12]: xpcproxy exited due to exit(78)`:
  - **exit 78 = `EX_CONFIG`**; here it is a missing helper:
    `/redwood/bin/reg_app` (errno 2, `0x6f` Invalid/missing Program).
  - `userland/src/reg_app.c` exists, but `userland/build.sh` never builds/copies it.
  - **与 extension 无关**；作为独立小修记录（build `reg_app` + sign entitlements）。
- `EXC_CORPSE_NOTIFY error 5 for pid 24`: pid 24 is **`configd`**, not the
  extension; error 5 is `KERN_FAILURE` from a corpse without crash metadata.
  Bystander, not the CheckIn blocker.

## 2. Corrected conclusion

1. **Extensions do exec** (ext14b): pids 45–53, real appex paths.  The elide path
   only bypasses xpcproxy.
2. **After exec they stall before host_info/CheckIn**: no exit/crash, CheckIn 0,
   whole-log `host_info` 0.  The blocker is in the extension's own startup after
   exec, before the libSystem/host_info point ext3 once caught.
3. **Historical explanations all excluded**: host_info (ext11 all kr=0), timeout
   (ext2), job type (earlier), and 「elide = no exec」 (ext14b).
4. **exit 78 / corpse error 5 are not the blocker**: exit 78 = missing `reg_app`
   helper; pid 24 = configd bystander.
5. **Note**: ext3's host_info stack was a transient state of that boot, not the
   universal path.

## 3. Unresolved questions (handoff)

1. **Elide path**: why does the widget job take `service will attempt to elide xpcproxy`
   while daemons exec?  Can the widget be forced to exec, or does CheckIn have to
   be fixed under the elide path?
2. **CheckIn prerequisites after exec**: bootstrap namespace, MachServices
   endpoint, signature/entitlement/sandbox, persona; none reached yet.
3. **reg_app build omission**: independent fix (compile + copy + sign); does not
   explain extensions, but should be fixed to remove noise.

## 4. Next experiments

### 4.1 ext12/ext13 misses and ext15 true-name sampling

- **ext12**：`ustacks=GCWidgets` **0 命中**（303 条 no-process）；GCWidgets 不是
  p_comm。
- **ext13**：`ustacks=launchd` **0 命中**；实际是 `/sbin/launchd.darwin`。
- **原因**：exec 之前 p_comm 是 launchd(proxyless)/`launchd`；**exec 之后 p_comm
  就是真名**（`BooksWidgetExtension`、`PeopleWidget_iOSExtension`、`WeatherWidget`、
  `mobilenotes.WidgetExtension` 等）。
- **ext15**：用 ext14b 的真名采样（BooksWidgetExtension 等），不再用日志短名。
- **方法教训**：采样前先用 exec trace 确认真实 p_comm；exec 前后名字不同。
- 判据：栈在扩展自身启动（dyld/libobjc/UIKit/NSExtensionMain）还是
  `pthread_init → host_info`；不再猜 launchd。

### 4.2 Exec-boundary instrumentation (next boot)

- `bsd/kern/kern_exec.c` at `exec_activate_image`/posix_spawn:
  `hakios: exec pid=%d comm=%s path=%s ret=%d`;
- `bsd/kern/kern_exit.c` `proc_exit`:
  `hakios: exit pid=%d comm=%s status=%d`;
- optional `host_self_trap` trace (port/type) only to prove task context.
- Keep default-on hostport/host_info traces; do not rely on boot-arg at the end
  (`cmdline: 262 of 607` truncation).

### 4.3 Static checks (no boot)

- extension job plist: `ProgramArguments`, `POSIXSpawnType`, `MachServices`,
  sandbox, `EnvironmentVariables`;
- `xpcproxy` environment and bootstrap namespace (`user/501` vs `system`);
- signature/entitlements/sandbox of the extension binary;
- `reg_app` build omission fix.

## 5. Known anomaly (not this stage's task)

- **Empty-header `mach_msg` (no reply) crashes the process**: probe2 stops at
  `before empty send`, never reaches `after`.
- Recorded as a kernel/IPC boundary behavior; **not** part of the CheckIn task and
  not to be chased in stage 22.

## 6. Infrastructure gained

- Kernel build pipeline: cctools ld64 + delete intermediates + high-vmaddr/entry
  self-check + payload regeneration; `tools/kernel-rebuild.sh [marker...]`.
- Default-on `hostport` + `host_info` traces in kernel
  `1f8c489f2ca93043d53b20aa04739109`.
- `symstack.py`, `gate.sh`, `gate_names.py`; ext3 map in
  `docs/s22-ext-03-symbolized.txt`.
- Do not use the no-UI temporary image (`PREBUILT=1 … + EXTRA/RC_JOBS`: `ui=0s`)
  for conclusions; ext9/ext11 used the standard recipe.

## 7. Not doing

- **不再拿 timeout / job / watchdog / host_info 当解释**：已分别被 ext2/ext11/早前实验排除；
- **不再用「elide = 不 exec」解释**：ext14b 已证 exec 发生；
- **不再用 ext3 单发栈作结论**（瞬时状态）；
- 不再猜 boot-arg（追踪默认开）；
- 不碰宿主 GPU；不启动 virtio-gpu；
- 不写 P2/P3 已修：CheckIn 从未发生。
