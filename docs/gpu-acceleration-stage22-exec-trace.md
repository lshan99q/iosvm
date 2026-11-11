# S22 exec trace: judgment framework and the posix_spawn blind spot

## 1. What the current kernel trace covers

The new kernel prints:

```c
/* bsd/kern/kern_exec.c:5132, inside __mac_execve(), before exec_activate_image() */
printf("hakios: exec pid=%d path=%s\n", proc_getpid(p), cn_pnbuf);
```

`__mac_execve()` begins at line 5042 and calls `exec_activate_image()` at 5141.
**`posix_spawn()` begins at line 3496 and calls `exec_activate_image()` at 4176
with no such print.**

launchd normally uses **`posix_spawn()`**, not `execve()`, for services.  So:

- a widget path appearing in `hakios: exec` means an `execve`-style call;
- a widget path **not** appearing is **not proof** that launchd did not exec it —
  the `posix_spawn` call is blind in this trace.

Before concluding "widget never execs", check whether the kernel's trace was also
added at `posix_spawn`/4176.  In the tree we inspected, it was not.

## 2. Judgment framework for ext14

1. Match a `hakios: exec` line to the widget by **pid from
   `service spawned with pid N`** and by **path** (e.g. `TVWidgetExtension`,
   `RemindersWidgetExtension`), not by the log short name (`GCWidgets`).
2. The current trace prints **before** `exec_activate_image()`; it does **not**
   print the return.  A line means "exec entry attempted", not "exec succeeded".
3. Cross-check the launchd lines:
   - `uncorking exec source upfront`
   - `created`
   - process `[TargetName]`
4. Interpret:

| kernel trace | launchd | meaning |
|---|---|---|
| `exec pid=N path=...` | `uncorking` + `created` | exec entry; likely real exec |
| `exec pid=N` | no `uncorking` | exec entry but not released/failed; not proof of running |
| no `exec` for N | any | cannot conclude — `posix_spawn` is blind |
| path for a different pid | — | not the widget; check pid/path |

## 3. If the widget truly does not exec

The next observation must be at the **`posix_spawn`** call, not the `execve` one:

```c
/* bsd/kern/kern_exec.c, posix_spawn(), immediately before line 4176 */
printf("hakios: pspawn pid=%d path=%s\n",
    proc_getpid(p), imgp->ip_ndp ? imgp->ip_ndp->ni_cnd.cn_pnbuf : "?");

error = exec_activate_image(imgp);

/* immediately after */
printf("hakios: pspawn ret pid=%d err=%d\n", proc_getpid(p), error);
```

Also useful around the same path:

- `mac_execve_enter()` / `exec_check_permissions()` / sandbox policy entry;
- `posix_spawn` return: if `pspawn` appears but no `ret`, the child is stuck inside
  activation; if `ret != 0`, log the error (sandbox/AMFI/code-sign/VM);
- launchd's own `Cannot setup an event for proxyless spawns!` with
  `launchd_log_serial=1`.

`pspawn pid=N path=...` with no `ret` is the decisive "stuck before/inside
exec_activate_image" line.  No `pspawn` at all means launchd never reached the
kernel spawn for that job.

## 4. If the widget does exec

If `pspawn ret pid=N err=0` (or `exec pid=N` plus
`uncorking exec source upfront`/`created`), the remaining gap is after exec:

- sample the target's `p_comm` (`TVWidgetExtension`, `RemindersWidgetExtension`;
  truncated to 15 chars) with `ustacks=<p_comm>` at exec+1..2 s;
- expected frames: `NSExtensionMain`, `_EXExtensionMain`,
  `bootstrap_check_in`, `xpc_main`, `xpc_connection_*`, `mach_msg2_trap`;
- launchd endpoint: `...apple-extension-service`;
- if the stack is in `bootstrap_check_in`, the missing step is the listener/name
  in the extension's bootstrap namespace; if in `NSExtensionMain`/dyld, it is
  extension initialisation.

Kernel observation for this branch, if needed:

```c
/* bsd/kern/bootstrap.c: bootstrap_check_in / bootstrap_register */
kprintf("hakios: bootstrap_checkin pid=%d name=%s\n", p->p_pid, name);
```

## 5. Do not over-read ext14

The only safe conclusions from the current `__mac_execve`-only trace:

- a widget **does** exec if its pid/path appears before `uncorking exec`;
- a widget **may still exec via posix_spawn** even when no line appears.

The next kernel trace to add is therefore the `posix_spawn` before/after pair
above; without it, "no exec" is a false negative for launchd services.
