# Stage 22 final: host port/MIG is healthy; the extension's call context is the blocker

**Status: closed to the evidence boundary.**
ext8 shows the host port MIG path and kernel `host_info` **working normally**; the
remaining blocker is the extension's own call context, not the kernel server.

## 1. Effective experiments

| run | configuration | observation | evidence |
|---|---|---|---|
| **ext2** | `launchd_disable_all_timeouts=1`; no `launchd_log_serial`; run past 1000 s | 5 s `Extension is hanging`/`CHECK_IN_TIMEOUT`/`killing` **0**; six widgets spawn/uncork; `checked in as alive` **0** | `logs/s22-ext/ext2/` |
| **ext3** | timeout disabled + `ustacks=PodcastsWidget` at 680 s, 90 samples | every sample: `_mach_msg2_trap+8` -> `_host_info+228` -> `___pthread_init+164` -> `_libSystem_initializer+132`; pid alive; no CheckIn | `logs/s22-ext/ext3/serial.log`; `docs/s22-ext-03-symbolized.txt` |
| **ext4** | correct-layout kernel `4d9f32…`, `__TEXT=0xfffffff007004000`, `hakios_host_info_trace=1`; UI 840 s, 15 widget INIT/running | `hakios: host_info` **0**; cmdline showed trace=1 (269/607); extension spawns/execs but no CheckIn | `logs/s22-ext/ext4/serial.log`, `report.md` |
| **ext8** | kernel **`1f8c489f2ca93043d53b20aa04739109`**, traces **default on** (`hakios: hostport msg id=%d`, id 200–245; `hakios: host_info enter/ret`); standard recipe | hostport **35 行**（**id 200×6 / 205×23 / 206×6**）；host_info **15 行**（flavor 1、5，**全部 `ret=0`**）；CheckIn 0 | `logs/s22-ext/ext8/` |

(ext5 sampled the wrong truncated name and is **not** counted; it confirms only that
the target name must be `PeopleWidget_iO` if retried.)

## 1.1 ext8: host port health check

- **host port MIG 正常**：35 条 hostport 消息覆盖 id 200–245（200×6、205×23、206×6）。
- **内核 `host_info` 正常**：15 条 `enter/ret`，flavor 1 和 5，**全部 `ret=0`**。
- **结论：`host_info` 本身、host port 注册、MIG 分发都正常**；「extension 卡在
  host_info」不是内核服务端坏。
- 需要查的是 **extension 的调用上下文**：bootstrap 命名空间、端口代理、sandbox、
  entitlement（以及 extension 早期 initializer 是否换了一个坏端口）。
- **ext4 trace 0 不再作为「内核没收到请求」的证据**：ext8 在默认追踪下看到了
  host_info；ext4 的 boot-arg 放在末尾且整条 cmdline 被截断（`cmdline: 262 of 607`），
  不能依赖末尾 boot-arg。

### 临时镜像的坑（不能用于结论）

- `PREBUILT=1 GL=1 GLREMOTE=1 CONFIGD=1` + `EXTRA/RC_JOBS` 的临时镜像**到不了 UI**
  （`ui=0s`、guestframes 3–4）。
- 不能拿它做 ext8 或任何性能/视觉结论；ext8 只用**标准配方**，才有 UI 和完整串口。

## 2. Conclusion, by layer

1. **H2 — confirmed.** The extension is stuck in libSystem initialisation:
   `pthread_init` calls `host_info` and waits in `mach_msg2`; it never reaches
   `main`, XPC, or CheckIn (ext3, 90 samples).
2. **Host port / MIG / kernel `host_info` — healthy.** ext8 sees 35 hostport
   messages (id 200–245) and 15 `host_info enter/ret` lines, all `ret=0`.
   The server is not missing, not unregistered, and not returning errors.
3. **The blocker is extension-specific caller context.** Candidate layers:
   bootstrap namespace (which host port does the extension resolve?),
   port proxy / launchd environment, sandbox, entitlement/persona, or an early
   initializer that replaces/uses a dead port. This is where the next stage starts.
4. Not timeout, not watchdog, not an extension crash: ext2 removed timeout, ext3
   shows the thread alive in the MIG send/receive, ext8 shows the server healthy.

## 3. Next executable checklist (next stage, not this one)

### 3.1 Kernel tracing points (now default-on)

- `hakios: hostport msg id=%d` at MIG/kobject dispatch (`ipc_kobject.c`):
  id 200–245 traffic; `id=200` is the host_info request;
- `hakios: host_info enter/ret` (`host.c`): flavor/count and return;
- if a later boot has no hostport line for `id=200` while the extension is stuck:
  the request never left user space (caller context), not a kernel server issue.

### 3.2 Minimal guest probe

The probe (`guest/hostinfo_probe.c` → `userland/src/hostinfo_probe.c`, in-image
`/redwood/bin/hostinfo_probe`) calls `host_info` from a plain executable, without
extension code. Expected ext8-era result: **`hostinfo: OK ncpu=N mem=M`** (positive
control).

- probe **returns** → host port/MIG is fine; focus on the extension's bootstrap
  namespace / port proxy / sandbox / entitlement;
- probe **blocks** → a global host-port/MIG issue remains (contradicts ext8; recheck
  the probe's own port and the standard recipe);
- probe `KR=…` → map the error code; not a hang.

### 3.3 Build capability

- Kernel: cctools Apple `aarch64-apple-darwin11-ld`, **not `ld64.lld`**
  (LLD does not implement `-image_base`/`-segaddr`); `CCTOOLS_LD64=<path>`;
  delete `mach.development.bcm2837{,.unstripped}` before relinking; verify
  `__TEXT vmaddr=0xfffffff007004000`, `entry_va != 0`, marker strings, and md5.
- `tools/kernel-rebuild.sh [marker...]` automates the above and prints md5.
- Guest: `$DYN` executable path; probe injection via `EXTRA`/`RC_JOBS` in the
  **standard** image, not the no-UI temporary image.

## 4. Not doing

- no more `ExtensionWatchdogTimeout` / job / watchdog changes;
- no more boot-arg guessing; **traces are default-on** because boot args can be
  truncated (`cmdline: 262 of 607`);
- no host GPU driver changes;
- no claim that P2/P3 is fixed: CheckIn never happened;
- do not use the no-UI temporary image for conclusions.

## 5. Infrastructure gained

- Kernel build pipeline: cctools ld64 + delete intermediates + high-vmaddr/entry
  self-check + `qemu/payload.bin` regeneration; `tools/kernel-rebuild.sh`.
- Default-on `hostport` + `host_info` traces in kernel
  `1f8c489f2ca93043d53b20aa04739109`; no boot-arg needed.
- `symstack.py`, `gate.sh`, `gate_names.py` in the harness; ext3 stack map in
  `docs/s22-ext-03-symbolized.txt`.

**Next stage starts with the extension call context (bootstrap namespace / port
proxy / sandbox / entitlement), using the healthy host port as the control.**
