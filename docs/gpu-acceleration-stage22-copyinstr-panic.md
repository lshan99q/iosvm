# S22 ext19 panic: `copyinstr(..., NULL)` in stat64, not a user-path fault

## 1. The faulting instruction is `copyinstr+0x18`

Kernel `844a0953…` symbols:

```
fffffff007451edc  copyinstr
fffffff007451ef4  copyinstr + 0x18   <-- panic PC
```

Disassembly:

```
fffffff007451ef0: f90007ff   str xzr, [sp, #0x8]
fffffff007451ef4: f900007f   str xzr, [x3]       <-- data abort
fffffff007451ef8: b4000342   cbz x2, copyinstr+0x84
...
```

`copyinstr` (osfmk/arm64/copyio.c:310) starts with:

```c
int copyinstr(..., vm_size_t *lencopied)
{
    vm_size_t bytes_copied = 0;
    *lencopied = 0;              /* no NULL check */
    ...
}
```

The stat trace called:

```c
copyinstr(uap->path, kpath, sizeof(kpath), NULL);
```

so **x3 is NULL** and the first store faults.  This is a self-inflicted NULL
pointer write, not a bad user path and not a syscall-context limitation of
`copyinstr` per se.

## 2. The shared_region message is unrelated

`shared_region_check_np()` calls `vm_shared_region_start_address()`; on failure it
sets `ENOMEM` and logs `vm_shared_region_start_address() failed`
(bsd/vm/vm_unix.c:2108).  That is a **normal dyld fallback path** (dyld then mmaps
the cache).  It does **not** data-abort; the abort PC is inside `copyinstr`.

Order in the log (shared-region message, then data abort) is timing, not causation.

## 3. Safe observation of the stat path

Do **not** copy the user path at syscall entry.  Use the kernel's already-copied
path/vnode:

```c
/* bsd/vfs/vfs_syscalls.c: fstatat_internal(), after nameiat() succeeds */
printf("hakios: stat namei pid=%d vp=%p path=%s\n",
    proc_getpid(vfs_context_proc(ctx)), nd.ni_vp,
    nd.ni_cnd.cn_pnbuf ? nd.ni_cnd.cn_pnbuf : "?");

/* before/after vn_stat_noauth/vn_stat */
printf("hakios: stat vn pid=%d vp=%p enter\n", ...);
error = vn_stat_noauth(...);
printf("hakios: stat vn pid=%d vp=%p err=%d\n", ..., error);
```

`nd.ni_cnd.cn_pnbuf` is a kernel buffer after `nameiat`; printing it does not
copy from user.  If even that is too early, print only `vp`/`error` and use
`vnode_getname()` on a referenced vnode (or `vnode_vid`) later.

For the MACF question, trace **vnode pointers** in
`mac_vnode_check_stat`/`vn_stat_noauth`, not the path string:

```c
printf("hakios: mac_stat vp=%p cred=%p\n", vp, cred);
```

Or use kdebug `FSDBG_CODE(DBG_FSRW, ...)` with the vnode id; no user copyin at
syscall entry.

## 4. Stat or another syscall?

- Add a **return** trace as above (vp/err, no user string).  Entry-only cannot
  prove a block.
- A second `ustacks` of the same pid still in `stat64+8` (or `vn_stat`) is the
  strong block signal.
- If `vn_stat` returns, move to `mmap`/`mach_vm_map`, `open/openat`,
  `getattrlist`, `fcntl`, `csops`; log vnode/fd/err, not paths via copyinstr.
- `vm_shared_region_start_address() failed` is not a hang signal; it is a dyld
  shared-region fallback.

## 5. Minimal fix candidates if stat truly blocks

1. Confirm the exact vnode and its filesystem (root APFS/sealed/cryptex) from the
   `vp`/`vnode_getname` trace; compare with a daemon that stats the same prefix.
2. MACF/sandbox: compare profiles/entitlements; only a **narrow read-only appex**
   exemption is defensible, never a global sandbox short-circuit.
3. Vnode lock contention: if the kernel stack is waiting on a vnode lock, find the
   holder; do not patch `stat64` to skip it.
4. dyld negative/prebuilt-loader cache: avoid repeated `fileExists` on the same
   missing dependency; fix the image/rpath/closure first.
5. Do not make `stat64` return success: that hides a missing image and can make
   dyld execute the wrong closure.

No boot by code; the next trace must avoid `copyinstr` at syscall entry.
