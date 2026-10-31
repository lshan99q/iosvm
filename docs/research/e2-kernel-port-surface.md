# E2 — Kernel port surface: `xnu-8796.121.2` → `xnu-12377.121.6`

Both trees cloned shallow (`--depth 1 --filter=blob:none --sparse`) from
`github.com/apple-oss-distributions/xnu`, APSL-2.0, no binaries. Sparse set:
`bsd/kern bsd/sys bsd/dev/arm osfmk/{arm64,arm/pmap,kern,vm,mach} config libsyscall SETUP
libkern/{amfi,libkern/amfi} EXTERNAL_HEADERS`. Disk peak ≈ 120 MB. All numbers are from
`git diff --no-index` between the working trees; the shallow clones share no history.

**Scale of the diff** in the checked-out dirs: `osfmk` 528 files
(+202 220/−71 230), `bsd` 303 (+54 111/−27 065), `EXTERNAL_HEADERS` 99 (+8 182/−2 867),
`libsyscall` 52 (+3 290/−482), `config` 39 (+701/−500).


## a. Syscall surface

`bsd/kern/syscalls.master`: 871 → 878 lines, diff `28 insertions(+), 21 deletions(-)`.
Highest number 555 → 557.

| category | count | detail |
|---|---|---|
| **renumbered** | **0** | no existing number changed meaning |
| **removed** | **0** | |
| added, new tail | 2 | 556 `sys_coalition_policy_set`, 557 `sys_coalition_policy_get`, both under `#if CONFIG_COALITIONS`, else `enosys` (:872-878) |
| added, holes filled | 5 | 67 `oslog_coproc_reg` (:125), 68 `oslog_coproc` (:126), 164 `funmount` (:260), 185 `sys_panic_with_data` (:281), 535 `objc_bp_assist_cfg_np` (:847) — all `nosys`/`enosys` in the old tag |
| renamed in place, ABI-neutral | 4 | 12 `chdir`→`sys_chdir`, 13 `fchdir`→`sys_fchdir`, 285 `settid`→`sys_settid`, 311 `settid_with_pid`→`sys_settid_with_pid`. `bsd/kern/makesyscalls.sh:699`, `:722` strip a leading `sys_` when emitting `syscall.h` and the name table, so `SYS_chdir` stays 12 |
| arg-type spelling only, ABI-identical | 11 | 65, 73, 74, 75, 78, 197, 203, 204, 250, 405 switch `caddr_t`→`caddr_ut`, `size_t`→`size_ut` — new typedefs equal to the old types (`bsd/sys/_types/_caddr_t.h:36`, `bsd/sys/_types/_size_t.h:68`) |
| arg type really changed | 1 | 536 `shared_region_map_and_slide_2_np` takes `shared_file_mapping_slide_np_ut`, not `shared_file_mapping_slide_np` |

The result: **a kernel that gets syscall numbers right on the old tag still gets them
right on the new one.** Nothing a Darwin-16.5-era userland calls moved; the one real ABI change
is 536, a private (`_np`) call.


## b. Mach traps and upcalls

The trap table is not in `bsd/dev/arm/systemcalls.c`; it is in `osfmk/kern/syscall_sw.c`.
`systemcalls.c` itself is nearly untouched: `3 insertions(+), 3 deletions(-)` (one printf gains
the syscall number, `add_saved_state_pc`→`add_user_saved_state_pc` at :503).
`osfmk/kern/syscall_sw.c`: 390 → 399 lines, `12 insertions(+), 3 deletions(-)`.

| trap | old | new |
|---|---|---|
| 0–62 | as-is | **unchanged** |
| 63 | `kern_invalid` | `mach_vm_reclaim_update_kernel_accounting_trap`, `#if __LP64__` only, `munge_wll` |
| 64–87 | as-is | **unchanged** |
| 88 | `kern_invalid` | `_exclaves_ctl_trap`, `munge_wwllllll`, 8/14 args |
| 89–127 | as-is | **unchanged** |

**Zero trap renumbering**; two previously-invalid slots became live. `osfmk/kern/syscall_sw.h`
is **byte-identical** (119 lines, `MACH_TRAP_TABLE_COUNT 128` at :92).
`osfmk/mach/syscall_sw.h` differs by only `14 insertions(+), 2 deletions(-)`: the two
`kernel_trap()` entries above plus `thread_set_x86_64_compat` at −108 (x86_64 only).
`osfmk/mach/mach_traps.h` (`47 insertions(+), 9 deletions(-)`) adds their prototypes.


## c. exec

`bsd/kern/kern_exec.c`: 7807 → 9078 lines, `1721 insertions(+), 450 deletions(-)` — the
largest file diff here. `bsd/sys/imgact.h` barely moves
(`13 insertions(+), 6 deletions(-)`; `struct image_params` gains only `ip_free_map` and
`ip_flags2`).

### `IMGPF_*` (definitions in `bsd/sys/imgact.h`)

Added: `IMGPF_3P_PLUGINS 0x00000080` (:150), `IMGPF_HARDENED_HEAP 0x00008000` (:159). Removed:
`IMGPF_SB_DEFAULT` / `IMGPF_SB_TRUE` / `IMGPF_SB_FALSE` (simulator-binary detection). Every
other bit keeps its old value — `IMGPF_INTERPRET`, `IMGPF_WAS_64BIT_ADDR`,
`IMGPF_IS_64BIT_ADDR`, `IMGPF_DISABLE_ASLR`, `IMGPF_ALLOW_DATA_EXEC`, `IMGPF_HIGH_BITS_ASLR`,
`IMGPF_IS_64BIT_DATA`, `IMGPF_DRIVER`, `IMGPF_RESLIDE`, `IMGPF_PLUGIN_HOST_DISABLE_A_KEYS`,
`IMGPF_HW_TPRO`, `IMGPF_ROSETTA`, `IMGPF_ALT_ROSETTA`, `IMGPF_NOJOP`.

### Chained fixups

`LC_DYLD_CHAINED_FIXUPS` appears **zero times in `bsd/kern/kern_exec.c` in either tag** — exec
never parses fixups. The kernel-side fixup code is in `osfmk/vm/vm_dyld_pager.c`
(`598 insertions(+), 83 deletions(-)`), and the real change is there: a new pointer format
`DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE`, dispatched at `vm_dyld_pager.c:933` to a new
`fixupCachePageAuth64()` (`:735`, 0 hits in the old tag). The old tag has `ARM64E`,
`ARM64E_USERLAND`, `ARM64E_USERLAND24` but not the shared-cache variant.
`osfmk/mach/dyld_kernel_fixups.h` is effectively frozen (`2 insertions(+), 2 deletions(-)`,
`__always_inline__` attributes only).

### arm64e / PAC

The subtype acceptance test was widened from an exact match (old, `kern_exec.c:990`):
`CPU_SUBTYPE_ARM64_PTR_AUTH_VERSION(cpusubtype) == CPU_SUBTYPE_ARM64_PTR_AUTH_CURRENT_VERSION`,
to a range (new, `kern_exec.c:1018-1021`): `ptrauth_abi_version >=
CPU_SUBTYPE_ARM64_PTR_AUTHV0_VERSION && ptrauth_abi_version <=
CPU_SUBTYPE_ARM64_PTR_AUTH_MAX_PREFERRED_VERSION`.

`osfmk/mach/machine.h` adds `CPU_SUBTYPE_ARM64_PTR_AUTHV1_VERSION 1` and defines
`CPU_SUBTYPE_ARM64_PTR_AUTH_MAX_PREFERRED_VERSION` as **V0 on iOS**, V1 on macOS — so on iOS 26
the accepted set is unchanged, but a v1 ABI now exists to grow into. The macOS-only
`arm64e_abi=os|all` dyld string and `-arm64e_preview_abi` bootarg are **gone** (old :5845,
:6110-6130) — but that block was `#if XNU_TARGET_OS_OSX`, never iOS surface.

### New enforcement a from-scratch AMFI substitute must feed

`kern_exec.c:2149` now calls a new `imgact_setup_runtime_mitigations()`, chaining through new
stages: `imgact_setup_hardened_heap` (:1197), `imgact_setup_platform_restrictions` (:1240),
`imgact_setup_has_checked_allocations_entitlement` (:1325), `imgact_setup_sec` (:1432),
`imgact_setup_guard_objects` (:1742). Each is driven by entitlements read off the code-signing
blob. The new tag adds a whole hardened-process family at `kern_exec.c:1096-1155`:

```
com.apple.developer.hardened-process            com.apple.security.hardened-process.dyld-ro
com.apple.security.hardened-process             com.apple.security.script-restrictions
com.apple.developer.hardened-process.hardened-heap
com.apple.developer.hardened-process.checked-allocations
com.apple.security.hardened-process.checked-allocations.{disable-pure-data,no-tagged-receive,soft-mode}
com.apple.security.hardened-process.containment.ipc
com.apple.security.hardened-process.no-guard-objects
com.apple.security.hardened-process.platform-restrictions[-string]
```

plus `KERN_INVALID_GUARD_OBJECT_SLOT 57` (`osfmk/mach/kern_return.h`, +4 lines, additive).

**The hard one.** `libkern/libkern/amfi/amfi.h` went `73 insertions(+), 4 deletions(-)`:
`KERN_AMFI_INTERFACE_VERSION` **6 → 7** (:38), `OSENTITLEMENTS_INTERFACE_VERSION` **1u → 2u**,
`TRUST_CACHE_INTERFACE_VERSION` **3u → 4u**, a new `queryGetUUID` trust-cache callback, a new
`adjustContext` on `OSEntitlements` taking a `CEContext_t`, and a `KERN_AMFI_SUPPORTS_MTE 4`
block adding five MTE policy callbacks (`has_mte_soft_mode`, `has_mte_opt_out`,
`has_mte_inheritance_opt_out`, `has_mte_data_tagging_override`/`_opt_out`,
`has_mte_alias_restriction_opt_in`). CoreEntitlements V2 also becomes a separately registered
kernel API: `amfi_core_entitlements_register()` and `SECURITY_READ_ONLY_LATE(const
CEKernelAPI_t *) libCoreEntitlements` in `libkern/amfi/amfi.c` (`11 insertions(+), 1 deletion(-)`),
which panics on double registration.

A check exists; the project must satisfy it or stop. The version bump means a v6 AMFI substitute
neither compiles against nor registers with the v7 kernel — the struct layout changed.

Code-signing reconstitution is still a build option in both tags
(`config/MASTER.arm64.iPhoneOS:20`) and still gated identically at `bsd/kern/ubc_subr.c:4685`,
with a hard requirement when a signing monitor is present (`:4695-4697`); that file moved
`334 insertions(+), 108 deletions(-)`.


## d. arm64e and the shared region

`osfmk/vm/vm_shared_region.c`: `637 insertions(+), 416 deletions(-)`.
`osfmk/arm64/pac_asm.h`: `87 insertions(+)`, **0 deletions**.

**The signing path itself is intact.**

| item | old | new | verdict |
|---|---|---|---|
| `jop_key` parameter | `uint64_t jop_key` (:3167, :3378) | `uint64_t jop_key` (:3258, :3487, :3592) | identical |
| `si_ptrauth` | :1180, :2648, :2660 | :1292, :2701, :2713 | identical logic |
| `pmap_sign_user_ptr(void \*, ptrauth_key, uint64_t, uint64_t)` | used | used | signature unchanged |
| ptrauth key domains | `EXTERNAL_HEADERS/ptrauth.h`, 384 lines | **byte-identical**, 384 lines | no change; `asia=0 asib=1 asda=2 asdb=3` |
| `pac_asm.h` | — | +87 lines | all MTE: `PACGA_TAG_*` use-case tags, `PACGA_IRG_RESEED` |

So a signing backend written against the old tag keeps the same key-domain API and the same key
values; `pac_asm.h`'s additions are additive MTE/PACGA machinery, not a re-keying. What it must
cope with is the moved `pmap` headers (item 2 below).

What *did* change around it:

1. **New slide format v5.** `vm_shared_region_slide_page_v5()` at `vm_shared_region.c:3479`,
   dispatched at :3604. The old tag has v1–v4 only. v5 keeps the same `jop_key`/`si_ptrauth`
   discipline but reads a new `vm_shared_region_slide_info_entry_v5` (in the new file
   `osfmk/vm/vm_shared_region_xnu.h:123`) and picks `ptrauth_key_asda` vs `ptrauth_key_asia`
   from a data flag (:3536).
2. **Batched signing fast path.** New `pmap_batch_sign_user_ptr()` at :3336 and :3555 beside the
   existing `pmap_sign_user_ptr()`; declared `osfmk/arm64/sptm/pmap/pmap.h:471`, implemented
   `osfmk/arm64/sptm/pmap/pmap.c:9663`.
3. **Header split.** `osfmk/vm/vm_shared_region.h` 369 → 113 lines; slide-info structs moved to
   the new `osfmk/vm/vm_shared_region_xnu.h` (251 lines). Include paths change.
4. **Shared region grew.** `osfmk/mach/shared_region.h` (`24 insertions(+), 5 deletions(-)`):
   `SHARED_REGION_SIZE_ARM64` `0x100000000` → `0x180000000` (4 GiB → 6 GiB) and
   `SHARED_REGION_SIZE_ARM64_32` `0xa6000000` → `0x88000000`. Slide arithmetic depends on this.
5. `VM_DEFINE_UNSAFE_TYPE` now wraps `shared_file_mapping_slide_np` — a new kernel-wide
   user-pointer typing mechanism (`osfmk/mach/shared_region.h:185-196`).


## e. Config

`config/MASTER.arm64.iPhoneOS`: 112 → 132 lines, `47 insertions(+), 27 deletions(-)`.

**Survived unchanged:** `config_enforce_signed_code` (:19 → :20), `config_code_signature_reconstitution`,
`config_library_validation`, and `config_pmap_ppl` — the last one relocated into a new
`ARM_EXTRAS_BASE` group (:18) rather than dropped.

**Appeared:** a `SPTM` build group plus `MASTER_CONFIG_ENABLE_SPTM` / `MASTER_CONFIG_ENABLE_KERNEL_TAG`
conditionals (:84-93); `kernel_tagging` (in `VM_KASAN` and via new `VM_EXTRA`/`VM_EXTRA_DEV`;
`options CONFIG_KERNEL_TAGGING` at :132); `config_ucoredump`, `config_ext_paniclog`,
`config_imgsrc_access`, `if_redirect`, `kctl_test`, `config_memory_microstackshot`,
`config_task_suspend_stats`, and a `CPU_COUNTERS_BASE` group holding `config_cpu_counters` /
`config_pervasive_cpi` / `config_pervasive_energy`.

**Disappeared:** `config_sched_traditional`, `config_sched_multiq`, `config_sched_grrr`,
`config_sched_proto` (the first two replaced by `config_sched_timeshare_core`), plus `NFS_DEV`,
`config_nexus_monitor`, `config_ledger_interval_max`, `pgzalloc`, `kpc`/`PERFMON_BASE` and
`kernel_tbi`.

New build switches, absent from the old tag: `SETUP/config/doconf:158` `-sptm`, `:161`
`-exclaves`, `:164` `-tag`. The old tag's per-SoC `MASTER.arm64.t8110.iPhoneOS` and
`t8120.iPhoneOS` are gone; `MASTER.arm64.WatchOS` is new.

SPTM is a real second pmap: `osfmk/arm64/sptm/pmap/pmap.c` is 13 473 new lines (15 more files
under `osfmk/arm64/sptm/`), while legacy `osfmk/arm/pmap/` survives in both tags
(`2419 insertions(+), 1233 deletions(-)`, 8 files). 38 files are new under `osfmk/arm64/`
overall — `mte.c` (330 lines), `sme.c` (349), `sop.c` (317), `apt.c`, `cpc_arm64.c`,
`x86_64_compat.c`, `hibernate_secure_hmac.c`.


## f. What did NOT change

Of the 1500 files present in both sparse trees, **770 are byte-identical**, 460 of them headers.
Byte-identical and load-bearing: `osfmk/kern/syscall_sw.h` (119 lines,
`MACH_TRAP_TABLE_COUNT 128` at :92), `osfmk/mach/arm/syscall_sw.h`, `EXTERNAL_HEADERS/ptrauth.h`
(384 lines — the ptrauth key enum is frozen), `osfmk/mach/thread_status.h`,
`osfmk/mach/arm/vm_types.h`, `osfmk/mach/arm/boolean.h`, `osfmk/mach/arm/kern_return.h`,
`osfmk/mach/notify.h`, `osfmk/mach/processor_info.h`. Near-frozen:
`osfmk/mach/kern_return.h` (+4, nothing removed), `osfmk/mach/dyld_kernel_fixups.h`
(`2 insertions(+), 2 deletions(-)`, inline attrs only), `bsd/dev/arm/systemcalls.c`
(`3 insertions(+), 3 deletions(-)`), `bsd/sys/imgact.h` (`13 insertions(+), 6 deletions(-)`).

Concretely: syscall numbers 0–555 keep their meaning, mach traps 0–62/64–87/89–127 keep their
positions, the trap table size is unchanged, the ptrauth key domain count and values are
unchanged, `jop_key`/`si_ptrauth` plumbing is unchanged, and the `IMGPF_*` bits below `0x80`
are unchanged.


## What this means for the port

- **No work — syscall and mach-trap numbering.** 0 renumbered, 0 removed, 0 traps moved
  (`bsd/kern/syscalls.master`, `osfmk/kern/syscall_sw.c`; `osfmk/kern/syscall_sw.h`
  byte-identical). A table built for the old tag stays correct. Traps 63 and 88 become live but
  were `kern_invalid`, so nothing regresses.
- **Small work in `osfmk/vm/vm_shared_region.c` — accept slide format v5.** v5 is dispatched at
  :3604 beside v1–v4, retained unchanged in shape, so the old path still compiles.
  Structs moved to `osfmk/vm/vm_shared_region_xnu.h` (includes change);
  `SHARED_REGION_SIZE_ARM64` is now `0x180000000` — 6 GiB, not 4.
- **Small work in `osfmk/vm/vm_dyld_pager.c` — one new pointer format.**
  `DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE` at :933 → `fixupCachePageAuth64()` at :735. The other
  four formats and the `signPointer()`/`dyld_a_key` path are unchanged.
- **Research needed — AMFI interface v6 → v7.** `libkern/libkern/amfi/amfi.h:38` bumped the
  version, added five MTE callbacks, changed `OSEntitlements` v1→v2 and `TRUST_CACHE` v3→v4, and
  split out a CoreEntitlements V2 registration (`libkern/amfi/amfi.c`). A substitute must fill in
  the v7 struct and register `libCoreEntitlements`, and answer for the hardened-process
  entitlement family at `kern_exec.c:1096-1155`. A check exists; the project must satisfy it or
  stop. Whether each new MTE/hardened-heap callback is implementable without device-specific
  knowledge is not answered by the source — that is the research.
- **Research needed — SPTM.** `osfmk/arm64/sptm/` is 16 new files, 13 473 lines of new pmap,
  reachable only via `-sptm` (`SETUP/config/doconf:158`). Legacy `osfmk/arm/pmap/` still builds
  in the new tag, so this is a choice, not a forced migration; whether iOS 26 arm64e *requires*
  SPTM is not determinable from source alone.
- **Small work in `config/MASTER.arm64.iPhoneOS`.** `config_enforce_signed_code`,
  `config_code_signature_reconstitution` and `config_library_validation` survive unchanged;
  `config_pmap_ppl` moved to `ARM_EXTRAS_BASE`; `config_sched_traditional`/`config_sched_multiq`
  are gone, replaced by `config_sched_timeshare_core`; `NFS_DEV` and `kpc` are gone.

### Not checked

`iokit/`, `bsd/vfs`, `bsd/net`, `bsd/dev` (except `arm`), `osfmk/x86*`, `libkern` (except
`amfi`) and most of `EXTERNAL_HEADERS` were outside the sparse checkout, so the scale numbers
understate the whole tree. `MASTER.arm64.t8110.iPhoneOS` / `t8120` were not diffed (absent from
the new tag). Whether iOS 26's shipped shared cache actually uses slide format v5 or
`DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE` needs an IPSW, which is out of scope; this only
establishes that the new kernel can handle both and the old one cannot.
