# S22 ext18: six widget stacks are all inside dyld

Symbolized full output: `docs/s22-ext-18-symbolized.txt`.
Raw log: `/host/iosvm/logs/s22-ext/ext18/serial.log` (kernel `193cbe74…`,
`touch.py stacks <pid>` numeric mode, slide `0x4b73c000`).

## Top frames per extension

| pid | process | top 3 | library |
|---:|---|---|---|
| 45 | BooksWidgetExtension | `__kernelrpc_mach_vm_deallocate_trap + 8`; `Diagnostics::clearError + 28`; `dyld4::Loader::getLoader(...)::block_invoke + 1676` | libsystem_kernel trap; rest **dyld** |
| 46 | AppStoreWidgetsExtension | `dyld4::RuntimeState::setUpTLVs(...)::block_invoke + 36`; `MachOAnalyzer::forEachThreadLocalVariableInSection + 96`; `MachOFile::forEachSection + 520` | **dyld** |
| 47 | GeneralMapsWidget | `simple_salloc + 72`; `Diagnostics::error(char const*, char*) + 40`; `dyld4::Loader::getLoader(...)::block_invoke + 1168` | libSystem malloc; rest **dyld** |
| 48 | HealthMedicationWidget | `dyld3::MachOFile::trieWalk + 140`; `Loader::hasExportedSymbol + 356`; `Loader::resolveSymbol + 788` | **dyld** |
| 49 | RemindersWidgetExtension | `simple_salloc + 72`; `Diagnostics::error + 40`; `dyld4::Loader::getLoader(...)::block_invoke + 1168` | libSystem malloc; rest **dyld** |
| 50 | WorldClockWidget | `stat64 + 8`; `dyld3::stat + 24`; `SyscallDelegate::fileExists + 36` | libsystem_kernel syscall; rest **dyld** |

Every stack has **1 thread** and ends at `dyld4::prepare` / `start + 1760`.  No
`libSystem_initializer`, `pthread_init`, `xpc_main`, `NSExtensionMain`, or
`bootstrap_check_in` appears.

## Interpretation

**The six extensions are still in dyld startup, before libSystem initialisation.**
The common path is image/dependency loading:

- `Loader::getLoader` / `forEachResolvedAtPathVar` / `PathOverrides::forEachPathVariant`
  — dependency/path lookup (Books, Maps, Reminders);
- `RuntimeState::setUpTLVs` + `forEachThreadLocalVariableInSection` — TLV setup after
  dependent load (AppStore);
- `MachOFile::trieWalk` / `hasExportedSymbol` / `resolveSymbol` — chained-fixup
  symbol resolution (Health);
- `PrebuiltLoader::isValid` / `invalidateInIsolation` / `fileExists`/`stat` —
  prebuilt-loader invalidation check (WorldClock);
- `Diagnostics::error` — dyld is on an error path looking for a dependent.

The single thread is exactly what "before `pthread_init`/libSystem" predicts.
This is **not** libSystem init, **not** host_info, **not** XPC/CheckIn, and not an
extension main loop.  It is **dyld dependency loading / fixup**, most likely a
missing or invalid dependent image/path or a prebuilt-loader/cache invalidation
loop.

Next proof (not another timeout):

- add `DYLD_PRINT_LIBRARIES=1` (and `DYLD_PRINT_INITIALIZERS=1`) to the widget
  launchd job's `EnvironmentVariables`; the last library before the stall names the
  missing dependency;
- check the appex's `LC_LOAD_DYLIB`/rpath and the image's framework paths;
- if `PrebuiltLoader` is invalidating, compare the shared-cache/prebuilt-loader
  set with the appex's expected closure;
- `Diagnostics::error` frames should print the missing path under
  `DYLD_PRINT_LIBRARIES`; do not change host_info/timeout.

## How the symbolization was done without a usable `ipsw` in PATH

We did have the `ipsw` **binary** in the worktree:
`/host/iosvm-work/hakios-xnu-kernel/qemu/ipsw` (91 MB, downloaded earlier; not an
Apple binary).  It was simply not in `PATH`.

The important part is the **dyld cache with a `.symbols` sidecar**:

```
/host/iosvm/build/images/rootfs/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64
/host/iosvm/build/images/rootfs/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.symbols
```

`sysimage/a2s.json` has only ~165 old entries and is not enough; `ipsw dyld a2s`
needs the **`.symbols`** beside the cache.  Reproduce with:

```sh
cd /host/iosvm-work/hakios-xnu-kernel
IOSVM=/host/iosvm-work/hakios-xnu-kernel \
IPSW=/host/iosvm-work/hakios-xnu-kernel/qemu/ipsw \
SYMSTACK_CACHE=/host/iosvm/build/images/rootfs/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64 \
SYMSTACK_KNOWN=/tmp/symknown.json \
python3 /path/to/tools/glwire/harness/symstack.py \
    /host/iosvm/logs/s22-ext/ext18/serial.log
```

- `SYMSTACK_CACHE` must point at the cache whose `.symbols` exists (the default
  `sysimage/20F66…/dyld_shared_cache_arm64` has no `.symbols`);
- `SYMSTACK_KNOWN` should be a fresh/empty JSON; an old cache full of `?` causes
  false unknown symbols;
- `symstack.py` was changed to take the **containing-symbol line** from
  `ipsw dyld a2s` (not only an exact-symbol hit);
- the parser accepts `ustacks: Name[pid] ... shared cache slide 0x…`.

If there is no `ipsw` binary at all, the shared cache's `.symbols`/dSYM still has
to be read by a Mach-O/dyld-cache tool; the 165-entry `a2s.json` alone cannot
symbolize these addresses.
