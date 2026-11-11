# S22 host port: globally healthy, extension context still unobserved

Status: read-only source/log analysis; no boot.

## 1. What ext8 actually proves

ext8 (kernel md5 `1f8c489…`) logged:

- **35** `hakios: hostport` lines: id **200 ×6**, 205 ×23, 206 ×6;
- **15** `hakios: host_info` lines, all **ret=0**: flavor=1/count=12 and
  flavor=5/count=8.

Those lines are at **117–166 s**.  The first widget in the same log
(`com.apple.gamecenter.widgets.extension`) spawns at **09:43:59**, boot started
**09:33:37** → about **622 s**; the run ends at **176 s**.  Therefore ext8 proves
**the kernel host port/MIG server works for the system processes observed**, but it
does **not** observe an extension call.  ext4 (the long run) is the one with the
extension and it has `hakios: host_info` **0**.

So the current evidence is:

- kernel `host_info()`/MIG is healthy globally (ext8);
- the extension path still fails before the kernel handler (ext4 trace 0; ext3
  stack in `pthread_init` → `host_info` → `mach_msg2`).

## 2. Which flavor, and where the port comes from

`libsystem_pthread` `___pthread_init` (disassembled) calls `host_info` at `+0xa0`:

```
x0 = <result of the preceding mach_host_self-ish call>
w1 = 5                  # HOST_PRIORITY_INFO
x2 = &info[8]
x3 = &count (8)
bl  _host_info
```

So the expected extension call is **flavor 5, count 8**; ext8's flavor=5 lines
are the same shape but come from system processes, not the extension.

`mach_host_self()` is a Mach trap, not a bootstrap lookup:

```c
host_self_trap():
    self = current_task();
    itk_lock(self);
    sright = host_port_copy_send(self->itk_host);
    itk_unlock(self);
    name = ipc_port_copyout_send(sright, current_space());
```

`host_port_copy_send()` accepts only `IKOT_HOST` / `IKOT_HOST_PRIV`; any other valid
port **panics** (`ipc_host.c:362`), it does not silently proxy.  A `TASK_NULL`/
`IP_NULL` `itk_host` yields `MACH_PORT_NULL`.

`itk_host` is set at task creation (`ipc_tt.c:219` root task: real host port;
`:255` child: inherited) and can be replaced by **`task_set_special_port(
TASK_HOST_PORT, ...)`** during `posix_spawn` (`ipc_tt.c:2210`).  **launchd/xpcproxy
can therefore change the extension task's host port**, but only to a real host port
(anything else panics), not to a user-space proxy.  If they set it to null/wrong, the
next `mach_msg2` fails/returns an error rather than reaching `host_info`.

## 3. Static checks (no message needed)

1. **Call site**: `libsystem_pthread` `___pthread_init+0xa0` → flavor **5**,
   count 8, host from `mach_host_self`; confirms the expected request shape.
2. **Port source**: `host_self_trap()` reads `task->itk_host`; `host_port_copy_send`
   only accepts IKOT_HOST/PRIV.  A user-space proxy is impossible without a panic.
3. **Task setup**: `task_set_special_port(TASK_HOST_PORT)` is the only override path;
   inspect launchd/xpcproxy posix_spawn port actions for the extension job.
4. **Binary/job static diff**:
   - `codesign -d --entitlements :- <extension binary>` vs an init helper;
   - the extension job plist (`POSIXSpawnType`, `MachServices`, sandbox profile,
     `EnvironmentVariables`);
   - `xpcproxy` environment / bootstrap namespace (`user/501` vs `system`);
   - extension sandbox profile.

None of these requires sending a message.

## 4. Differences between init/daemons and extensions

| factor | init/daemon | extension |
|---|---|---|
| bootstrap namespace | system/user launchd | user/501 xpcproxy, extension service endpoint |
| task host port | inherited real IKOT_HOST | may be set by xpcproxy/launchd posix_spawn; if null, `host_self` null |
| sandbox/entitlements | daemon profile | extension sandbox; persona type 6 is missing globally (`kpersona_find_by_type` errors) |
| IPC space/reply port | normal | xpcproxy/extension service may create a restricted reply path |
| lifecycle | init helper starts early | extension spawns at ~620 s and has its own watchdog |

The ext3 stack (`_host_info` → `_mach_msg2_trap`) means the caller is in the MIG
send/receive; the kernel host port is healthy, so the remaining difference is the
**extension task's port/reply context**, not `host_info()`.

## 5. What the next boot should log

Add (or keep behind boot args) these exact points:

1. **`host_self_trap()`** in `osfmk/kern/ipc_host.c`:
   ```
   hakios: host_self pid=%d comm=%s itk=%p kotype=%d name=%u
   ```
   immediately after `ipc_port_copyout_send`; expect the extension pid and an
   `IKOT_HOST` port.  A `name=0` / `itk=0` is decisive.
2. **`task_set_special_port(TASK_HOST_PORT)`** in `osfmk/kern/ipc_tt.c`:
   ```
   hakios: task_host_port pid=%d comm=%s old=%p new=%p
   ```
   to see whether launchd/xpcproxy replaces the extension's host port.
3. **`host_info` wrapper** (`osfmk/kern/host.c`): include pid/comm in the existing
   `hakios: host_info enter/ret` line; if the extension never appears, it never
   reached the handler.
4. **`ipc_kobject_server_internal()`** (`ipc_kobject.c`): include sender pid/comm
   in `hakios: hostport msg id=%d`; id=200 with extension pid is the positive
   control.
5. **`mach_msg2` send/reply** path: log the destination port name/right and send
   return; a `MACH_SEND_INVALID_DEST`/reply-port error explains the blocked loop.

Controls: the prebuilt `hostinfo_probe` (`/host/.../userland/obj/hostinfo_probe`)
and ext8's system-process traces.  Probe OK + extension trace absent → extension
context; probe BLOCKED → port/reply global for that context.

**Do not** change timeout/job/watchdog or boot args for this; the next evidence is
port identity/return, not another timeout.
