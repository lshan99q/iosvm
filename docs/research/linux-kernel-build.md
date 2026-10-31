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

## The link: Apple's `-alias_list`, and how it was replaced

The final link failed with **9 undefined symbols**, and they were not missing code. XNU links with
`-Wl,-alias_list,<file>`; the file's pairs say *the plain name a caller writes is an alias of the
implementation that carries a suffix*:

```
_IOMallocData_external       _IOMallocData
_mach_vm_allocate_external   _mach_vm_allocate
_vm_fault_external           _vm_fault
```

`vm_user.o` defines `_mach_vm_allocate_external` and `_mach_vm_allocate_kernel`, and nothing else --
so without the alias list the plain name really is undefined. `ld64.lld` has no `-alias_list`.

The wrapper now implements it: it reads the file it was handed, emits one trampoline per pair
(`_mach_vm_allocate: b _mach_vm_allocate_external`), assembles it for `arm64-apple-ios16.5`, and
appends the object to the link. The trampolines are **weak**, because a few of those names do exist
plainly in the tree and a strong duplicate would fail the link (`_task_id_token_port_name_to_task`
did exactly that).

Two more host-tool gaps surfaced behind it:

- **`strip`** was GNU binutils, which cannot read Mach-O. It is now `llvm-strip`.
- **`libtool -ca … -filelist`** was not handled by the shim; it is now, reading the file list.

And one portability bug of my own: `${var//pat/}` is a bashism and `/bin/sh` here is dash, so the
shim died with `Bad substitution` until it became `tr -d ' '`.

## What exists now, and what it still refuses

**The kernel compiles and links on Linux:** `mach.development.bcm2837`, 15,819,368 bytes,
`Mach-O 64-bit arm64 executable`. Building it needed the theos layout, the public 16.5 SDK, Apple's
open sources, and the five stand-ins above (**ld**, **libtool**, **strip**, **mig**, **tcsh**) --
none of them Apple's binaries.

**It does not boot yet.** Handed to the project's own harness (`tools/qemu-open.sh`), the loader
rejects it before the kernel runs:

```
payload entry 'kernel' size f16268
kernel va 0..1300000 entry 0
unexpected kernel layout
```

That message is the upstream `xnuloader` checking the kernel's segment layout, and `ld64.lld` lays
segments out differently from cctools' `ld` -- a known difference, not a defect in the build. The
upstream prebuilt kernel still boots (PASS, five markers), so the harness itself is fine.

So the remaining step is exactly one thing: **get cctools' `ld64` onto this host**, at which point the
wrapper stops emulating Apple's linker and uses it. That build needs libdispatch, which Ubuntu does
not package; it was built from source (`swift-corelibs-libdispatch`, with clang -- the default GCC
rejects its warning flags). The `ld64` build itself was interrupting at `libhelper.la` when driven
from a subdirectory; it has to be driven from `cctools/ld64`, and that is where it stands.
