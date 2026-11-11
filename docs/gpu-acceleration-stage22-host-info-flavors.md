# S22 host_info flavors, ext4 branches, and ext5 CheckIn criteria

Status: read-only prediction after the patched kernel linked (12:52, 15,902,536 B;
`strings | grep -c 'hakios: host_info'` = 2).  No boot from this document.

## 1. What pthread_init actually asks

Disassembling the extracted `libsystem_pthread` `___pthread_init` at `0x1d0cd70bc`
shows one host_info call, at `+0xa0`:

```
x0 = host port (from the preceding mach_host_self-ish call)
w1 = 5                  # HOST_PRIORITY_INFO
x2 = &info[8]
x3 = &count (8)
bl  _host_info
```

So the observed `_host_info+228` is **HOST_PRIORITY_INFO**, not `HOST_BASIC_INFO`
and not `HOST_PROCESSOR_SLOTS`.  (The first sample's `_libkernel_init` frame is the
same libSystem initializer walk, before pthread_init.)

The task's "likely BASIC / PROCESSOR_SLOTS" is therefore not what this stack shows.
`HOST_PROCESSOR_SLOTS` is not even defined in this tree's `osfmk/mach/host_info.h`;
`host_info` has no case for it and would return `KERN_INVALID_ARGUMENT` immediately.
`host_processor_slots` is a separate MIG routine (`host_priv.defs`), not host_info.

## 2. host_info branch audit (`osfmk/kern/host.c`)

No branch takes the host lock.  `host_info_inner` is a switch; only
`HOST_VM_PURGABLE` calls into VM (`vm_purgeable_stats`), which can take VM locks.
The MIG server (`_Xhost_info`, generated `mach_host_server.c:199`) and
`ipc_kobject_server_internal` (`ipc_kobject.c:384`) also take no host lock.

| flavor | value | hakios path | count behavior | abnormal path |
|---|---:|---|---|---|
| `HOST_BASIC_INFO` | 1 | implemented | <5 -> `KERN_FAILURE`; >=12 full 12, else old 5 | `memory_size` now capped at 2 GB; `max_mem` 12 GB; zero CPU fields fallback 1 (patched) |
| `HOST_SCHED_INFO` | 3 | implemented | <3 -> `KERN_FAILURE`; sets 3 | none |
| `HOST_RESOURCE_SIZES` | 4 | **always `KERN_INVALID_ARGUMENT`** | checks count, then invalid | abnormal return even with a valid count |
| `HOST_PRIORITY_INFO` | 5 | implemented | <8 -> `KERN_FAILURE`; sets 8 | **observed call; no lock, immediate success** |
| `HOST_SEMAPHORE_TRAPS` | 7 | implemented as gestalt | ignores input count, sets `*count = 0` | zero-count success may surprise callers |
| `HOST_MACH_MSG_TRAP` | 8 | implemented as gestalt | ignores input count, sets `*count = 0` | same |
| `HOST_VM_PURGABLE` | 9 | implemented | <count -> `KERN_FAILURE`; sets count | **`vm_purgeable_stats` can block on VM locks**; not pthread_init |
| `HOST_DEBUG_INFO_INTERNAL` | 10 | DEVELOPMENT/DEBUG only | <count -> `KERN_FAILURE` | non-DEV returns `KERN_NOT_SUPPORTED` |
| `HOST_CAN_HAS_DEBUGGER` | 11 | implemented | <1 -> `KERN_FAILURE`; sets 1 | none |
| `HOST_PREFERRED_USER_ARCH` | 12 | implemented | <count -> `KERN_FAILURE`; sets count | none |
| `HOST_PROCESSOR_SLOTS` | — | **not defined / no case** | default | immediate `KERN_INVALID_ARGUMENT`, never a wait |

**Bottom line**: the only host_info branch that can plausibly block is
`HOST_VM_PURGABLE`'s `vm_purgeable_stats`; the observed flavor 5 cannot.  If ext4
shows `enter flavor=5` with no `ret`, the block is below the switch (MIG reply path or
the trace console), not a host lock.

## 3. ext4 result branches

### A. `enter flavor=5 count=8` but **no** matching `ret`

- Do **not** add another timeout/limits boot.
- The switch case has no lock; check the serial log for a panic and whether the
  `enter` line is the last kernel line.
- Next instrumentation: add a `kprintf` immediately before
  `basic_info = ...`/inside `case HOST_PRIORITY_INFO`, and a `kprintf` in
  `ipc_kobject_server_internal` before/after `(*ptr->routine)`.  This separates
  "stuck inside host_info_inner" from "MIG reply never sent".
- If `ipc_kobject_server` after-call is absent, the wait is in the kobject/MIG reply
  path (host port right/reply port), not `host_info`.

### B. `enter ...` + `ret=0 count=8` (immediate return)

- host_info is not the blocker.  The extension PC remains in `_host_info` because the
  MIG stub is waiting for a reply that the kernel says it sent, or because the trace
  is sampled around a rapid loop.
- Next boot (ext5) should sample the post-host_info PC with `ustacks`, not add more
  host_info arguments:

```sh
XCMD="glwire_local=1 launchd_disable_all_timeouts=1 hakios_host_info_trace=1 \
      ustacks=PodcastsWidget ustacks_at=680 ustacks_ms=1000 ustacks_count=90 \
      ustacks_threads=8 ustacks_tname=*" \
HOST_ENV="GLWIRE_NO_PANEL=1" \
MINFRAMES=999999 MINSHOT=300000 UI_GRACE=400 NOUI_BY=900 MAXSECS=1120 \
  ./harness/boot-measure.sh ext5 /tmp/root-byref.apfs 1120 --no-panel
```

- If ret is **non-zero** (e.g. `KERN_FAILURE`), the count/flavor contract is the next
  target: compare the same `enter count` with the branch's minimum; do not change
  libSystem.
- If `ret=0` and the new stack is past `_pthread_init` (main / XPC), proceed to the
  CheckIn grep below even if the old `_host_info` sample remains in the log.

## 4. ext5 CheckIn criteria

Exact launchd string to look for (launchd has both "checked in as alive" and the
failure "check-in timeout"):

```sh
grep -aE "checked in as alive|Extension is hanging|CHECK_IN_TIMEOUT|killing extension|xpcproxy spawned|extension checked in" /tmp/ext5.log
```

PASS for the extension path:

- at least one `... extension checked in as alive ...` (case-sensitive as emitted);
- **no** `Extension is hanging on launch. Killing.` for PodcastsWidget;
- **no** `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` / `CHECK_IN_TIMEOUT` for that pid;
- `ustacks: PodcastsWidget` shows the thread past `_pthread_init` (main/UIKit/XPC run
  loop), and the pid is alive >60 s.

PARTIAL: pid alive, no kill, but `checked in as alive` 0 → CheckIn still missing;
the next stack tells whether it is XPC (`xpc_main`/`bootstrap_check_in`) or a later
startup hang.

FAIL: still `Extension is hanging` / `CHECK_IN_TIMEOUT` → the timeout-disable or
host_info fix did not reach this launch; do not claim P2/P3.

## 5. Build note (host)

Successful host build used:

```sh
LD64_LLD=/usr/lib/llvm-18/bin/ld64.lld make kernel -j8
```

`ld64.lld` is required; `ld.lld` rejects `-rename_section`.  Container has no
bison/flex/m4/uuid and no root, so the host is the only build path.  Verify:

```sh
ls -l hakios-kernel
strings hakios-kernel | grep -c 'hakios: host_info'   # 2
```
