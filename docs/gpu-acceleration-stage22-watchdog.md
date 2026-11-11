# S22 ext24: check-in works without timeouts; the problem is slow vs watchdog

## 1. Timeout/watchdog keys in launchd

From the launchd binary strings and the in-tree limits file:

| key / message | role | default / current |
|---|---|---|
| `ExtensionWatchdog` | enables the per-extension check-in watchdog | `true` (limits plist) |
| `ExtensionWatchdogTimeout` | max time from extension spawn to check-in; on expiry: `Extension is hanging on launch. Killing.` / `last exit code = (extension watchdogged)` | embedded default is small (observed 5–13 s; the in-tree limits file sets **120**) |
| `_LaunchWatchdogTimeOut` / `launch watchdog timeout = %u` | launchd's general launch watchdog; disabled message is `launch watchdog timeout = (globally disabled)` | binary default; overridable in limits |
| `SIGTERMTimeout` | wait after SIGTERM before SIGKILL | `3` (limits plist) |
| `SIGKILLTimeout` | wait after SIGKILL; `exceeded sigkill timeout: %u` | binary default (escalation only) |
| `ExitTimeOut` / `_VoluntaryExitTimeOut` / `VoluntaryExitTimeOut` | voluntary/forced exit wait | binary default; not startup |
| `ShutdownTimeout` / `shutdowntimeout` | shutdown wait | binary default; not startup |
| `CrashOnSIGTERMTimeout` | crash instead of SIGKILL after SIGTERM timeout | `true` (limits plist) |
| per-job `TimeOut` / `TimeOut` | job-specific timeout | job plist |
| `com.apple.xpc.launchd.timeout` | XPC event timeout domain | runtime |

`launchd_disable_all_timeouts=1` is the global switch: launchd logs
`launch watchdog timeout = (globally disabled)` and ext24 shows **0
`Extension is hanging`**, so it suppresses the extension check-in watchdog (and
the other launchd timeouts) rather than only the general launch watchdog.

## 2. Would 38 s survive the default?

- **Default `ExtensionWatchdogTimeout`** (the 5–13 s observed in ext3/ext9) would
  kill BatteriesWidget at 5–13 s, long before its 38 s check-in.
- **The in-tree limits plist already sets 120 s**, which is >38 s.  If a build
  still kills at ~5 s, that image is not reading
  `/private/etc/com.apple.xpc.launchd.limits.plist` (or has the embedded default).
- **`LaunchWatchdogTimeOut`** is a second gate; if its default is below 38 s it
  can kill first even when the extension check-in timeout is 120.  Set it
  explicitly rather than relying on the binary default.
- `SIGTERMTimeout`/`SIGKILLTimeout`/`ExitTimeOut` only matter after a termination
  decision; they are not the startup clock.

## 3. Why only four extensions check in

- Extension jobs are **spawned on demand** by SpringBoard/Chrono/WidgetKit; ext24
  shows four check-ins (Batteries, Home, GradientPoster, ...), not every installed
  widget.  Only requested jobs reach the check-in path.
- The white Weather/Calendar slots show requests without content; those
  extensions are either later in the request queue or still in dyld.
- `Extension is hanging` is 0 and no widget exits, so the un-checked-in ones are
  not being killed in ext24; they are simply not at the check-in point yet.
- The 38 s Batteries time is consistent with QEMU's single/few CPUs plus dyld
  startup cost (path/fixup/TLV work seen in ext18), not with a deadlock.

## 4. Minimal non-global configuration

Keep the watchdogs enabled, but give extensions enough time.  Edit
`userland/com.apple.xpc.launchd.limits.plist` (copied to
`/private/etc/com.apple.xpc.launchd.limits.plist` by `build.sh`):

```xml
<key>ExtensionWatchdog</key>
<true/>
<key>ExtensionWatchdogTimeout</key>
<integer>600</integer>
<key>LaunchWatchdogTimeOut</key>
<integer>600</integer>
<key>SIGTERMTimeout</key>
<integer>3</integer>
<key>CrashOnSIGTERMTimeout</key>
<true/>
```

- `ExtensionWatchdogTimeout=600` covers 38 s with a 15x margin; 120 would also
  cover it, but 600 avoids a second borderline failure when the machine is loaded.
- `LaunchWatchdogTimeOut=600` removes the second gate; if the key is ignored by
  this launchd build, the extension watchdog is still the primary.
- **Do not** set `launchd_disable_all_timeouts=1` in normal runs; this keeps
  SIGTERM/SIGKILL/exit behavior for genuinely hung jobs.
- Verify the file is actually in the image (`/private/etc/...`) and that launchd
  reads it; a 120 s entry that still produces a 5 s kill means the plist was not
  loaded.

## 5. Slow-root-cause hypotheses (testable)

1. **dyld path scan**: ext18 has `forEachResolvedAtPathVar` /
   `PathOverrides::forEachPathVariant`; with 35k cache paths and many rpaths,
   scanning is expensive.  Test `DYLD_PRINT_SEARCHING=1` and count candidate paths
   per image; compare cold vs warm launch.
2. **Per-process shared-cache mapping**: `shared_region_check_np` failure (empty
   region) makes dyld fall back to mapping the cache.  If every extension maps the
   1+ GB cache privately, that is a large, repeatable cost.  Test the
   `sr_start`/`sr_magic` prints from
   `docs/gpu-acceleration-stage22-shared-cache.md`; compare a 38 s check-in with a
   process that has a populated region.
3. **mmap/page-fault cost**: first touch of cache text over APFS; compare first
   launch vs a second launch of the same extension (warm cache) and `ustacks` in
   `mach_vm_fault`/`mmap`.
4. **CPU contention**: ext18 is one thread, but 50 extensions start together; use
   `ustacks=top`/`cpuload` to see whether the delay is scheduling rather than dyld.

Do not compress the timeout to "fix" the slow path; fix the cache mapping/path
scan if the hypothesis holds.
