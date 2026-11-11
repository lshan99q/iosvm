# S22 ext24/ext25: CheckIn succeeded, but no widget content

## 1. Chain from CheckIn to pixels

1. **Extension check-in**: launchd marks the appex
   `<label>.apple-extension-service` alive.  This only proves the listener exists.
2. **Chrono asks for a timeline**: `chronod`/ChronoCore discovers widget descriptors
   and calls the extension's WidgetKit `TimelineProvider` for entries.
3. **Timeline is stored** by ChronoCore; it selects the current entry / snapshot.
4. **Renderer**: ChronoCore asks the widget renderer (WidgetRenderer/ChronoCore's
   host process) to build the SwiftUI view and render it to an IOSurface.
5. **SpringBoard/PosterBoard** places that surface in the widget slot;
   backboardd/glwire composites it.  glwire only carries the finished frames.

CheckIn is step 1.  A white slot means one of steps 2–4 did not produce a surface;
the GPU/glwire path is downstream and cannot create content by itself.

## 2. What the logs show

- ext24: 4 `extension checked in as alive` (Batteries, Home, GradientPoster,
  repeated Batteries), 0 hanging, no exits, but Weather/Calendar slots white.
- ext25: 2 check-ins (LockScreenPeople, WorldClock), 53 `WILL_SPAWN`, several
  widget execs, 0 hanging; slots still white.
- **No `WidgetRenderer` process or log line** in either log; `chronod` is running
  (pid 31), PosterBoard (pid 44) is running.
- **`chronod` repeatedly logs**:
  `Unable to find persona with type 6: 3 - No such process`.
  In XNU `persona.h`, **type 6 is `PERSONA_SYSTEM_PROXY`** (5=`PERSONA_DEFAULT`,
  7=`PERSONA_SYS_EXT`).  ChronoCore cannot get the system-proxy persona.

So step 1 succeeded; the missing step is the **Chrono/timeline/renderer** side,
most likely because `chronod` cannot set up its system-proxy persona and therefore
cannot host/launch the renderer or connect the extension's timeline service.
This is stronger evidence than the absence of a renderer log, which alone could
just be logging level.

## 3. Minimal observations

- `launchd_log_serial=1` (or grep the existing launchd log) for
  `WidgetRenderer`, `com.apple.WidgetRenderer`, `timeline`, `persona`, and
  `extensionkitservice`.  ext25 already has
  `Service stub initialized for com.apple.extensionkitservice`; check whether a
  renderer service stub is created at all.
- `ps -A` / launchd process list for `WidgetRenderer`, `ChronoCore`, `chronod`.
- `applog_name=chronod` (plus `applog=1`) to get ChronoCore os_log lines about
  timeline/snapshot; `chronod` is the process that knows whether the extension's
  timeline was requested.
- Extension side after CheckIn: sample the extension pid and look for
  `xpc_main`/`xpc_connection`/WidgetKit timeline frames.  If it is only parked in
  its listener, Chrono never asked.
- Kernel persona trace (safe, no user strings):
  `printf("hakios: persona_find pid=%d type=%d ret=%d\n", ...)` in
  `persona_find_by_type`/`persona_find_all`; type 6 should appear for `chronod`.
- glwire is not the first suspect: check whether any widget IOSurface exists
  (`surftrace`, `GLWIRE_DUMP`) before blaming the host uploader.

## 4. UI end condition in ext25

`boot-measure.sh` treats `UI_GRACE` as **seconds after the first qualifying UI
screenshot**, not total runtime:

```sh
if UI_GRACE > 0 && ui_at > 0 && tick*10 >= ui_at + UI_GRACE; then end=ui
```

ext25's log header says `ui_grace=400`; the end is 570 s, i.e. the UI first
qualified around 170 s and the run ended 400 s later.  The `ui=570` field printed
at the end is the current tick, not the first UI time.  With `MAXSECS=2400` this
still ends at 570, so the cap never matters.

To actually run the full window, disable both early exits:

```sh
UI_GRACE=0 MINFRAMES=999999 MINSHOT=300000 \
  qemu/boot-measure.sh ext26 /tmp/root-w15.apfs 2400 --no-panel
```

- `UI_GRACE=0` disables the UI-grace break;
- `MINFRAMES=999999` disables the frame-count acceptance;
- `MINSHOT` is still used to detect/report UI, but does not break;
- the loop then runs to `MAXSECS=2400` unless QEMU exits/panics.
- If a fixed post-UI observation is wanted, use a large `UI_GRACE` (e.g. 600)
  and make `MAXSECS` larger than `UI + UI_GRACE`; do not use 0 and expect a
  UI-triggered end.

The script's own default is `UI_GRACE=0`; ext25 explicitly set 400, so "570"
is not a default value but first-UI + 400.
