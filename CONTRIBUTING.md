# Contributing

Read `PROVENANCE.md` first. Three rules are one-strike vetoes, and a pull request that breaks one
is closed without discussion of the code:

1. **No Apple binary, image, cache, firmware, font or certificate** in the tree.
2. **No code that impersonates a real device to Apple's servers**, and no activation bypass. The
   boot path never enters recovery mode, so there is nothing to bypass — keep it that way.
3. **No credentials**, in any file, in any commit message, in any fixture.

The reasoning is in `docs/adr/ADR-006-no-credentials.md`; the enforcement is
`tools/provenance-check.py` in CI and in `.pre-commit-config.yaml`.

## What you need to work on what

| You have | You can do |
|---|---|
| a Linux box | everything except the guest Metal plugin — `make test-open` must always pass |
| Linux + the QEMU fork's build deps | `hw/`, `host/vkbridge`, the protocol |
| Linux + an IPSW you obtained yourself | everything, including booting a guest |
| Linux + a Mac | the guest Metal plugin (iPhoneOS SDK) |
| a GPU, or just lavapipe | the backend; CI covers lavapipe |

**You never need an iPhone.** You never need to upload your IPSW anywhere. If a contribution
requires someone else's Apple content to review, it cannot be merged.

## The loop

```sh
make test-open          # the gate: no Apple file, no GPU, no QEMU fork
make provenance         # just the compliance check
make style              # SPDX headers + clang-format
```

## Branches and commits

Trunk-based. `main` is the only long-lived branch; everything else lives under two weeks.

```
feat/<short-description>[-<issue>]
fix/…  docs/…  hw/…  proto/…  host/…  guest/…  refactor/…  test/…  ci/…  chore/…
spike/<name>              time-boxed, ≤ 5 days, deleted when the report lands
```

Conventional Commits, subject in English, imperative, ≤ 72 characters, no trailing period:

```
feat(proto): add IOSVM_OP_PRESENT and swapchain negotiation
```

Bodies explain **why**. One commit does one thing, and every commit builds.

Forbidden in a commit:
- binaries (unless declared in `PROVENANCE.md` in the same PR)
- anything under `firmware/` or `userland/`
- credentials, `.env`, keys
- commented-out blocks over 20 lines
- C/ObjC that `clang-format --dry-run --Werror` rejects

## Pull requests

Title in the same style as a commit. The body answers three questions: **what changed, why, how
you verified it.** Tick the compliance list. Link an issue.

Merge: squash for features and fixes (linear history), rebase only to preserve an intentional
sequence. No merge commits. Delete the branch after merging.

`LICENSE*`, `PROVENANCE.md`, `ci/`, and `docs/adr/` need a maintainer review even for a one-line
change: they are the compliance surface.

## Spikes

Unknowns are researched in a time-box, not in a branch that lives forever.

| | |
|---|---|
| Box | 3 days, 5 at most |
| Output | `docs/adr/<name>.md`: what was tried, the result, the conclusion, the next step |
| Conclusion | one of exactly three: `feasible`, `not feasible`, `needs more information: <what>` |
| Forbidden | "one more day" after the box expires — this is how research projects die |
| Code | does not go into `main` unless it has tests worth keeping |

## Code

| | |
|---|---|
| C in `hw/` | QEMU's style (`qemu/docs/devel/style.rst`), checked by `checkpatch.pl` |
| C++20 in `host/`, `proto/` | `clang-format` from `.clang-format`; `Result`/`std::expected`, no exceptions |
| ObjC in `guest/` | ARC; the same clang-format |
| Every source file | `SPDX-License-Identifier:` on the first code line (`tools/check-spdx.py`) |
| Protocol structs | fixed-width types, explicit little-endian, `#pragma pack` or explicit padding |
| Scripts | `#!/usr/bin/env bash` + `set -euo pipefail`; `--help`; idempotent; **dry-run by default** for anything that writes |
| Third-party code | a row in `NOTICE`, in the same PR |

## Documentation

`docs/` is English. ADRs are one decision per file, never edited to change a conclusion — write a
new one with `Supersedes:`. Every ADR has an **Evidence** section; "speculation" is allowed but
must be labelled speculation.

Do not write "close to native performance". Write `4.2 ms, n=100, on <host>`.

## Reporting a security issue

Do not open an issue. See `SECURITY.md`.
