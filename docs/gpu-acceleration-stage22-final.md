# Stage 22 final: extensions never reach exec/CheckIn; host_info is healthy

**Status: closed to the evidence boundary.**
ext11/probe2 proves `host_info` is **completely normal**; ext9 shows the real current
symptom: extensions go `spawn → running/INIT` but **never reach
`uncorking exec source upfront`**, so exec/CheckIn never happens.

## 1. Effective experiments

| run | configuration | observation | evidence |
|---|---|---|---|
| **ext2** | `launchd_disable_all_timeouts=1`; no serial; >1000 s | 5 s hanging/CHECK_IN_TIMEOUT/killing **0**; widgets spawn; `checked in as alive` **0** | `logs/s22-ext/ext2/` |
| **ext3** | timeout disabled + `ustacks=PodcastsWidget` 90 samples | stack in `pthread_init → host_info → mach_msg2`; pid alive; no CheckIn | `logs/s22-ext/ext3/`; `docs/s22-ext-03-symbolized.txt` |
| **ext8** | kernel `1f8c489f…`, default hostport/host_info traces; system processes | hostport **35**（id 200×6 / 205×23 / 206×6）；host_info **15**，全部 ret=0 | `logs/s22-ext/ext8/` |
| **ext9** | standard recipe; UI 660 s; timeouts off | GCWidgets[48]/BatteriesWidget[49] spawn, 70 INIT lines, `running`; **no `uncorking exec source upfront`**; hanging 0; CheckIn 0; hostport/host_info 0 | `logs/s22-ext/ext9/` |
| **ext11** | probe2 before exec; kernel `1f8c489f…` | BASIC `kr=0 ncpu=1`; LOAD `kr=0`; `host_statistics64 kr=0`; crash at **empty-header `mach_msg`** (no reply) | `logs/s22-ext/ext11/` |

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

## 1.2 ext9: widget takes the elide path and never execs

Same boot, two lifecycles side by side (`docs/s22-ext-11-exec-diff.md`):

| step | daemon `notifyd` (pid 29) | widget `BatteriesWidget` (pid 49) |
|---|---|---|
| xpcproxy | **`xpcproxy spawned with pid 29`** | **无**（没有 xpcproxy 子进程） |
| elide | 无 | **`service will attempt to elide xpcproxy` ×2** |
| messages | 无 | `draining messages` ×2（含 `...apple-extension-service`） |
| exec | **`uncorking exec source upfront`** | **无 `uncorking exec`** |
| attach | `SOURCE_ATTACH` | `SPAWNED` → `running` → `INIT`，然后停 |

- **widget 全 log 的 `uncorking exec` = 0**；daemon 的 uncorking 与
  `xpcproxy spawned` 一一对应。
- **结论：widget 扩展走 launchd 的 elide-xpcproxy 路径，它从未 exec**；
  CheckIn 为 0 是这条路径的下游现象。
- 内核 `hostport`/`host_info` 全程 0，与「没到 exec/extension 代码」一致。

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

1. **根因锁定在 elide 路径：exec 从未发生。** widget 走
   `service will attempt to elide xpcproxy`、无 xpcproxy 子进程、无
   `uncorking exec`；CheckIn=0 是下游现象。
2. **历史解释全部排除**：
   - `host_info`（ext11/probe2：BASIC/LOAD/host_statistics64 全 kr=0）；
   - `timeout`（ext2：全部关掉后仍 CheckIn 0）；
   - `job 类型`（早前四类 job 对照无效）。
3. **ext3 的 host_info 栈是那个 boot 的瞬时状态**，不能当普遍根因。
4. **exit 78 / corpse error 5 不是扩展阻塞**：exit 78 = `reg_app` 缺失
   （EX_CONFIG，独立小修）；pid 24 = configd 旁证。
5. **下一步**：ext12 采那个卡住 pid 的 ustacks，判它是
   「等 launchd 的 uncork 消息」还是「exec 半路失败」。

## 3. Unresolved questions (handoff)

1. **Elide path**: why does the widget job take `service will attempt to elide xpcproxy`
   while daemons exec?  Can the widget be forced to exec, or does CheckIn have to
   be fixed under the elide path?
2. **CheckIn prerequisites after exec**: bootstrap namespace, MachServices
   endpoint, signature/entitlement/sandbox, persona; none reached yet.
3. **reg_app build omission**: independent fix (compile + copy + sign); does not
   explain extensions, but should be fixed to remove noise.

## 4. Next experiments

### 4.1 ext12: sampling missed; ext13 targets

- ext12：`ustacks=GCWidgets` **0 命中**（303 条 `no process called GCWidgets`）；
  真实 widget 是 **`TVWidgetExtension [47]`、`RemindersWidgetExtension [48]`**。
- code 判断：elided 子进程未 exec 时 **`p_comm=launchd`**，不是 widget 名。
- ext13 采样对象：
  1. `launchd`（elided child 的 p_comm）；
  2. `TVWidgetExtension`；
  3. `RemindersWidgetExtension`；
  优先按 **pid**（47/48）采，避免再被 p_comm 名字坑。
- **方法教训**：采样前先确认实际进程名；日志里 `service spawned with pid` 旁边的
  名字才是真的；`ustacks` 按 `p_comm` 匹配，用错名字就是 0 命中。
- 判据仍是：栈在等 launchd 的 **uncork 消息**，还是在 **exec 半路**。

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
- **不再用 ext3 单发栈作结论**（瞬时状态）；
- 不再猜 boot-arg（追踪默认开）；
- 不碰宿主 GPU；不启动 virtio-gpu；
- 不写 P2/P3 已修：CheckIn 从未发生。
