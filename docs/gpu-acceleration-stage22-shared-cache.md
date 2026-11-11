# S22: appex dependencies are in the dyld shared cache

## 1. Static result: all dependencies are in the cache

`tools/glwire/harness/cache_deps.py` extracts the path table from every
`dyld_shared_cache_arm64*` file with `strings`, then compares it with
`llvm-objdump --macho --dylibs-used` for each appex.  No `ipsw` is needed.

```
cache: .../20F66__iPhone10,3_6/dyld_shared_cache_arm64* (35335 image paths)
BooksWidgetExtension:        48 deps, 0 missing
PeopleWidget_iOSExtension:   37 deps, 0 missing
WeatherWidget:               44 deps, 0 missing
mobilenotes.WidgetExtension: 45 deps, 0 missing
AppStoreWidgetsExtension:    49 deps, 0 missing
```

Direct checks:

```
/System/Library/Frameworks/Foundation.framework/Foundation
/System/Library/Frameworks/UIKit.framework/UIKit
/System/Library/Frameworks/WidgetKit.framework/WidgetKit
/usr/lib/libSystem.B.dylib
```

are all present in
`sysimage/20F66__iPhone10,3_6/dyld_shared_cache_arm64*`.

So the earlier "every dependency is missing" result was checking the **rootfs
filesystem**, not the cache.  These images are expected to be cache-only; the
disk absence is not the failure.  The cache is split (`dyld_shared_cache_arm64`
384K header/path table plus `.01`, `.02`, ...); scan all of them, not only the
main file.

## 2. `vm_shared_region_start_address() failed` is not sandbox selection

In `osfmk/vm/vm_shared_region.c`:

- the function waits while `sr_mapping_in_progress` is set;
- if `sr_first_mapping == -1`, the region is **empty**, it returns
  `KERN_INVALID_ADDRESS`;
- otherwise it returns `KERN_SUCCESS` with `sr_base_address + sr_first_mapping`.

`shared_region_check_np()` turns that failure into `ENOMEM`
(bsd/vm/vm_unix.c:2108).  dyld then falls back to mapping the cache with
`shared_region_map_and_slide_2_np`/mmap.  A failure therefore says "this task's
current shared region has no mapping yet", not "sandbox gave the extension a
different cache".

Each task's region is `task->task_shared_region` via `vm_shared_region_get(task)`;
sandbox/MACF does not choose it.  The `Diagnostics`/dyld error path is consistent
with a failed dependency lookup, but the cache itself contains all five appexes'
dependencies.

## 3. Minimal experiment: print the region and cache magic

Instrument only the failure/success path, not `stat` entry, and never use
`copyinstr`:

```c
/* osfmk/vm/vm_shared_region.c: vm_shared_region_start_address() */
if (kr != KERN_SUCCESS) {
    printf("hakios: sr_start pid=%d comm=%s base=0x%llx first=%lld\n",
        task_pid(current_task()), current_proc->p_comm,
        (unsigned long long)sr_base_address,
        (long long)sr_first_mapping);
}
```

```c
/* bsd/vm/vm_unix.c: shared_region_check_np(), after the call */
printf("hakios: sr_check pid=%d comm=%s sr=%p first=%lld kr=%d err=%d\n",
    proc_getpid(p), p->p_comm, shared_region,
    shared_region ? (long long)shared_region->sr_first_mapping : -1,
    kr, error);
```

To prove the cache is visible when check succeeds, copy only the header magic
(no path string):

```c
char magic[16];
if (copyin((user_addr_t)start_address, magic, sizeof(magic)) == 0) {
    printf("hakios: sr_magic pid=%d comm=%s magic=%.16s\n",
        proc_getpid(p), p->p_comm, magic);
}
```

Expected: `magic` is `dyld_v1   arm64` for a valid cache.  If check fails
(`ENOMEM`/empty region) and the map-file path succeeds, the fallback is normal;
the extension still sees the same cache.

For the map path, print pid/comm and the mapping result around
`vm_shared_region_map_file_final()`; do not print user paths.

## 4. Judgment

- Cache contains all dependencies → **not a missing-image problem** from the
  static side.
- `sr_check` failure with `first == -1` → **empty region before map**, dyld
  fallback; not sandbox.
- If `sr_check` success gives `dyld_v1   arm64` and dyld still errors on a
  dependency, the problem is the **lookup/rpath/closure**, not visibility.
- If `sr_check` success gives a different magic/address for an extension than
  launchd, then compare `task_shared_region` setup; but sandbox is not the
  selector.
- Do not add a stat/sandbox bypass based on this; there is no stat-block evidence.
