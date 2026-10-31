# The Mac step

Exactly one part of IOSVM needs a Mac: `guest/metal-plugin`, which is Objective-C compiled against
the iPhoneOS SDK. Everything else — unpacking an IPSW, building the kernel, building QEMU, running
the VM, the whole host side — is Linux (ADR-009).

There is deliberately **no CI job** for this. A CI runner cannot have the SDK, and a build that
cannot be reproduced in public is not a build this project can gate on.

## What you need

- macOS with Xcode 16 or newer (for the iPhoneOS 18+ SDK)
- the `iphoneos` SDK, plus `ldid` or an ad-hoc signing identity
- the guest image you built on Linux, mounted or copied somewhere reachable

## Build

```sh
# on the Mac, in a checkout of this repository
xcodebuild -showsdks | grep iphoneos
make guest SDK=iphoneos ABI=arm64e
# produces build/guest/metal-plugin/libmetal-plugin.dylib
```

Then, on the Linux side, inject what you built into your own image. **The injection tool is M2 work
and does not exist yet** — until it lands, mount your own `userland/system.apfs` with any APFS
tool and copy the dylib into it. Whatever you do, do not commit the result.

## Rules that do not bend for the Mac step

1. **The `.dylib` is never committed.** It is a build product, and `*.dylib` is git-ignored.
2. **No Apple SDK header is copied into this repository.** `guest/` may `#import`
   `<Metal/Metal.h>` at build time; it may not vendor it. If a header is needed, it is declared
   minimally by hand, or the build fails — that is the correct outcome.
3. **The plugin is signed with an ad-hoc identity, never a real one.** It is not pretending to be
   from Apple, and it is not installing anything system-wide.
4. **A change to the plugin is still reviewed like any other change.** Being unbuildable in CI
   does not make it unreviewable: keep the diff small and explain how you verified it in the PR.
5. **The plugin's interface is `proto/`, not the dylib.** Anything the plugin does to the wire
   format is reviewable on Linux against `tests/fake-gpu/`.

## What this costs the project

Risk R5: a contributor without a Mac can do M0, M1 and most of M2, but not finish M2's guest side.
That is a real split. It is mitigated by keeping the plugin's interface (the serialised command
stream) in `proto/`, which **is** testable on Linux against `tests/fake-gpu/`. Work on the plugin
can therefore be validated by someone who cannot compile it.
