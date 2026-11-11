# S22 ext19: what a `hakios: stat` line can and cannot prove

## 1. ext18 only puts one widget in `stat`

The six ext18 stacks are not all in `stat`.  Their top frames are:

| pid | process | top | in stat? |
|---:|---|---|---|
| 45 | BooksWidgetExtension | `mach_vm_deallocate_trap + 8` → `Diagnostics::clearError` → `Loader::getLoader` | no |
| 46 | AppStoreWidgetsExtension | `RuntimeState::setUpTLVs` → `forEachThreadLocalVariableInSection` | no |
| 47 | GeneralMapsWidget | `simple_salloc` → `Diagnostics::error` → `Loader::getLoader` | no |
| 48 | HealthMedicationWidget | `MachOFile::trieWalk` → `hasExportedSymbol` → `resolveSymbol` | no |
| 49 | RemindersWidgetExtension | `simple_salloc` → `Diagnostics::error` → `Loader::getLoader` | no |
| 50 | WorldClockWidget | **`stat64 + 8`** → `dyld3::stat` → `SyscallDelegate::fileExists` | **yes** |

So the correct statement is: **WorldClockWidget is in a stat syscall**; the other
five are in dyld path resolution/fixup/TLV code.  They may *later* stat, but their
sampled frames do not show it.

## 2. The current stat trace prints only entry

`bsd/vfs/vfs_syscalls.c:7100`:

```c
int stat64(proc_t p, struct stat64_args *uap, ...)
{
    if (hakios_stat_trace) {
        ... if path contains .appex or PlugIns:
            printf("hakios: stat pid=%d path=%s\n", proc_getpid(p), kpath);
    }
    return fstatat_internal(...);
}
```

There is **no return/errno print**.  Therefore:

- a line proves `stat64` was **entered** for that pid/path;
- no line after it does **not** prove it is blocked; the call may have returned and
  the filter may have suppressed the next path (only `.appex`/`PlugIns` are printed);
- a line plus a **later identical pid still in `stat64 + 8`** (another `ustacks`)
  is stronger evidence of a hang;
- a line plus a later `exec`/`created`/progress proves it returned.

The minimal addition is:

```c
error = fstatat_internal(...);
printf("hakios: stat ret pid=%d err=%d path=%s\n", proc_getpid(p), error, kpath);
return error;
```

Only entry+no-return+still-in-stat+same pid is a block.

## 3. If a widget's last stat path is `.appex`/`PlugIns`

Candidate causes, in order of plausibility:

1. **APFS/vnode path lookup** on the sealed/root image: `namei` → vnode lookup
   → `vn_stat_noauth`; a missing/very long component or a root-image path that is
   not present can spin in dyld's path-variant loop (the `Diagnostics::error`
   frames in Maps/Reminders fit a missing dependency).
2. **MACF/sandbox vnode check** (`mac_vnode_check_stat` / `vnode_authorize`):
   normally quick, but a policy/entitlement path that calls back into userspace
   or takes a vnode lock can stall; the extension's sandbox profile is the first
   thing to compare with a daemon that stats the same path successfully.
3. **vnode/APFS lock contention**: another process holds the vnode lock (or the
   root image is being modified); `stat64 + 8` in the kernel is the syscall
   instruction, so a lock wait would show in the kernel stack below `stat64`.
4. **sealed volume / cryptex path**: the appex path may resolve through a
   cryptex/preboot mount that is not fully up; compare with a successful daemon
   stat of the same prefix.

Do not attribute all six stacks to stat; Books/AppStore/Health are not in stat.

## 4. If stat returns normally

If entry and return both appear and the pid progresses, the next syscalls to
instrument are:

- `mmap`/`mach_vm_map` (dyld image mapping; `mmap` frames);
- `open`/`openat`/`getattrlist` (dyld path resolution);
- `fcntl` (dyld file descriptors);
- `csops`/`csops_audittoken` (code-sign/entitlement checks);
- `mach_msg`/bootstrap lookups only after libSystem is up (not yet at these
  stacks).

Print pid/path/ret for `open`/`mmap`/`csops` with the same `.appex`/`PlugIns`
filter, not all paths.

## 5. Minimal fix candidates if stat truly blocks

Prefer diagnosis over bypass:

1. **Confirm the blocking vnode**: kernel stack below `stat64` (`ustacks` already
   gives user frames; use `ustacks` on the kernel? no—use `showtask`/kernel stack
   if available) and the exact final path.  If it is a missing dependency, fix the
   image path rather than the stat.
2. **Sandbox/MACF**: compare the extension's sandbox profile/entitlements with a
   daemon that stats the same prefix; only then consider a narrow
   `mac_vnode_check_stat` exemption for read-only appex paths.  A blanket
   sandbox short-circuit is not acceptable.
3. **vnode attribute/negative-dentry cache**: if the same missing path is retried
   in a loop, cache the negative result in dyld (prebuilt loader/path override),
   not in the kernel.
4. **Sealed/cryptex**: ensure the preboot/cryptex mount for the appex is present
   before launchd starts the widget; skipping `fileExists` is a last resort.
5. **Do not** disable sandbox globally or patch `stat64` to return success: that
   hides the missing image and can let dyld exec the wrong closure.

## 6. Decision table for ext19

| ext19 evidence | conclusion | next |
|---|---|---|
| widget pid last `stat` path `.appex`, then no return, same pid still `stat64+8` | stat block candidate | add ret trace; kernel stack/vnode; compare sandbox |
| `stat` entry+ret, then `exec`/`created`/CheckIn | stat returned | trace `mmap`/`open`/`fcntl`/`csops` |
| no widget stat at all, but stacks still dyld | not stat | DYLD_PRINT_LIBRARIES; dependency path |
| stat path is a missing dependency, loop in `Loader::getLoader` | missing image, not fs hang | fix image/rpath/closure |

The stat trace alone cannot distinguish "returned" from "still in syscall"; pair it
with a second `ustacks` of the same pid or with a return print.
