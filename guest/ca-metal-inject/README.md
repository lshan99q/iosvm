# guest/ca-metal-inject — the CoreAnimation-on-Metal boot instrumentation

Status: experimental, same branch as `guest/metal-plugin/`. This directory is not part of the
delivered port. It exists because CoreAnimation's Metal renderer stops before it hands anything to
the plug-in: `CA::OGL::new_metal_context()` first asserts that
`-[NSBundle URLForResource:withExtension:]` answered a non-nil URL for
`QuartzCore.framework/defaults.metallib` (`metallib_url != nil`, `ogl-metal.mm:3308`). Nothing of
ours is called before that assert, so the only way to find the lookup and get past it is a library
inserted into `backboardd` itself.

The full static and experimental record is in
[`docs/research/ca-metallib-url.md`](../../docs/research/ca-metallib-url.md) and
[`docs/stage30-ca-metal.md`](../../docs/stage30-ca-metal.md).

## What it does

- Hooks `-[NSBundle URLForResource:withExtension:]`, its three- and four-argument variants,
  `-pathForResource:ofType:` (and the `inDirectory:` variant), and interposes the CFBundle
  equivalents. Every Metal-named lookup and every nil lookup from the QuartzCore bundle is printed
  with its bundle path and answer. The two-argument method is the one QuartzCore calls; Foundation
  funnels it into the three-argument one, so both are hooked.
- When QuarzCore's `default` / `metallib` lookup would answer nil, it returns
  `/redwood/share/ca-default.metallib` instead — a 64-byte file of ours (an `MTLB`-looking header
  and nothing else), written into the root image by `install.sh`. The content is never parsed:
  once the plug-in overrides `-[MTLDevice newLibraryWithURL:error:]`, CoreAnimation hands the URL
  to the plug-in and the plug-in answers a placeholder library.
- Logs a fault register census over the previous `SIGSEGV`/`SIGBUS`/`SIGABRT` handler: pc, lr,
  x0–x3, the selector in x1 when it is one, and a short un-slid frame-pointer walk. This is how
  Apple-inherited methods that were about to crash on a stub object were named (`dealloc`,
  `setDisableRunTimeCompilation:`, ...).
- Optional diagnostic shims, enabled only while `/redwood/etc/ca-inject-shims` exists in the root
  image: a no-op replacement for `-[_MTLPipelineLibrary setDisableRunTimeCompilation:]` and
  no-op replacements for the `-dealloc` of the Metal private base classes. They were used to find
  the walls; the plug-in now implements both itself, so the shims should stay off (no file) for
  any result that claims the plug-in works on its own.

## Build and install

Both use the HakiOS cross toolchain read-only, like `guest/metal-plugin/build.sh`:

```sh
guest/ca-metal-inject/build.sh                 # build/libcainject.dylib
guest/ca-metal-inject/install.sh on            # into $MT_WORK/staging (sudo; default mt-work)
tools/metalwire/prepare-guest.sh ui            # rebuild the root image with it
MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=3" tools/metalwire/run-guest.sh ca-metal3 300
```

`install.sh off` removes the injection line again; `prepare-guest.sh` then rebuilds the image
without it. The boot-arg level `metal=3` is what shows the device to the window server; with
`metal=1` the plug-in is loaded but `backboardd` is not shown the device and CoreAnimation keeps
its OpenGL ES path, which is the delivered behaviour.

Environment: `MT_WORK` (default `/home/lshan/DSH-desktop/mt-work`), `HAKIOS_DIR` (the read-only
HakiOS checkout with the toolchain).

## Provenance

`ca_inject.m`, `build.sh` and `install.sh` are written here; the placeholder file is generated at
install time and is not tracked. No Apple file is copied or redistributed: the URL returned points
at our own placeholder, and the real `QuartzCore.framework/default.metallib` (absent from the
extracted image anyway) is never read.
