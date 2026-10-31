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

## Evidence

- **The license texts in this repository are the canonical ones, not paraphrases.** Checked on
  2026-10-31: `LICENSE.APSL` is byte-identical to the APSL-2.0 text shipped by HakiOS
  (`19829` bytes, header "APPLE PUBLIC SOURCE LICENSE Version 2.0 - August 6, 2003"), and
  `LICENSE.GPL2` is Ubuntu's `/usr/share/common-licenses/GPL-2`. Neither was retyped.
- **The referenced projects' licenses were read from GitHub's API, not assumed:**
  `jprx/darwin-vm` MIT, `oosexgang/hakios-metal` MIT, `steelbrain/metal2vulkan` NOASSERTION
  (a LICENSE file exists but GitHub cannot classify it — treat as unverified until read),
  `UniXNU/UniXNU` BSD-2-Clause-Patent per its NOTICE, `jprx/qemu-sptm` GPL-2.0 per its README.
  The `NOASSERTION` cases are why `NOTICE` says "referenced for design, not copied": nothing from
  them is in this tree yet.
- **What is unverified:** whether APSL-2.0 §3's source-publication duty is satisfied by shipping
  patches alone. That is a legal reading of the licence, not a build fact, and it is the one item
  here a lawyer should confirm before v0.1.0 ships binaries.

## Follow-up

- `xnu/` contains only `*.patch`, plus a `README.md` naming the exact upstream tag each applies to.
- `tools/check-licenses.py` (M1, listed in `docs/phases.md`): verify that every file under `qemu/`
  keeps its original header, and that no file under `xnu/` is anything other than a patch.
