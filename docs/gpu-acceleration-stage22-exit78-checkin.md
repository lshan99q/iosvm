# S22 ext9: exit 78, CheckIn, and the corpse notification

## 1. exit 78 is xpcproxy's EX_CONFIG, and here it is a missing helper

`xpcproxy` contains the strings `EX_CONFIG`, `Missing program`,
`Could not find and/or execute program specified by service`, and
`initialization failure: ... 0x%llx`.  `exit(78)` is `EX_CONFIG` from
`<sysexits.h>`; xpcproxy uses it when it cannot set up/execute the job.

ext9 shows the exact chain for **`com.iosvm.reg`** (not an extension):

```
(user/501/com.iosvm.reg [12]) Could not find and/or execute program ...:
    2: No such file or directory: /redwood/bin/reg_app
(user/501/com.iosvm.reg [12]) ... access(/redwood/bin/reg_app, X_OK) failed
    with errno 2 ... error 0x6f - Invalid or missing Program/ProgramArguments
(user/501/com.iosvm.reg [12]) initialization failure: ...: 0x6f
(user/501/com.iosvm.reg [12]) xpcproxy exited due to exit(78)
```

`userland/src/reg_app.c` exists, but `userland/build.sh` never compiles/copies
`reg_app`; the image therefore lacks `/redwood/bin/reg_app`.  This is a **build
omission for a separate registration helper**, not the extension environment.
Fix (separate change): build `reg_app.c` into `obj/reg_app`, copy to `root/bin/`,
and sign it with its entitlements.  It is not the CheckIn blocker.

## 2. CheckIn requires exec first; ext9 never gets there

For a launchd extension, `xpcproxy` is expected to exec the extension; the
extension's `NSExtensionMain`/`xpc_main` then does
`bootstrap_check_in("<label>.apple-extension-service")` and sends the **CheckIn**
message.  Only after that does launchd log `extension checked in as alive`.

Requirements, in order:

1. launchd created the implicit endpoint (`...apple-extension-service`) — ext9
   shows `created the implicit endpoint upfront` and `draining messages ...`;
2. **xpcproxy execs the program** and launchd logs `uncorking exec source upfront`;
3. extension's `bootstrap_check_in` finds the endpoint in its bootstrap namespace;
4. signature/entitlements/sandbox permit the listener/check-in;
5. extension sends CheckIn; launchd marks alive.

In ext9, GCWidgets[48] and BatteriesWidget[49] reach `service state: running` /
`INIT`, but there is **no `uncorking exec source upfront`** for either.  They are
still inside the xpcproxy/launchd spawn handshake; CheckIn and `host_info` are never
reached.  So the missing step is **before** CheckIn: xpcproxy did not get to exec.
It is not an entitlement or listener failure yet.

## 3. EXC_CORPSE_NOTIFY error 5, and why pid 24

- `EXC_CORPSE_NOTIFY = 13` (`mach/exception_types.h`); `osfmk/kern/task.c:2319`
  prints the message when `exception_triage_thread(EXC_CORPSE_NOTIFY, ...)` fails
  in `task_deliver_crash_notification()`.
- **Error code 5 is `KERN_FAILURE`** (`mach/kern_return.h`).  In
  `task_deliver_crash_notification()` the first check is:
  `if (crash_info == NULL) return KERN_FAILURE;` — a corpse without crash metadata
  (or a triage that cannot reach an exception/corpse port) prints exactly this.
- **pid 24 is `configd`**, not an extension: ext9 line 770
  `system/com.apple.configd [24]`, and later `pid/24 [configd]`.  configd's corpse
  notification failed because no crash-reporter/crash-info was present; it is a
  bystander, not the extension CheckIn failure.

## 4. Static checks and the next minimal observation

Static, no message:

- `userland/build.sh` omits `reg_app`; fix that separately;
- extension job plists (`ProgramArguments`, `POSIXSpawnType`, `MachServices`,
  sandbox/entitlements) and `xpcproxy` environment;
- `host_self_trap` only returns `task->itk_host` and `host_port_copy_send` only
  accepts IKOT_HOST/PRIV, so a user-space host-port proxy is impossible (it would
  panic); the port can be overridden by `task_set_special_port(TASK_HOST_PORT)`.

Next boot, one instrument at the **exec** boundary:

```c
/* bsd/kern/kern_exec.c, at exec_activate_image / posix_spawn path */
kprintf("hakios: exec pid=%d comm=%s path=%s ret=%d\n",
    current_proc->p_pid, current_proc->p_comm, path, ret);

/* bsd/kern/kern_exit.c, proc_exit */
kprintf("hakios: exit pid=%d comm=%s status=%d\n",
    p->p_pid, p->p_comm, status);
```

Plus, only to prove the context:

```c
/* osfmk/kern/ipc_host.c host_self_trap */
kprintf("hakios: host_self pid=%d comm=%s itk=%p kotype=%d name=%u\n",
    current_proc->p_pid, current_proc->p_comm,
    self->itk_host, ip_kotype(self->itk_host), name);
```

`hostport`/`host_info` tracing is already available and should stay off unless the
exec line shows the extension actually started.  Do not change timeout/job/watchdog.
