# S22 ext4: host_info trace 0, and the last-budget ustacks plan

Status: read-only analysis of `logs/s22-ext/ext4`; no boot from this document.

## 1. ext4 facts

Correct-layout kernel `4d9f32…`, 15,789,552 B; `__TEXT=0xfffffff007004000`.
`launchd_disable_all_timeouts=1` boot reached UI (`ui=840s`, guestframes 37,
hostframes 536).  15 widget INIT/running events, first widget
`com.apple.findmy.FindMyWidgetPeople` spawned **05:28:35** (boot start 05:14:39,
≈836 s), exec `uncorking` **05:31:25** (≈1006 s).

`hakios: host_info` count is **0**, while the cmdline contains
`hakios_host_info_trace=1` (269/607).  This is not just "extension didn't call it":
**every** host_info caller, including SpringBoard's libSystem pthread_init and kernel
internal `host_info` calls, should pass the wrapper.  So first decide whether:

- the wrapper is not reached at all (instrumentation/PE parse), or
- every caller is before/after the traced path, or
- the extension is stuck earlier than pthread_init's host_info.

The new kernel has `_host_info_trace` and both trace strings (`nm` shows
`_host_info` and `_host_info_trace`), so the patch is linked; the question is runtime.

## 2. ext5 (last boot): sample the live widget

Use the **first spawned widget** and its truncated kernel process name
(`FindMyWidgetPeo`; launchd prints `[FindMyWidgetPeo]`).  Spawn is ≈836 s, so sample
from **850 s**:

```sh
XCMD="glwire_local=1 launchd_disable_all_timeouts=1 \
      ustacks=FindMyWidgetPeo ustacks_at=850 ustacks_ms=1000 ustacks_count=120 \
      ustacks_threads=8 ustacks_tname=*" \
HOST_ENV="GLWIRE_NO_PANEL=1" \
MINFRAMES=999999 MINSHOT=300000 UI_GRACE=450 NOUI_BY=900 MAXSECS=1260 \
  ./harness/boot-measure.sh ext5 /tmp/root-byref.apfs 1260 --no-panel
```

(Keep `hakios_host_info_trace=1` only if the trace itself is under test; the key
sample is ustacks.  The process name is limited to the kernel's `p_comm` width,
hence `FindMyWidgetPeo`.)

Symbolize:

```sh
grep -aE "ustacks: FindMyWidgetPeo|checked in as alive|uncorking exec|EXC_CORPSE|host_info" /tmp/ext5.log
python3 tools/glwire/harness/symstack.py /tmp/ext5.log FindMyWidgetPeo
```

## 3. Decision tree

| first stack | conclusion | next |
|---|---|---|
| `dyld` / `libobjc` / `UIKit` / extension init | H2 is **earlier than host_info**; the ext3 pthread_init→host_info stack cannot be assumed for this kernel | target the earlier initializer/framework; no more host_info |
| `_pthread_init` / `_host_info` / `mach_msg2` | same H2 point as ext3; then trace 0 is an instrumentation/flag problem, not a missing call | inspect `host_info_trace`/PE parse; add per-case print |
| `xpcproxy` / launchd / pre-main | failure before libSystem init | inspect xpcproxy/exec/loader; H3 excluded |
| `xpc_main` / `bootstrap_check_in` / `xpc_connection` | H3 CheckIn endpoint | H3 is back; source trace needed |
| no samples / pid gone | process died before 850 s | inspect exit/corpse; not host_info |

CheckIn criterion (same as before): at least one
`... extension checked in as alive ...`, and no `Extension is hanging` /
`CHECK_IN_TIMEOUT` for the same pid.  A live pid alone is **PARTIAL**, not CheckIn.

## 4. Budget

ext4 used the first of two boots; ext5 is the **last**.  Do not spend it on a
host_info fix before the ustacks answer; if the stack is earlier, host_info is a red
herring for this kernel.
