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
