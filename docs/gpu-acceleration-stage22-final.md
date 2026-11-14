# Stage 22 final: extensions are slow, not stuck; the default watchdog kills them

**Status: closed to the evidence boundary.**
ext14b proved the extensions **do exec**; ext24 now gives the first
`extension checked in as alive` lines when timeouts are disabled.  The blocker is
**startup latency vs the default watchdog**, not a dead dyld path.

## 1. Effective experiments

| run | configuration | observation | evidence |
|---|---|---|---|
| **ext2** | `launchd_disable_all_timeouts=1`; no serial; >1000 s | 5 s hanging/CHECK_IN_TIMEOUT/killing **0**; widgets spawn; `checked in as alive` **0** | `logs/s22-ext/ext2/` |
| **ext3** | timeout disabled + `ustacks=PodcastsWidget` 90 samples | stack in `pthread_init → host_info → mach_msg2`; pid alive; no CheckIn | `logs/s22-ext/ext3/`; `docs/s22-ext-03-symbolized.txt` |
| **ext8** | kernel `1f8c489f…`, default hostport/host_info traces; system processes | hostport **35**（id 200×6 / 205×23 / 206×6）；host_info **15**，全部 ret=0 | `logs/s22-ext/ext8/` |
| **ext9** | standard recipe; UI 660 s; timeouts off | GCWidgets[48]/BatteriesWidget[49] spawn, 70 INIT lines, `running`; **no `uncorking exec source upfront`**; hanging 0; CheckIn 0; hostport/host_info 0 | `logs/s22-ext/ext9/` |
| **ext11** | probe2 before exec; kernel `1f8c489f…` | BASIC `kr=0 ncpu=1`; LOAD `kr=0`; `host_statistics64 kr=0`; crash at **empty-header `mach_msg`** (no reply) | `logs/s22-ext/ext11/` |
| **ext14b** | kernel **`7a4dff5340ad26786e9dc70f26e69016`** with `hakios: exec pid=%d path=%s` | exec pid=45…53: Books/People/Weather/mobilenotes + AppStore/Passbook/FindMy/Sleep; all **no exit/crash**; CheckIn 0; host_info 0 | `logs/s22-ext/ext14b/` |
| **ext24** | kernel `203c3df8…`; `launchd_disable_all_timeouts=1`; ~15 min | **`checked in as alive` 4 次**：Batteries spawn 06:56:01 → checkin **06:56:39（38 s）**；HomeWidget **+8 min**；**GradientPosterExtension +10 min**；Batteries 重复；hanging 0；widget 无 exit | `logs/s22-ext/ext24/`；`/tmp/ext24-last.png` |
| **ext26** | 真长跑 2400 s（UI_GRACE=2400） | `checked in as alive` **34 次**（首条 **+15/16 min**；+34 min 到 33，最终 34）；`hanging 0`；结束时槽**仍无内容**，右下绿块 bbox 564,1220–1124,2432 | `logs/s22-ext/ext26/`；`/tmp/ext26-last.png` |

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

## 1.2b ext24: first CheckIn — slow, not dead

- **BatteriesWidget**: spawn **06:56:01** → `extension checked in as alive`
  **06:56:39** = **38 s**.
- **HomeWidget**: +8 min; **GradientPosterExtension**: +10 min (壁纸扩展)；
  Batteries 另有一条重复；合计 **4 次** `checked in as alive`。
- `Extension is hanging` **0**；widget **无 exit**。
- 截图 `/tmp/ext24-last.png`：桌面有 Weather/Calendar 两个白槽（check-in 了，
  内容还没出）。
- **结论**：扩展不是卡死在 dyld；是 **dyld/启动太慢，默认看门狗在 CheckIn 前
  把它们杀了**。关掉 timeout 后能完成 CheckIn。
- **壁纸扩展 check-in ≠ 壁纸已显示**；P2 仍需单独验证。

## 1.2c ext26: 34 CheckIns, still no widget content

- 长跑 2400 s：`checked in as alive` **34 次**；首条在 **+15/16 min**；
  +34 min 到 **33**，最终 **34**；`Extension is hanging` **0**。
- 分布不是前 10 分钟一批：08:58–09:03 只 3 个，09:09 起加速，09:14–09:17 一波 20+。
- **CheckIn 已基本达成，但槽仍无内容**：结束截图是模糊桌面 + 右下绿块
  （bbox x 564–1124, y 1220–2432），不是 widget 内容；绿块不是 widget 槽位置。
- 因此下一步不是再证明「能否 check in」，而是 **check-in 之后的
  Chrono→timeline→WidgetRenderer 链**（chronod persona type 6 等）。

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

1. **扩展不是「卡死在 dyld」，是「慢」**：ext24 在关掉 timeout 后看到 4 次
   `checked in as alive`，Batteries 38 s，HomeWidget +8 min，
   GradientPosterExtension +10 min。
2. **默认看门狗是门**：默认 5–13 s 内没 CheckIn 就被杀；关掉 timeout 后慢启动
   能走完。之前「dyld 错误路径 = 缺库」的推断**降级为慢启动**。
3. **缓存检查 0 missing**，且没有 stat/mmap 卡死证据；不再把缺依赖/卡死当结论。
4. **host_info 仍健康**（ext11/8）；不是 host_info 阻塞。
5. **CheckIn 了不等于 widget 有内容**：ext24 两白槽；ext26 长跑 34 次 CheckIn
   后槽仍无内容，右下绿块也不是 widget 槽。下一步是 check-in 后的
   Chrono/timeline/WidgetRenderer 链，不是再看门禁。

## 3. Unresolved questions (handoff)

1. **Elide path**: why does the widget job take `service will attempt to elide xpcproxy`
   while daemons exec?  Can the widget be forced to exec, or does CheckIn have to
   be fixed under the elide path?
2. **CheckIn prerequisites after exec**: bootstrap namespace, MachServices
   endpoint, signature/entitlement/sandbox, persona; none reached yet.
3. **reg_app build omission**: independent fix (compile + copy + sign); does not
   explain extensions, but should be fixed to remove noise.

## 4. Next experiments

### 4.1 ext26 done; next = post-CheckIn content chain

- ext26 已跑 2400 s：34 次 CheckIn、hanging 0，但槽仍无内容。
- 下一步不再做「check-in census」，改为查 check-in 之后的
  Chrono→timeline→WidgetRenderer 链（含 chronod persona type 6）。
- 判据：槽里出现真实 widget 内容（不是白槽/绿块），且 CheckIn 不回退。

### 4.2 ext12/ext13 misses and ext15 true-name sampling

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

### 4.3 Exec-boundary instrumentation (next boot)

- `bsd/kern/kern_exec.c` at `exec_activate_image`/posix_spawn:
  `hakios: exec pid=%d comm=%s path=%s ret=%d`;
- `bsd/kern/kern_exit.c` `proc_exit`:
  `hakios: exit pid=%d comm=%s status=%d`;
- optional `host_self_trap` trace (port/type) only to prove task context.
- Keep default-on hostport/host_info traces; do not rely on boot-arg at the end
  (`cmdline: 262 of 607` truncation).

### 4.4 Static checks (no boot)

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

- **不再把「dyld 缺依赖」当结论**：缓存检查 0 missing，ext24 证明是慢启动；
- **不再用 stat/mmap 卡死解释**；
- **不再用「elide = 不 exec」解释**：ext14b 已证 exec 发生；
- **不再拿 host_info 当瓶颈**（ext11 全部 kr=0）；
- 不再用 ext3 单发栈作结论（瞬时状态）；
- 不再猜 boot-arg（追踪默认开）；
- 不碰宿主 GPU；本阶段不启动 virtio-gpu（stage 23 另行立项）；
- 不写 P2/P3 已修：CheckIn 从未发生。

## 8. Tool/method: `ustacks` name vs pid

`ustacks` matches names with:

```c
strncmp(proc_name_address(proc), ustacks_name, sizeof(ustacks_name))
```

`ustacks_name` is 32 bytes but `proc_name_address()` is the **16-byte `p_comm`**.
A name longer than **15 characters** therefore cannot match:
`BooksWidgetExtension` (21) is stored as `BooksWidgetExte` (15).  The QEMU input
path `touch.py stacks NAME` is also uppercase-hostile (the guest virtio keyboard
does not process shift), so a mixed-case `p_comm` is effectively untypable that
way.  Boot-arg names are not keyboard-typed, but the 15-character limit still
applies.

The newer kernel (`06bf09c…`) adds a numeric mode in `ustacks_fire()`: if the
target is all digits, it matches `proc_pid(proc)`.  Use the pid from
`hakios: exec pid=N path=…` or launchd's `service spawned with pid N` and
`touch.py stacks <pid>`.

Consequence for ext3: `PodcastsWidget` was a valid sample because it is
**14 characters** (fits `p_comm`) and the boot argument could carry its uppercase
letters; the stack is valid, but this is **not a general extension-sampling
method**.  Do not infer that arbitrary appex names can be sampled by name.

## 8. Next performance work

- 后续性能工作见 **stage 23**：`docs/virtio-gpu-stage23-plan.md`——**virtio-gpu（传输墙）+ TCG 优化（翻译墙）**。
- 产品路径当前 2–3.3 fps；4.6–5.0/10.91 是 `NO_PANEL=1` 实验模式，不能当产品性能。
