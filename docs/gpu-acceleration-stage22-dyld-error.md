# S22: stat returned, dyld is on its error path

## 1. ext21/ext22: stat is not the stall

- ext21 (kernel `fb48916d…`) logged only 1–2 stat calls per extension pid, all
  on its own appex path; no hang.
- ext22's begin/done version logged **4756 begin = 4756 done**: every stat
  returned.
- Therefore "dyld is blocked in stat" is **excluded**.  `stat64+8` in ext18 was a
  sampled syscall, not proof of a permanent wait.

## 2. What `Diagnostics::error`/`clearError`/`simple_salloc` mean

ext18's Maps/Reminders/Books stacks contain:

```
simple_salloc
Diagnostics::error
Diagnostics::clearError
dyld4::Loader::getLoader(...)
forEachResolvedAtPathVar
PathOverrides::forEachPathVariant
```

This is dyld's **failure reporting** path, not a normal symbol lookup: a
dependency lookup failed, dyld is formatting/storing the error, and then retries
other path variants.

What it does and whether it can block:

- `Diagnostics::error` formats into a dyld-owned string; `simple_salloc`
  allocates it.  This is **malloc/formatting**, not a syscall or console write.
  With one thread there is no lock contention to explain a permanent stall.
- dyld does **not** write the error to stderr by default; it stores it and may
  abort only if the image is required.  So "formatting the error" itself is not a
  blocking I/O.
- The expensive/looping part is the **retry over path variants**:
  `Loader::getLoader` → `forEachResolvedAtPathVar` → `PathOverrides` → `fileExists`
  (`stat`).  ext22 proves those stats return, so this is a **loop/expensive
  repeated lookup**, not a blocked stat.
- `Diagnostics::clearError` just resets the stored error before the next attempt.

So: **dyld is failing to find a dependent image and cycling through candidates**;
the `Diagnostics` frames are the symptom, not the lock.

## 3. Enable dyld logging globally (no kernel/plist change)

The extension jobs are generated from
`/System/Library/xpc/launchd.plist` (`AppExtensions` entries have
`_executablePath`/`_infoPlist`); there is no per-appex `EnvironmentVariables` in
that generated table.  Do not edit that table for this.

The reliable injection point is **before launchd starts**, in the small image's
`root/etc/rc` (the script that ends with `exec /sbin/launchd.darwin`):

```sh
export DYLD_PRINT_LIBRARIES=1
export DYLD_PRINT_SEARCHING=1
# optional, but statistics only prints when the process exits:
# export DYLD_PRINT_STATISTICS=1
exec /sbin/launchd.darwin
```

`launchd` inherits this environment and passes it to the jobs it spawns.  Do
**not** put it in `root/etc/rc.jobs`: that file runs inside the already-started
`com.hakios.demo` job, after launchd, and cannot change the extension's
environment.

Per-job `EnvironmentVariables` in a LaunchDaemon plist only affects that explicit
job, not the generated extension jobs.  There is no `launchctl` binary in this
image, so `launchctl setenv` is not an option here.

`DYLD_PRINT_LIBRARIES` prints each image as dyld loads it; `DYLD_PRINT_SEARCHING`
prints the paths it tries.  Both go to the process's stderr (the console for these
jobs).  `DYLD_PRINT_STATISTICS` is exit-time only and useless for a hung extension.

## 4. Reading the output

- **Last `DYLD_PRINT_LIBRARIES` line before the stall** = last image dyld accepted.
  The next `@rpath/...`/framework name in the appex's `LC_LOAD_DYLIB` is the
  missing one.
- **`DYLD_PRINT_SEARCHING`** shows the candidate paths; the first path that appears
  without a following "found"/load is the one being retried.  `did not find`,
  `not found`, `Library not loaded`, `image not found`, and
  `Referenced from:` are the useful markers.
- Match the appex binary's dependencies with `otool -L`/`llvm-objdump --macho -p`
  (or `ipsw`), especially `@rpath` and `LC_RPATH`; compare with a daemon that
  loads the same framework successfully.
- If `DYLD_PRINT_LIBRARIES` prints nothing for the widget, the `DYLD_*` variables
  were stripped (restricted/sandboxed process) or dyld did not reach the print;
  then use the frame evidence and inspect the appex's dependency list statically.

## 5. Judgment and next fix

- `Diagnostics::error` + `Loader::getLoader` + repeated path variants =
  **missing/invalid dependent dylib**, not stat/sandbox I/O;
- fix the image path/rpath/closure (put the dylib where `@rpath` resolves, or
  correct `LC_RPATH`), not `stat64`;
- do not add a sandbox exemption for this: there is no evidence the stat itself
  blocks;
- if the missing library is a private framework that exists only in the dyld
  shared cache, check that the appex's expected cache/closure is the one mapped
  (the `PrebuiltLoader` frames in WorldClock are the same theme).

`DYLD_PRINT_LIBRARIES`/`SEARCHING` is a one-boot, no-kernel-change experiment.
