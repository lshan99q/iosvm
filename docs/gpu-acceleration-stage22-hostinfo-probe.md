# S22 host_info guest probe and hostport MIG trace

Status: sources + build/injection instructions; no boot.

## 1. Probe

Repo source: `guest/hostinfo_probe.m` (plain C in an `.m` file, matching the
`$DYN` example).  The worktree's actual userland tree does not carry this repo's
`guest/` directory, so copy it there for the build:

```sh
cd /host/iosvm-work/hakios-xnu-kernel/userland
cp <repo>/guest/hostinfo_probe.m src/hostinfo_probe.m
```

It does exactly what the failing extension does, but from a plain executable:

- `mach_host_self()` and `MACH_PORT_VALID`;
- header-only `mach_msg` to the host port with a **1 s send timeout** (no reply right);
- worker thread calls `host_info(mach_host_self(), HOST_BASIC_INFO, ...)`;
- main thread waits on a dispatch semaphore for **5 s**;
- prints `OK ncpu=%d mem=%llu`, `BLOCKED after 5s`, or `KR=%d`.

Build with the existing `$DYN` line from `userland/build.sh` (verified syntax against
the real `theos` iPhoneOS16.5 SDK):

```sh
DYN='xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1'
$DYN -o obj/hostinfo_probe src/hostinfo_probe.m
```

The one-off path uses `userland/build.sh`'s existing `EXTRA`/`RC_JOBS` hooks,
without touching `mkapfs`:

```sh
cd /host/iosvm-work/hakios-xnu-kernel/userland
EXTRA="obj/hostinfo_probe" RC_JOBS=<repo>/guest/rc.jobs.hostinfo ./build.sh root.apfs
```

`EXTRA` copies the file into `root/bin/` (APFS `redwood/bin/`); `RC_JOBS` is copied
to `root/etc/rc.jobs` and is run once by the `com.hakios.demo` job.  The small root
image is mounted at **`/redwood`**, so the in-image path is
**`/redwood/bin/hostinfo_probe`** (not `/bin`).

A prebuilt arm64 binary (49K, `/usr/lib/libSystem.B.dylib`, iOS 16.5) is available at:

```
/host/iosvm-work/hakios-xnu-kernel/userland/obj/hostinfo_probe
/host/iosvm/logs/s22-ext/probe-bin/hostinfo_probe
```

so a container without clang can use it directly:

```sh
EXTRA="/host/iosvm-work/hakios-xnu-kernel/userland/obj/hostinfo_probe" RC_JOBS=<repo>/guest/rc.jobs.hostinfo   ./build.sh root.apfs
```

`guest/rc.jobs.hostinfo` is the one-line job file:

```sh
/redwood/bin/hostinfo_probe >>/dev/console 2>&1
```

### Expected serial

```
hostinfo: mach_host_self port=<n> valid=1
hostinfo: empty send kr=0            # or MACH_SEND_TIMED_OUT/INVALID_DEST
hostinfo: OK ncpu=1 mem=12884901888  # max_mem; memory_size is capped at 2 GB
```

or, the decisive result:

```
hostinfo: BLOCKED after 5s
```

or the error path:

```
hostinfo: KR=-<n>
```

If the probe blocks too, the host port/MIG server is broken **globally**, not
extension-specific.  If it returns, the extension is special (sandbox/persona/
entitlement/early initialiser).

## 2. Optional kernel hostport trace

Patch: `patches/0011-hostport-mig-trace.patch`, applied to
`osfmk/kern/ipc_kobject.c`:

- **include only `<pexpert/pexpert.h>`**; this tree has no `kern/printf.h`
  (kprintf's prototype comes from pexpert), so do not add that include;
- `hakios_hostport_trace=1` (default off) prints
  `hakios: hostport msg id=%d port=%p` at the MIG/kobject dispatch entry,
  before `find_mig_hash_entry()`;
- every kobject message is logged; a host_info request is `msgh_id=200`, so
  `id=200` is the one to look for;
- no trace at all while the probe/extension is blocked means the request never
  reached the kobject server (host port/queue path);
- `id=200` appears but `kern/host.c host_info()` still has no trace means the
  dispatch/handler path is the problem.

Apply the patch and rebuild with the tree's one-command wrapper:

```sh
cd /host/iosvm-work/hakios-xnu-kernel
git -C xnu-src/xnu-8796.121.2 apply <repo>/patches/0011-hostport-mig-trace.patch
./tools/kernel-rebuild.sh "hakios: host_info" "hakios: hostport msg"
```

`tools/kernel-rebuild.sh` clears the stale intermediate/linked outputs, links with
cctools (not `ld64.lld`), checks `__TEXT vmaddr=0xfffffff007004000` and
`entry_va != 0`, and checks the marker strings.  It must print
`kernel-rebuild: OK`; otherwise do not boot.

## 3. What each result means

| probe | kernel trace | conclusion |
|---|---|---|
| `OK` | `id=200` | host port/MIG works; extension-specific path is the problem |
| `BLOCKED` | `id=200` only once | MIG dispatch is entered but no reply/handler return |
| `BLOCKED` | **no** `id=200` | request never reaches the kobject server (port right/queue) |
| `KR=-…` | `id=200` | server replied with an error; count/flavor/port right is next |

Run the probe first; add `hakios_hostport_trace=1` to the same boot (no extra
experiment) to see whether `id=200` arrives.  Do not change timeout/job/watchdog.

## 6. probe2: order changed, empty send last

`guest/hostinfo_probe2.c` (also copied to the real
`userland/src/hostinfo_probe2.c`) fixes the first probe's death point:

1. `mach_host_self` (before/after);
2. `host_info(HOST_BASIC_INFO)` (before/after);
3. `host_info(HOST_LOAD_INFO)` (before/after);
4. `host_statistics64(HOST_VM_INFO64)` (before/after);
5. **header-only `mach_msg` to the host port, no reply** — last, so if it dies there
   the last line is `probe2: before empty send (header only, no reply)`;
6. `done`.

Every print is followed by `fflush(stdout)`, so a missing `after` names the exact
call.  The first probe had the empty send before `host_info`; ext10 died after the
`mach_host_self` line, so this order isolates the crash.

### Build (works with the SDK's `.tbd` only)

`xnu-clang`/`ld64` with cctools cannot consume `libSystem.B.tbd` (`library not
found for -lSystem`).  Use **LLD** for this guest executable; cctools is for the
kernel.  Verified here:

```sh
SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
CC=${XNU_CLANG:-xnu-clang}
LD64_LLD=${LD64_LLD:-/usr/lib/llvm-19/bin/ld64.lld}

$CC -target arm64-apple-ios16.5 -isysroot "$SDK" -O1 \
    -c src/hostinfo_probe2.c -o obj/hostinfo_probe2.o
$CC -target arm64-apple-ios16.5 -isysroot "$SDK" \
    -fuse-ld="$LD64_LLD" \
    -Wl,-platform_version,ios,16.5,16.5 -Wl,-arch,arm64 \
    -L"$SDK/usr/lib" -lSystem.B \
    -o obj/hostinfo_probe2 obj/hostinfo_probe2.o
```

- `-lSystem.B` is the SDK's `.tbd`; the driver's default `-lSystem` can fail when
  cctools is selected.
- `CCTOOLS_LD64` is the kernel linker, **not** this guest executable.
- Prebuilt binary: `/host/iosvm/logs/s22-ext/probe-bin/hostinfo_probe2`
  (49K, md5 `c88a81fa3af096598861bd377d49adee`, `LC_LOAD_DYLIB
  /usr/lib/libSystem.B.dylib`).

### Inject

```sh
EXTRA="obj/hostinfo_probe2" RC_JOBS=<repo>/guest/rc.jobs.hostinfo2 \
  ./build.sh root.apfs
```

`guest/rc.jobs.hostinfo2`:
```sh
/redwood/bin/hostinfo_probe2 >>/dev/console 2>&1
```

### Expected

```
probe2: start
probe2: before mach_host_self
probe2: after mach_host_self port=... valid=1
probe2: before host_info(HOST_BASIC_INFO)
probe2: after host_info basic kr=0 count=12 ncpu=... memory=... max_mem=...
probe2: before host_info(HOST_LOAD_INFO)
probe2: after host_info load ...
probe2: before host_statistics64
probe2: after host_statistics64 ...
probe2: before empty send (header only, no reply)
probe2: after empty send kr=...
probe2: done
```

The last printed line identifies the crashing call.  If it dies after
`before empty send`, the hand-built header message is the culprit; if it dies after
`before host_info`, the real host_info call is.
