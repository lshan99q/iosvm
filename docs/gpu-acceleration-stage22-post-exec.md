# S22 after exec: the steps to CheckIn and the ext15 branch table

## 1. What ext14b established

The new kernel printed real exec lines for widget pids 45–53, e.g.:

```
exec pid=45 /Applications/Books.app/PlugIns/BooksWidgetExtension.appex/BooksWidgetExtension
exec pid=47 .../PeopleWidget_iOSExtension
exec pid=48 .../WeatherWidget
exec pid=49 .../com.apple.mobilenotes.WidgetExtension
exec pid=50..53 AppStore/Passbook/FindMy/Sleep
```

Those pids have **no exit/crash** record, yet `checked in as alive` is 0 and
`hakios: host_info` is **0**.  The old "elide never execs" conclusion is invalid:
elide only skips xpcproxy; the appex is still exec'd.  The failure is **after exec
and before CheckIn**, and it is **before host_info**.

## 2. Required steps, exec -> CheckIn

| # | step | what must happen | observable |
|---|---|---|---|
| 1 | kernel `exec_activate_image` | Mach-O loaded; task switched | `hakios: exec pid/path`; no exit/crash; pid alive |
| 2 | dyld startup | `_dyld_start` -> `dyld4::prepare` -> `Loader::runInitializers`; closures/cache | stack: `dyld4::prepare`, `Loader::runInitializers`, `start`; `DYLD_PRINT_LIBRARIES=1` / `DYLD_PRINT_INITIALIZERS=1` (launchd job env) |
| 3 | libSystem initializer | `libSystem_initializer` -> `libkernel_init`, libdispatch, libsystem_trace, malloc, `pthread_init` | stack: `_libSystem_initializer`, `___libkernel_init`, `_libdispatch_init`, `___pthread_init` |
| 4 | pthread/host_info | `___pthread_init` calls `host_info` (flavor 5, count 8) | `hakios: host_info enter flavor=5 count=8`; host port trace |
| 5 | `NSExtensionMain` / `_EXExtensionMain` | extension config loaded; process/listener prepared | stack: `NSExtensionMain`, `_EXExtensionMain`, `EXExtensionMain` |
| 6 | `xpc_main` / `_xpc_runtime_main` | XPC runtime starts, endpoint lookup/check-in | stack: `xpc_main`, `_xpc_runtime_main`, `bootstrap_check_in`, `xpc_connection_*` |
| 7 | CheckIn message | launchd matches the check-in on `<label>.apple-extension-service` | launchd: `checked in as alive` |

`bootstrap_check_in`/`xpc_main` are **userland libxpc** (Apple binary), not in the
XNU tree; observe them with `ustacks` + `symstack`, not a kernel source print.

## 3. What can block before host_info

`host_info` is called from `___pthread_init`, which is already inside
`libSystem_initializer`.  Earlier calls in the same chain include:

1. **dyld** itself: closure loading, `PrebuiltLoader`, shared-cache mapping;
2. **libkernel_init** and **libSystem_initializer** entry;
3. **libdispatch / libsystem_trace / malloc / os_once** initializers;
4. `___pthread_init` before its `host_info` call.

If `host_info` is 0, the stop is one of 1–4, not CheckIn and not the host port
server (ext11 probe2 proved host_info returns `kr=0` for a plain executable).
The ext3 stack showed the later `pthread_init -> host_info` point; the extension
now stops **earlier**, before even that call.

## 4. ext15 branch table

| stack location | what it proves | next proof | fix direction |
|---|---|---|---|
| `_dyld_start` / `dyld4::prepare` / `Loader::runInitializers` | stuck in image loading/initializers | `DYLD_PRINT_LIBRARIES=1` and `DYLD_PRINT_INITIALIZERS=1` in the widget's launchd `EnvironmentVariables`; stack symbolization | missing/wrong framework, dyld closure/cache, prebuilt loader |
| `_libSystem_initializer` / `___libkernel_init` / `_libdispatch_init` / `os_log` | libSystem initializer before pthread | symbol at `+offset`; compare a daemon's stack; launchd debug | missing library/symbol or dispatch/os_log init |
| `___pthread_init` / `_host_info` / `_mach_msg2_trap` | the ext3 point, inside pthread | `host_info` trace should appear; `host_self_trap` pid/comm/port; probe | host port / reply path (but probe2 says server healthy) |
| `NSExtensionMain` / `_EXExtensionMain` | libSystem passed; extension config/runtime start | check `NSExtensionPointIdentifier` and appex Info.plist; launchd job env | wrong/missing extension point, bundle/runtime |
| `xpc_main` / `_xpc_runtime_main` / `bootstrap_check_in` | extension reached XPC but not CheckIn | `launchd_log_serial=1`; endpoint `...apple-extension-service`; check `MachServices` | listener/name/bootstrap namespace/entitlement |
| `NSRunLoop` / `UIApplicationMain` after CheckIn | CheckIn already done or skipped | launchd `checked in as alive`; xpc connection state | extension main logic |

## 5. Decision rules

- **dyld/libSystem branches**: fix the image/runtime, not host_info; host_info is
  downstream and trace 0 is expected.
- **pthread/host_info branch**: only then use host_self/host_info traces; the
  plain probe returned OK, so compare the extension's pid/port, not the server.
- **NSExtensionMain branch**: the appex was exec'd but its extension point/config
  is wrong or missing; inspect `Info.plist` and launchd's job.
- **xpc/bootstrap branch**: CheckIn endpoint/namespace; use launchd debug log,
  not another timeout boot.
- **NSRunLoop branch**: CheckIn likely happened; the remaining problem is
  extension content, not this stage.

## 6. ext15 sampling and trace choice

- Sample the **real exec'd pid**: use the pid from `hakios: exec pid=N path=…`
  or launchd's `service spawned with pid N`.  Do **not** derive a name from
  `BooksWidgetExtension`: `p_comm` is 16 bytes, so its stored name is
  `BooksWidgetExte` (15 chars), and `strncmp(..., 32)` cannot match a longer
  string.  Mixed-case names are also awkward through the QEMU keyboard.
- Current kernel (`06bf09c…`) numeric mode: `touch.py stacks <pid>` (or a numeric
  `ustacks` target) matches `proc_pid()` directly.
- Window: exec+1 s, 120 samples, `ustacks_tname=*`; 15-char p_comm aliases are
  `BooksWidgetExte`, `PeopleWidget_iO`, `WeatherWidget`.
- If the stack is not one of the named frames because of truncation, symbolize
  with `tools/glwire/harness/symstack.py`; the full output is richer than the
  short `ustacks` line.
- Do not add another host_info fix; the current evidence says host_info is never
  reached.
