# Continuous integration

The workflow lives where GitHub looks for it: `.github/workflows/linux.yml`. This directory holds
the parts that are not a workflow.

| File | What |
|---|---|
| `.github/workflows/linux.yml` | The merge gate: `provenance` (+ secret scan, + doc links), `test-open`, `style` |
| `.github/workflows/upstream-boot.yml` | Boots an open-source XNU under QEMU and asserts the markers. Weekly at a pinned upstream commit, plus on demand — separate from the gate so an external repository cannot break a pull request |
| `ci/mac.md` | The Mac-only manual step for `guest/metal-plugin` (no CI job: it needs the iPhoneOS SDK) |

Tier 1 (`provenance`, `test-open`, `style`) is the merge gate and must pass with **no Apple file,
no QEMU fork and no GPU present**. A contributor without an IPSW has to be able to stay green.
