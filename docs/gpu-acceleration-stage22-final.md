# Stage 22 final: extension CheckIn is blocked on a host_info MIG reply

**Status: closed to the evidence boundary. No more boots in this stage.**
The conclusion is layered: the extension is wedged in libSystem's `pthread_init`;
the `host_info` request it makes **never enters kernel `host_info()`** (trace 0), so
the caller is waiting for a MIG reply that is never sent.

## 1. Three effective experiments

| run | configuration | observation | evidence |
|---|---|---|---|
| **ext2** | `launchd_disable_all_timeouts=1`; no `launchd_log_serial`; run past 1000 s | 5 s `Extension is hanging`/`CHECK_IN_TIMEOUT`/`killing` **0**; six widgets spawn/uncork; `checked in as alive` **0** | `logs/s22-ext/ext2/` |
| **ext3** | timeout disabled + `ustacks=PodcastsWidget` at 680 s, 90 samples | every sample: `_mach_msg2_trap+8` -> `_host_info+228` -> `___pthread_init+164` -> `_libSystem_initializer+132`; pid alive; no CheckIn | `logs/s22-ext/ext3/serial.log`; `docs/s22-ext-03-symbolized.txt` |
| **ext4** | correct-layout kernel `4d9f32…`, `__TEXT=0xfffffff007004000`, `hakios_host_info_trace=1`; UI 840 s, 15 widget INIT/running | `hakios: host_info` **0**; cmdline confirmed trace=1 (269/607); extension spawns/execs but no CheckIn | `logs/s22-ext/ext4/serial.log`, `report.md` |

(ext5 sampled the wrong truncated name and is **not** counted; it confirms only that
the target name must be `PeopleWidget_iO` if retried.)

Timeline: ext2 removes the watchdog gate but CheckIn stays 0; ext3 shows the live
thread in `pthread_init` -> `host_info` -> `mach_msg2`; ext4 shows the same request
never reaches `host_info()` in the kernel.  The common point is the **MIG round
trip**, not timeout and not an extension crash.

## 2. Conclusion, by layer

1. **H2 — confirmed.** The extension is stuck in libSystem initialisation:
   `pthread_init` calls `host_info` and waits in `mach_msg2`.  It never reaches
   `main`, XPC, or CheckIn.
2. **The request does not reach kernel `host_info()` — trace 0.**  The caller has
   sent a message and is waiting for a reply.  The working hypothesis is:
   **host port / MIG server is not ready or not registered**, or the request is
   dropped before `ipc_kobject_server_internal()`; the reply never arrives.
3. Not timeout, not watchdog, not the extension crashing: ext2 already removed
   timeout and the thread stays alive in the MIG send/receive.

The end state is a **message sent to a server that does not answer**, not a kernel
`host_info` computation.

## 3. Next executable checklist (next stage, not this one)

### 3.1 Kernel tracing points

- `mach_msg2` / MIG dispatch entry: log destination port, `msgh_id=200`, sender;
- `ipc_kobject_server_internal()` (`osfmk/kern/ipc_kobject.c:384`): log
  `msgh_id`, `ipc_kobject` type, whether `find_mig_hash_entry()` returns a handler;
- host port registration: `ipc_host_init()` (`ipc_host.c:122`) `IKOT_HOST` port and
  the generated `mach_host_server.c:199 _Xhost_info` routine table;
- unknown-routine path: `find_mig_hash_entry()` null -> `MIG_BAD_ID`; log it instead
  of silently replying.

### 3.2 Minimal guest probe (separates extension-specific from global)

Write a guest executable that calls `host_info` directly, without extension code:

```c
#include <mach/mach.h>
#include <stdio.h>
int main(void) {
    host_basic_info_data_t bi;
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    mach_port_t host = mach_host_self();
    printf("probe: host=%u count=%u\n", host, count);
    kern_return_t kr = host_info(host, HOST_BASIC_INFO,
        (host_info_t)&bi, &count);
    printf("probe: kr=%d count=%u max_cpus=%d memory=%u\n",
        kr, count, bi.max_cpus, bi.memory_size);
    return 0;
}
```

Build capability: **existing guest toolchain can build executables** (see
`userland/build.sh`: `net_test`, `rwtouch`, `hello_dyn`, `gl_test` are executables;
`$DYN` links against the iPhoneOS16.5 SDK).  No new tool is required:

```sh
$DYN -o obj/hostinfo_probe src/hostinfo_probe.c
cp obj/hostinfo_probe root/bin/
```

Run it as an early launchd job (or from `rc`) and compare with the extension:
- probe blocks at `host_info` too → **host port/MIG server is global-broken**;
- probe returns `KERN_SUCCESS` → **extension-specific** (sandbox/persona/entitlement
  or the extension's early initialiser).

### 3.3 Build capability

- Kernel: cctools Apple `aarch64-apple-darwin11-ld`, **not `ld64.lld`** (LLD 19 does
  not implement `-image_base`/`-segaddr`); `LD_LIBRARY_PATH=<tree>/toolchain/lib`;
  delete `mach.development.bcm2837{,.unstripped}` before relinking; verify
  `__TEXT vmaddr=0xfffffff007004000`, `__start` high, trace strings=2.
- Guest: `$DYN` executable path above; no static-only limitation.

## 4. Not doing

- no more `ExtensionWatchdogTimeout` / job / watchdog changes;
- no more boot-arg guessing;
- no host GPU driver changes;
- no claim that P2/P3 is fixed: CheckIn never happened.

## 5. Infrastructure gained

- Kernel build pipeline is now reproducible: cctools ld64 + `LD_LIBRARY_PATH`,
  clearing stale `BUILD/obj`/linked kernel, high-vmaddr and entry self-check.
- `symstack.py` is in `tools/glwire/harness/` with the `.symbols` cache and
  containing-symbol fix; complete ext3 stack map is in
  `docs/s22-ext-03-symbolized.txt`.
- `gate.sh` and `gate_names.py` are in the harness for one-command PASS/FAIL.
- Kernel `host_info` trace patch and flavor/count audit:
  `tools/glwire/patches/host-info-fix.patch`,
  `docs/gpu-acceleration-stage22-host-info-flavors.md`.

**Next stage starts with the guest `host_info` probe and the three kernel trace
points above; no further extension boots until then.**
