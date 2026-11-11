# S22 launchd/xpcproxy differential: daemons exec, extensions do not CheckIn

## 1. A successful daemon sequence (ext9)

`configd[24]` (ext9, `system/com.apple.configd`):

```
t+0.000  WILL_SPAWN
t+0.000  service state: spawn scheduled / spawning
t+0.001  launching: speculative
t+0.130  xpcproxy spawned with pid 24
t+0.130  internal event: SPAWNED
t+0.130  service state: xpcproxy
t+1.252  internal event: SOURCE_ATTACH
t+29.27  service state: running
t+29.27  internal event: INIT
t+29.27  Successfully spawned configd[24] because speculative
t+41.61  uncorking exec source upfront
t+41.61  created
```

`mDNSResponder[41]` (ext9) is the short version of the same handshake:

```
WILL_SPAWN -> spawn scheduled/spawning -> launching: ipc (socket)
-> xpcproxy spawned with pid 41 -> SPAWNED -> service state: xpcproxy
-> SOURCE_ATTACH -> service state: running -> INIT
-> Successfully spawned mDNSResponder[41] -> uncorking exec source upfront -> created
```

Both use **xpcproxy** and both reach `uncorking exec source upfront` + `created`.

## 2. Extension sequences

### GCWidgets[48] / BatteriesWidget[49] (ext9)

```
service spawned with pid 48/49
internal event: SPAWNED
service state: running
internal event: INIT
Successfully spawned GCWidgets[48] / BatteriesWidgetExtension[49]
internal event: SOURCE_ATTACH
(no xpcproxy spawned with pid)
(no uncorking exec source upfront)
(no created)
```

Batteries additionally logs the elision decision:

```
service will attempt to elide xpcproxy
draining messages from com.apple.Batteries.BatteriesWidget
draining messages from com.apple.Batteries.BatteriesWidget.apple-extension-service
```

### ScreenTimeWidgetExtension[48] (ext11)

```
service will attempt to elide xpcproxy
WILL_SPAWN / spawn scheduled / spawning / launching: launch job demand
service spawned with pid 48
SPAWNED / service state: spawned / running / INIT
Successfully spawned ScreenTimeWidgetExtension[48]
SOURCE_ATTACH
uncorking exec source upfront
created
(no checked in as alive)
```

So **elision is not always fatal for exec**: ScreenTime/AppStore/Music do reach
`uncorking exec`/`created`; GC/Batteries do not.  But **no extension in ext9/ext11
reaches `checked in as alive`**.

## 3. Top three differences

1. **Daemons use xpcproxy; extensions take the elision path.**
   Every successful daemon logs `xpcproxy spawned with pid`; the widget jobs log
   `service will attempt to elide xpcproxy` and never spawn xpcproxy.  This is the
   structural handshake difference.  (C2 forced `launchd_proxyless_spawn=0` in an
   older boot; it did not fix CheckIn then, but that was before the current
   timeout-disabled/extension-focused runs.)
2. **Per-bundle exec is inconsistent: GC/Batteries never log `uncorking exec` or
   `created`; ScreenTime/AppStore/Music do.**
   The former are stuck before exec; the latter execute but still never CheckIn.
   This rules out a single universal "launchd timeout" and points at the elision /
   direct-spawn path plus the bundle's `Program`/bootstrap setup.
3. **After exec, the extension never CheckIn.**
   ScreenTime has `uncorking exec source upfront` + `created`, yet no
   `checked in as alive`, no `host_info`/`hostport` trace.  The missing handshake is
   therefore **inside the extension after exec** (`NSExtensionMain` /
   `bootstrap_check_in` / `xpc_main`), not launchd's listener (the
   `...apple-extension-service` endpoint is created/drained before spawn).

`SOURCE_ATTACH` is present in **both** success and failure paths, so it is not the
missing line.  `exit(78)` is separate: `com.iosvm.reg` cannot find
`/redwood/bin/reg_app` (a build omission), not an extension.

## 4. launchd or xpcproxy?

- **GC/Batteries (ext9)**: xpcproxy is **not spawned at all** (elision path); the
  stop is in **launchd's elided/direct spawn path**, before `uncorking exec`.
- **ScreenTime (ext11)**: launchd also elides xpcproxy, but reaches `uncorking
  exec`/`created`; the remaining failure is **extension-side after exec**.
- **Successful daemons**: the xpcproxy path reaches exec and run.

Thus the first discriminator is the **elision/direct-spawn path**, and the second is
the extension's post-exec CheckIn.  It is not one stuck process for all widgets.

## 5. Minimal testable change

No image change; first boot-arg:

```
launchd_proxyless_spawn=0
```

This forces xpcproxy for the extension jobs.  Expected differential:

- widget now logs `xpcproxy spawned with pid`;
- GC/Batteries gain `uncorking exec source upfront`/`created` if the failure was
  the elided path;
- if ScreenTime still has no CheckIn after exec, the remaining gap is extension
  `NSExtensionMain`/`bootstrap_check_in`, not launchd spawn.

For launchd/xpcproxy-side logging, add **`launchd_log_serial=1`** (launchd writes
its verbose log to the serial console).  `xpcproxy` itself is an Apple binary; its
internal `XPC_PROXY`/checkin messages only appear through launchd's debug logging,
not through a source hook.  Use a dedicated serial file because this flag can
compete with the normal console capture.

Per-bundle static check before the boot: confirm the widget's `Program` path exists
and `MachServices` contains `<label>.apple-extension-service`; do not change the
plist in the same boot as `launchd_proxyless_spawn=0`.

## 6. Corpse error 5

`EXC_CORPSE_NOTIFY=13`; `task_deliver_crash_notification()` returns
**`KERN_FAILURE` (5)** when `crash_info == NULL` (or triage finds no corpse port).
ext9's pid 24 is **configd**, not an extension:
`system/com.apple.configd [24]`; the extension pids are 48/49.  It is a bystander,
not the CheckIn cause.

## 7. Next observation after forcing xpcproxy

If exec still does not produce CheckIn, instrument the extension side:

- `xpcproxy`/launchd: the forced `xpcproxy spawned` and `uncorking exec` lines;
- `host_self_trap` / `host_info` are **not** needed now (ext11 probe2 proves
  `HOST_BASIC_INFO`/`HOST_LOAD_INFO`/`host_statistics64` all return `kr=0`);
- add a stack sample on the executed extension pid (`ustacks=<p_comm>`) to see
  whether it is in `NSExtensionMain`, `bootstrap_check_in`, or `xpc_main`.

Do not spend another boot on timeouts/jobs/watchdog.
