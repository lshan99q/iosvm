# Reproducing what this port shows today, from an empty shell

Everything below was run on this machine. It is the shortest path from a checkout to
`docs/demo/system-ui-first.png` -- the boot logo on the panel -- and the commands are the ones that
were actually used, not a cleaned-up version of them.

## 0. What has to be true first

| thing | why |
|---|---|
| `~/theos` with `sdks/iPhoneOS16.5.sdk` | the userland and the plug-ins link against it |
| `/tmp/cctools/bin` (cctools-port `ld64`) | XNU's link needs `-alias_list`, which `ld64.lld` lacks |
| `xnu-deps/host/bin` on `PATH` (`xnu-clang`, `ld64`, `lzfse`, `install_name_tool`) | the build scripts call them by name |
| `~/ipsw/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw` unpacked into `sysimage/` | the Darwin userland, the dyld cache, the manifest |
| 26 GB free in `/home` | a 12 GB RAM image, a 2.5 GB system image and build trees |

## 1. The kernel

```sh
cd ~/DSH-desktop/iosvm-work/hakios-xnu-kernel
export THEOS=$HOME/theos
make kernel JOBS=14            # ends with the kext symbol-set check failing, which is expected
cp xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 prebuilt/hakios-kernel
```

The four kernel patches in this repository's `patches/` are what make the panel writable and what
give CoreAnimation a renderer:

| patch | what it changes |
|---|---|
| `0001` | the display brings up the show service, and paints a band of its own at start-up |
| `0003` | the GPU service creates its user client explicitly, so the linker cannot drop it |
| `0004` | the user client starts on a machine with no GPU, which is the only machine this runs on |

## 2. Mesa

```sh
PATH=$PWD/xnu-deps/host/bin:$PATH bash tools/mk-mesa.sh
# ends with: Mesa 25.0.7 (softpipe,freedreno): .../libOSMesa.8.dylib, 14805208 bytes, 1296 gl* functions
```

`install_name_tool` has to be on `PATH`; without it the build stops at its last step with
`install_name_tool: 未找到命令` even though Mesa itself is already built.

## 3. The images

```sh
cd userland
export THEOS=$HOME/theos iOSVM_USE_LLD=1
export PATH=../xnu-deps/host/bin:/tmp/cctools/bin:$PATH

# the small image: init, our painter, Mesa, the GL stand-in, and 25 launchd jobs
GL=1 RC=rc.launchd EXTRA=/tmp/guestbin/fill_panel ./build.sh root.apfs

# the big image: the Darwin userland, the cryptex and the dyld cache
SYSTEM=1 ./build.sh system.apfs
```

Two things in this tree differ from upstream and both are needed: `build.sh` also puts the dyld
cache where dyld looks first (`/System/Library/Caches/com.apple.dyld/`, a symlink into the cryptex),
and `init.c` finds its own image by asking whether `/redwood/etc/rc` exists instead of trusting
`argv[0]`, which the kernel's first exec does not pass.

## 4. Running it

```sh
cd ~/DSH-desktop/iosvm-work/hakios-xnu-kernel
QEMU_SMP=4 QEMU_TB_MB=1024 QEMU_RAM_GB=12 \
  IMG=$PWD/userland/system.apfs ROOT=$PWD/userland/root.apfs \
  timeout 1700 bash qemu/run-darwin.sh 1620 >/tmp/run.log 2>&1 &

sleep 150
bash qemu/screendump.sh /tmp/screen.png      # 1125x2436
```

`QEMU_RAM_GB` must stay 12: the loader's `rdres` needs the whole memory window.

## 5. What to expect, and what is still missing

Expect, after two to three minutes: `successful spawn` lines from launchd, `backboardd`, `SpringBoard`,
and a screen that shows Apple's boot logo -- drawn by CoreAnimation through Mesa's `softpipe`, which
is what `docs/research/linux-kernel-build.md` addendum 12 records.

What does not happen yet: the screen does not move past that logo. The user domain stays in
on-demand-only mode (`pending spawn, domain in on-demand-only mode`), so jobs that would follow do
not run and SpringBoard's own scene never arrives. Addendum 13 has the measurements.
