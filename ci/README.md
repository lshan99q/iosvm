# Continuous integration

The workflow lives where GitHub looks for it: `.github/workflows/linux.yml`. This directory holds
the parts that are not a workflow.

| File | What |
|---|---|
| `.github/workflows/linux.yml` | The Linux tiers: `provenance`, `test-open`, `style`, and the optional `vkbridge-fake-gpu` |
| `ci/mac.md` | The Mac-only manual step for `guest/metal-plugin` (no CI job: it needs the iPhoneOS SDK) |

Tier 1 (`provenance`, `test-open`, `style`) is the merge gate and must pass with **no Apple file,
no QEMU fork and no GPU present**. A contributor without an IPSW has to be able to stay green.
