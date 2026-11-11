# S22 elide xpcproxy: what pid N is, and where uncork should happen

## 1. What "elide xpcproxy" means

launchd has two spawn paths for a service:

- **xpcproxy path**: launchd forks a child, the child execs
  `/usr/libexec/xpcproxy`; launchd logs `xpcproxy spawned with pid N`, then
  `SOURCE_ATTACH`, then `uncorking exec source upfront` and `created`; xpcproxy
  sets up the bootstrap/role/sandbox and execs the real program.
- **elide path** (`launchd_proxyless_spawn` enabled, the default for many
  extension jobs): **no xpcproxy process**.  launchd forks a child that is still
  **`launchd(proxyless)`** (the strings are in launchd:
  `launchd(proxyless)`, `_Proxyless`, `Cannot setup an event for proxyless spawns!`).
  The logged `service spawned with pid N` is this **launchd child before it execs
  the target**, not the target program and not xpcproxy.

So for GCWidgets[48]/Batteries[49], pid 48/49 is a **forked launchd(proxyless)
child**, waiting for launchd to release its **exec source** (uncork).  It is not
"the extension running but not checking in"; the target exec has not happened.

## 2. What `uncorking exec source` waits for

launchd's strings show the state machine:

```
SOURCE_ATTACH                         # child attaches to the exec source
deferring exec source uncork
uncorking exec source after service init
uncorking exec source upfront
uncorking exec source for disposal
domain is not initialized, can't uncork
domain shutting down, not uncorking
blocking xpcproxy until controller configures the service
Cannot setup an event for proxyless spawns!
```

The successful daemon sequence is:

```
xpcproxy spawned -> SOURCE_ATTACH -> running/INIT -> Successfully spawned
-> uncorking exec source upfront -> created
```

The elided widget sequence is:

```
service will attempt to elide xpcproxy
-> ... -> SOURCE_ATTACH -> running/INIT -> Successfully spawned
-> (no uncorking)
```

`SOURCE_ATTACH` alone is not enough; the child/exec source must be **configured**
(the `blocking xpcproxy until controller configures the service` state) and the
domain must be initialized.  For proxyless spawns launchd itself says
`Cannot setup an event for proxyless spawns!` — if that setup fails/defers, the
exec source is never uncorked, and the child remains launchd(proxyless).  That is
the best-supported reading of the missing line; the current logs do not show a
panic or exit, only the absent uncork.

`pid N` in the elided case is therefore **launchd's forked child**, not the
extension.  Sampling it (`ustacks=launchd`, `ustacks_tname=*`) shows whether it is
in `posix_spawn`/sandbox setup, `mach_msg` for the controller, or already past exec.

## 3. Forcing no elision

Known switch:

```
launchd_proxyless_spawn=0
```

launchd's message for it is `service will always use xpcproxy, elision was
disabled by boot-arg`.  Historical C2 **did** produce `xpcproxy spawned` for
extensions, but CheckIn was still 0; so forcing xpcproxy is necessary for the
GC/Batteries-style no-exec cases but was **not sufficient** for CheckIn.  That is
why this is a diagnostic, not a fix.

launchd also has the private plist/strings `_Proxyless`, `__Proxyless`,
`_ExperimentalNonLaunching`, `_OmitSandboxParameters`, `_SandboxProfile`,
`SandboxFlags`, and `SandboxProfile`.  Of these only `_Proxyless` looks like a
per-job elision control; it is private and not documented, so the safe test is the
boot-arg first.  There is no other public boot-arg for elision in the binary's
strings.

For launchd/xpcproxy-side detail use:

```
launchd_log_serial=1
```

xpcproxy is an Apple binary; its internal controller/checkin messages are not
source-hookable, and only launchd's debug log (to serial) can show them.  Use a
dedicated serial file because this flag can compete with the normal console.

## 4. Sampling the elided child

- `ustacks=launchd`, `ustacks_at=<spawn+2s>`, `ustacks_ms=1000`,
  `ustacks_count=60`, `ustacks_tname=*` samples every launchd/launchd(proxyless)
  thread.  The child of the widget is one of them; its stack names the missing
  step (`posix_spawn`, `sandbox_apply`, `mach_msg`, or a jump to the target).
- `pid N` itself is not addressable by name; the child has `p_comm=launchd`, so
  sample by process name, not by pid.
- If the stack is in `posix_spawn`/`sandbox_apply`, the elide path is failing at
  setup; if it is in `mach_msg`, it is waiting for the controller/launchd event.

## 5. ext13 minimal observation

One boot, no image change:

```
launchd_proxyless_spawn=0
launchd_log_serial=1        # dedicated serial file, not the normal console
ustacks=launchd ustacks_at=<first widget spawn + 2s> ustacks_ms=1000
ustacks_count=60 ustacks_threads=8 ustacks_tname=*
```

Judge:

- `xpcproxy spawned with pid` appears for GC/Batteries → elision bypassed;
- `uncorking exec source upfront` + `created` appear → the child reached exec;
- if `checked in as alive` still 0 after that, the remaining gap is the extension's
  `NSExtensionMain`/`bootstrap_check_in`/`xpc_main`, not launchd spawn;
- if the launchd stack is in `posix_spawn`/`sandbox_apply`, add the kernel exec and
  sandbox traces below.

Kernel traces, if needed, specifically:

```c
/* bsd/kern/kern_exec.c: exec_activate_image / posix_spawn completion */
kprintf("hakios: exec pid=%d comm=%s path=%s ret=%d\n",
    p->p_pid, p->p_comm, path, ret);

/* bsd/kern/kern_exit.c: proc_exit */
kprintf("hakios: exit pid=%d comm=%s status=%d\n",
    p->p_pid, p->p_comm, status);
```

No timeout/job/watchdog changes; no host GPU changes.
