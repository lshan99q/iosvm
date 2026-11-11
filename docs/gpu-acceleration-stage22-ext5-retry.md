# S22 ext5 retry: the actual widget p_comm is `PeopleWidget_iO`

ext5 used `ustacks=FindMyWidgetPeo`; all 120 samples said `no process called
FindMyWidgetPeo`.  The launchd log shows the spawned widget at 05:55:13 as:

```
(pid/49 [PeopleWidget_iO])  created
```

Boot started 05:43:10, so `PeopleWidget_iO` appears at ≈723 s (UI 750 s).
`BatteriesWidget` is pid 48; `PeopleWidget_iO` is pid 49.  The kernel `p_comm`
is 16 bytes, so the exact sample target is **`PeopleWidget_iO`** (15 chars),
not the bundle name or `FindMyWidgetPeo`.

## One retry (only if the budget is extended)

```sh
XCMD="glwire_local=1 launchd_disable_all_timeouts=1 \
      ustacks=PeopleWidget_iO ustacks_at=760 ustacks_ms=1000 ustacks_count=120 \
      ustacks_threads=8 ustacks_tname=*" \
HOST_ENV="GLWIRE_NO_PANEL=1" \
MINFRAMES=999999 MINSHOT=300000 UI_GRACE=250 NOUI_BY=900 MAXSECS=1000 \
  ./harness/boot-measure.sh ext5r /tmp/root-byref.apfs 1000 --no-panel
```

- 760 s is 37 s after the process's `created` (723 s) and after UI;
- 120 samples cover 760–880 s, including the first CheckIn window;
- keep `hakios_host_info_trace=1` optional; ustacks is the evidence.

Symbolize:

```sh
grep -aE "ustacks: PeopleWidget_iO|checked in as alive|uncorking exec|EXC_CORPSE" /tmp/ext5r.log
python3 tools/glwire/harness/symstack.py /tmp/ext5r.log PeopleWidget_iO
```

## Decision

| stack | conclusion |
|---|---|
| `dyld`/`libobjc`/`UIKit`/extension init | H2 earlier than host_info; stop chasing host_info |
| `_pthread_init`/`_host_info`/`mach_msg2` | same H2 point; trace 0 is an instrumentation/flag issue |
| `xpcproxy`/launchd/pre-main | H3 excluded; inspect spawn/exec |
| `xpc_main`/`bootstrap_check_in` | H3 endpoint |
| no samples/pid gone | early death |

CheckIn remains `checked in as alive` ≥1 and no `Extension is hanging` /
`CHECK_IN_TIMEOUT` for the same pid; a live pid alone is PARTIAL.

Budget note: ext4 + ext5 already used the two-boot allowance.  This retry is a
**third boot**; run it only with explicit approval, not as an automatic repeat.
