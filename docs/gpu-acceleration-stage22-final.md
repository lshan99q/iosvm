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

## 1.2 ext9: the actual current symptom is pre-exec

- `GCWidgets[48]` (10:55:55) and `BatteriesWidget[49]` (10:55:57) spawn;
  70 `INIT`-related lines; `service state: running`.
- **没有 `uncorking exec source upfront`**（两个都没有）→ 仍卡在
  **xpcproxy/launchd spawn 握手**，**exec 根本没发生**。
- 因此 CheckIn 自然为 0；`Extension is hanging` 0（timeouts 已关）。
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

1. **`host_info` is healthy**; the host_info-bottleneck theory is dead.
2. **Current sole main symptom**: extension `spawn → running/INIT` but **no
   `uncorking exec source upfront`** → stuck in xpcproxy/launchd spawn handshake,
   **exec never happens**, CheckIn is 0.
3. **ext3's host_info stack is transient** for that boot, not a universal cause.
4. **exit 78 / corpse error 5 are not the extension blocker**:
   exit 78 = missing `reg_app` helper; corpse pid 24 = configd bystander.
5. **Not timeout/watchdog**: timeouts off in ext2/ext9; no hanging/kill.

## 3. Unresolved questions (handoff)

1. **xpcproxy/launchd spawn handshake**: why does it stop before
   `uncorking exec source upfront` for extension jobs?  Which check returns?
2. **CheckIn prerequisites after exec**: bootstrap namespace, MachServices
   endpoint, signature/entitlement/sandbox, persona; none reached yet.
3. **reg_app build omission**: independent fix (compile + copy + sign); does not
   explain extensions, but should be fixed to remove noise.

## 4. Next experiments

### 4.1 Exec-boundary instrumentation (next boot)

- `bsd/kern/kern_exec.c` at `exec_activate_image`/posix_spawn:
  `hakios: exec pid=%d comm=%s path=%s ret=%d`;
- `bsd/kern/kern_exit.c` `proc_exit`:
  `hakios: exit pid=%d comm=%s status=%d`;
- optional `host_self_trap` trace (port/type) only to prove task context.
- Keep default-on hostport/host_info traces; do not rely on boot-arg at the end
  (`cmdline: 262 of 607` truncation).

### 4.2 Static checks (no boot)

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

- **不再把 `host_info` 当瓶颈**（ext11 已证 kr=0）；
- **不再用 ext3 单发栈作结论**（瞬时状态）；
- 不再调 timeout/job/watchdog；不再猜 boot-arg（追踪默认开）；
- 不碰宿主 GPU；不启动 virtio-gpu；
- 不写 P2/P3 已修：CheckIn 从未发生。
