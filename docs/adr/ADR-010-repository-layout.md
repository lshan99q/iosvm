# ADR-010: one repository per component, pinned by the main one

- Status: Accepted
- Date: 2026-10-31
- Supersedes: —
- Superseded by: —

## Background

IOSVM is five things that share a wire format and nothing else:

| Component | Language | What it needs to build and test |
|---|---|---|
| the protocol | C11 | a C compiler |
| the host backend | C++20 | Vulkan headers + a driver (lavapipe is enough) |
| the guest plugin | ObjC | the iPhoneOS SDK, i.e. a Mac |
| the QEMU device | C | the QEMU fork, i.e. ~40 minutes of build |
| the kernel patches | patch/C | XNU sources and a Darwin toolchain |

A single repository holding all five would mean every contributor clones the largest thing to work
on the smallest, and every CI job installs every toolchain. The QEMU fork alone is 40 MB; a
`guest/` tree needs a Mac before it compiles at all.

There is also a boundary argument. The protocol is the **contract** between two ends that must not
import each other's internals. If `proto/` lives inside the same build as the backend, the first
"just include this internal header" shortcut is a compile flag away, and the boundary erodes
quietly. Separate repositories make each shortcut a deliberate act.

## Decision

One repository per component, with the main repository as the coordination point:

```
iosvm                  docs, ADRs, tools, gates, the plan. Pins the components.
├── proto/             submodule → iosvm-proto      (C11, no deps, no GPU)
│   └── (host/vkbridge) submodule → iosvm-vkbridge  (C++20 + Vulkan)
├── hw/                the QEMU device — stays here until the fork exists
└── guest/             the Metal plugin — stays here until it needs the Mac split
```

Rules:

1. **The protocol versions independently.** `IOSVM_PROTO_VERSION` is the contract; a backend states
   which protocol versions it speaks, and a mismatch is rejected, not coerced.
2. **The main repository pins each component by commit** (submodule gitlink), and bumps are their
   own pull request — the same rule the design baseline wrote for the QEMU submodule.
3. **Each component carries its own gates** and its own `.gitignore`, so it can be cloned and
   tested alone. A component that cannot be tested alone is a smell.
4. **A component may not be added to the main repository's build by path reference.** If the
   submodule is missing, targets fail with a message telling you to fetch it, not silently.
5. **This ADR does not license a repo explosion.** A new repository needs a different toolchain or
   a different trust boundary. Otherwise it is a directory.

## Options weighed

| Option | Good | Bad |
|---|---|---|
| One monorepo | one clone, atomic cross-cutting changes, one CI | every contributor pays for every toolchain; boundary erodes; 200 MB+ clone |
| One repo per component (chosen) | small clones, honest gates, enforced boundary, independent versioning | cross-repo change needs two PRs; pins must be maintained; "build everything" needs orchestration |
| Two repos: "open" and "needs Apple" | the important split only | the protocol ends up bundled with one end of it, which is exactly the boundary that matters |

## Consequences

**Positive**

- A contributor with only a C compiler can improve the protocol and run its full test suite.
- A contributor with a GPU but no Mac can work on the backend.
- CI per component is minutes, not tens of minutes, and needs no toolchain it does not use.

**Negative**

- A change that touches the wire format needs a coordinated pair of PRs and a pin bump. The protocol
  version exists precisely so this is visible: bump `IOSVM_PROTO_VERSION`, land the protocol, then
  move the backends.
- The main repository can be cloned without its components and some targets then fail. That is
  intended, and the failure message says what to run — but it is still friction, and it is why
  `make help` marks which targets need a submodule.
- Three `.gitignore`s, three CIs, three READMEs to keep consistent. Accepted: they are small, and
  drift is caught by the main repository's `tools/check-links.py` when it cites them.

## Evidence

- The component split was exercised before it was decided: the protocol and the backend were
  written independently, each buildable and testable with no Apple file and no QEMU, and each
  produced a passing test suite on this host (lavapipe for the backend). Neither needed the other
  to compile.
- The boundary cost is real and was paid once already: the backend's probe validates blit results
  against its own buffers, without knowing that a Metal translator will one day feed it. That is
  the interface working as designed.
- **Not yet tested:** the pin-bump workflow, because there is only one pin so far. It is recorded
  here as the thing to watch, not as a solved problem.

## Follow-up

- `tools/sync-components.sh` (M2): fetch and check out the pinned components, and report a
  component whose `main` is ahead of the pin.
- The main repository's CI gains a job that builds each component at the pin once per day, so a
  pin that no longer compiles is noticed without a PR.
