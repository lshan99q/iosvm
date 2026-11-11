# ext3 symbolized: extension is stuck in libSystem initializers (H2)

Raw log: `/host/iosvm/logs/s22-ext/ext3/serial.log` (PodcastsWidget[45], slide
`0x3b244000`).  Full symbolized output: `docs/s22-ext-03-symbolized.txt`.
Tool: `tools/glwire/harness/symstack.py` (copy of `qemu/symstack.py`, fixed to accept
the containing-symbol line from `ipsw dyld a2s`).

## Representative stacks

First sample (688.747 s):

```
_task_self_trap + 8
___libkernel_init + 52
_libSystem_initializer + 72
dyld: findAndRunAllInitializers block_invoke + 152
dyld: MachOAnalyzer::forEachInitializer block_invoke_4 + 332
...
dyld: Loader::findAndRunAllInitializers + 392
dyld: PrebuiltLoader::runInitializers + 40
dyld: APIs::runAllInitializersForMain + 72
dyld4::prepare + 2872
start + 1760
```

Every later sample (689.752 s onward, all 90 samples):

```
_mach_msg2_trap + 8
_host_info + 228
___pthread_init + 164
_libSystem_initializer + 132
dyld: findAndRunAllInitializers block_invoke + 152
...
start + 1760
```

Key address mapping (unslid = address - 0x3b244000):

| runtime address | unslid | symbol |
|---|---|---|
| `0x1fca96ff0` | `0x1c1852ff0` | `_mach_msg2_trap + 8` |
| `0x1fcaa8ad8` | `0x1c1864ad8` | `_host_info + 228` |
| `0x1fca9f0e0` | `0x1c185b0e0` | `___pthread_init + 164` |
| `0x20bf1b160` | `0x1d0cd7160` | `_libSystem_initializer + 132` |
| `0x20bfaa750` | `0x1d0d66750` | dyld `findAndRunAllInitializers` block_invoke + 152 |
| `0x1df82b42c` | `0x1a45e742c` | dyld `MachOAnalyzer::forEachInitializer` block_invoke_4 + 332 |
| `0x1fca96f18` | `0x1c1852f18` | `_task_self_trap + 8` (first sample only) |
| `0x1fcaa1830` | `0x1c185d830` | `___libkernel_init + 52` |
| `0x1fcaa1c20` | `0x1c185dc20` | `_libSystem_initializer + 72` |

## Conclusion: H2, not H3

The live extension never reaches `main`, XPC, or CheckIn.  It is inside dyld's initializer
walk, in `libSystem_initializer`, and after the first second it sits in
`___pthread_init` → `_host_info` → `_mach_msg2_trap`.  The `mach_msg` frame is **inside
pthread's host_info call**, not an XPC check-in wait; there are no `xpc_main`,
`bootstrap_check_in`, or `xpc_connection` frames.  So:

- H1: partially supported (the 5 s watchdog is the killer when timeouts are on);
- **H2: confirmed** — the extension is alive but wedged in libSystem startup;
- H3: excluded for this stack.

The immediate source target is `libsystem_pthread`'s `pthread_init` / `host_info` call (and
the kernel's `host_info` Mach trap it waits in), not launchd's CheckIn endpoint.  This is a
source/rebuild fix, not another boot-arg; the 3-boot budget is exhausted.
