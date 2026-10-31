# Building the arm64 kernel on a Linux host: what it takes, and where it stopped

The project needs its own kernel (drivers are added inside it), and the build must work on Linux
because that is the platform the project targets. The upstream tree builds on macOS with Xcode; this
records the Linux path, as executed here on 2026-10-31, including the four things that had to be
replaced and the one thing that is still open.

## What the build needs

| Piece | Where it came from | Note |
|---|---|---|
| `theos` | `git clone https://github.com/theos/theos` | the build only uses its layout |
| `iPhoneOS16.5.sdk` | `github.com/theos/sdks`, sparse checkout of that one directory (233 MB) | the public SDK, the same one the Linux path has always used |
| Apple open sources | `xnu-deps/setup.sh --stage1` | AvailabilityVersions, libdispatch, libpthread, mig sources, the macOS 13.3 SDK for DriverKit headers |
| the kernel tree | already present (`xnu-src/xnu-8796.121.2`) | Apple's `xnu-8796.121.2` with the HakiOS work on top |
| `libfirehose_kernel.a`, `libpthread_kext.a` | `xnu-deps/setup.sh --stage2` | compiled against the kernel's own exported headers |

## Four host tools that are Apple's on macOS and have to be stood in for on Linux

Each failed loudly, and each is the kind of thing that silently changes what gets built if it is
faked badly:

1. **`ld`** — there is no cctools on this host, but LLVM ships `ld64.lld`, which speaks the Mach-O
   command line. Two shims are needed: the binary must be invoked **as `ld64.lld`** (an `ld`
   symlink makes LLD choose its GNU personality), and it wants `-arch` and `-platform_version`
   stated, which clang only passes for a Darwin target. The wrapper supplies `-arch arm64` and
   `-platform_version ios 16.5 16.5` when absent and passes everything else through.
2. **`libtool`** — Apple's static-archive driver. A shim maps `-static -o LIB objs…` onto
   `llvm-ar rcs`. **It must be `llvm-ar`, not GNU `ar`**: GNU ar writes an ELF-style archive index,
   and `ld64.lld` then refuses with *archive has no index*.
3. **`mig`** — built by stage 1 from Apple's own `mm_bootstrap_cmds`, so the generated stubs are
   Apple's dialect rather than a reimplementation.
4. **`tcsh`** — the build calls `csh` for a few scripts; its own build failed on
   `unable to find the tgetent()` (no ncurses dev package here). Ubuntu's `tcsh` provides `csh` and
   is enough; it is linked into `xnu-deps/host/bin` rather than built.

With those in place the whole tree **compiles**: 320 objects in `osfmk`'s directory alone, 203 MB of
build products under `BUILD/obj/DEVELOPMENT_ARM64_BCM2837`.

## Where it stopped, precisely

The final link of `mach.development.bcm2837.unstripped` fails with **9 undefined symbols**:

```
_IOMallocData  _IOMallocZeroData  _mach_vm_allocate  _mach_vm_map
_mach_vm_remap  _sysctlbyname  _ubc_create_upl  _vm_fault  _vm_map_create
```

These are not "code that does not exist": `vm_fault` and `vm_map_create` are core VM functions whose
sources compile and whose objects are in the build, `IOMallocData` is IOKit's, and the `mach_vm_*`
ones are MIG server entries. Two hypotheses, both testable next:

- the MIG output here defines `__Xmach_vm_allocate_external` (local) but not `mach_vm_allocate`, so
  the host `mig` may be generating a different dialect from the one this XNU release expects;
- or the final link's object list is not picking up the subdirectories, which would be an
  `ld64.lld` versus cctools difference rather than a build-file difference.

Either way the honest state is: **the build is one step from done, and the step is understood.** The
next attempt should try cctools-port's own `ld` before chasing the symbol list, because that
removes the whole class of "LLVM's Mach-O linker is not Apple's" differences at once.
