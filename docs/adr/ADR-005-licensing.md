# ADR-005: licensing — MIT here, GPL-2.0 for the fork, APSL-2.0 for XNU patches

- Status: Accepted
- Date: 2026-10-31
- Supersedes: —
- Superseded by: —

## Background

This project stands on three codebases with three incompatible-in-detail licenses, and consumes a
fourth (Apple's user space) that we must never redistribute.

| Component | Whose | License |
|---|---|---|
| QEMU fork (`qemu/`) | upstream QEMU + us | GPL-2.0 |
| XNU, patched (`xnu/*.patch`) | Apple | APSL-2.0 |
| our `hw/`, `proto/`, `host/`, `guest/`, `tools/` | us | we choose |
| iOS user space | Apple | not redistributable |
| borrowed translation tables (UniXNU, metal2vulkan) | others | BSD-2-Clause-Patent / MIT |

## Decision

1. **Our original code is MIT** (`LICENSE`). It is the least restrictive option that lets the
   widest set of people reuse the protocol and the backend, including in closed products. Nothing
   in our tree is improved by copyleft: the interesting parts are the protocol and the translation
   tables, and a fork war over those helps nobody.
2. **The QEMU fork stays GPL-2.0** (`LICENSE.GPL2`). It is a separate work, distributed as a
   submodule, and we do not link it into our MIT code as a single program.
3. **XNU patches are APSL-2.0** (`LICENSE.APSL`). We ship them as **patches against a pinned
   upstream release**, never as a copy of the tree: a patch is unambiguously a modification
   published in source form, which is exactly what APSL-2.0 §3 requires. We do not ship a binary
   without the patches.
4. **No Apple file is ever redistributed.** Not the kernel, not the user space, not fonts, not the
   dyld cache. `PROVENANCE.md` is the contract and `tools/provenance-check.py` is the enforcement.
5. **Each borrowed file carries its original header and a row in `NOTICE`.** No relicensing by
   omission.

## Consequences

**Positive**

- A single, simple license for the code most people would want to read.
- The GPL/APSL obligations are confined to two clearly-labelled subtrees, so a reviewer can audit
  them separately.

**Negative**

- The combination is not uniformly permissive: a downstream user who wants the QEMU fork must
  accept GPL-2.0 for it. That is inherent to forking QEMU, not a choice we can avoid.
- APSL-2.0's source-publication duty means we must keep patches readable and applied to a named
  upstream tag. Losing track of which release a patch applies to would be a license compliance
  bug, not a build bug.

## Follow-up

- `xnu/` contains only `*.patch`, plus a `README.md` naming the exact upstream tag each applies to.
- `tools/check-licenses.py` (M1): verify that every file under `qemu/` keeps its original header,
  and that no file under `xnu/` is anything other than a patch.
