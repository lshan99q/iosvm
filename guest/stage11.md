# The guest-side additions of stage 11

The guest's own sources live in the hakios port (`userland/src/` of
`hakios-xnu-kernel`, which is not Apple's firmware and not this repository); what is here is what
the stage added, so that the two halves of `tools/glwire` can be read together.

## 1. `inject_ciq_probe.c` — the surface probe (P2'''', `libhakios.ciq.dylib`)

Copy it to `userland/src/inject_ciq_probe.c`, then three lines in `userland/build.sh`:

```
INJECT="... (the existing list) ...
/redwood/lib/libhakios.ciq.dylib backboardd SpringBoard
..."

		$DYN -dynamiclib -install_name /redwood/lib/libhakios.ciq.dylib -framework IOSurface \
			-framework QuartzCore -o obj/libhakios.ciq.dylib src/inject_ciq_probe.c

	[ -f obj/libhakios.ciq.dylib ] && cp obj/libhakios.ciq.dylib root/lib/
```

It has two `__DATA,__interpose` pairs — `CAImageQueueRegisterIOSurfaceBuffer` and
`IOSurfaceCreate` — and prints one `ciq:` line per call, with the surface's id, size, `'%.4s'` pixel
format and bytes-per-row. The measured result is in `docs/gpu-acceleration-stage11-results.md` §8:
**zero calls**, in both processes, although the kernel created 24 surfaces in the same boot. The
reason is in the images (SpringBoard imports only `_SBSystemAppMain`; backboardd imports
`IOSurfaceCreate` but no `CAImageQueue*`; QuartzCore calls its own copy with same-image `BL`s), and
the probe itself must also be loaded before dyld's chained fixups are resolved for interposition to
apply.

## 2. `inject_sb_stage11_today.patch` — the Today switch (P3'''')

A patch for `userland/src/inject_sb.c` (all additions). It hooks
`-[SBIconController _rootFolderDidChange]` and
`-[SBIconController _newTodayViewControllerWithIconManager:…]` only when a `today_*` boot argument
is given, and leaves the methods alone (`left alone` on the console) when the addresses do not
match:

| boot argument | what it does |
|---|---|
| `today_log=1` | one line per `_rootFolderDidChange`, one per Today view built, with the boot's milliseconds and the thread |
| `today_light=1` | the factory returns nil: `loadView` → search presenter → `SBFFeatherBlurView` → `MTMaterialView` never runs, the rest of `_rootFolderDidChange` stays |
| `today_off=1` | `_rootFolderDidChange` returns immediately and the factory returns nil: the Today page does not exist |

Measured (s11d vs s11e, 900 s boots): one call per boot at ~505 s; with `today_off=1` the interface
comes ~60 s earlier, frames +22 %, stalls −17 % — the criterion wanted −50 %, and the document says
where the rest of the stall time is.
