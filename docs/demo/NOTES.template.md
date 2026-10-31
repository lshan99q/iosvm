# <milestone> acceptance capture

Copy this file next to every capture you commit under `docs/demo/`, named NOTES.md, and fill it
in. A capture without one is not evidence — it is a picture.

- **Milestone**: M0 / M1 / M2 / M3 / M4
- **Commit**: <the exact commit the capture was taken at>
- **Guest firmware**: <IPSW version and device identifier, e.g. `Darwin 16.5, 20F66, iPhone10,3`>; and the
  `firmware/` SHA256 prefix from `.provenance`, e.g. `a1b2c3d4e5f6`
- **Host**: <distro, CPU, RAM, Vulkan driver version (`vulkaninfo --summary` first line)>
- **Command**: <the exact command that produced it>
- **What it shows**: <one sentence, and what a reviewer should look at>
- **What it does not show**: <be specific — a frame is not a working desktop>
- **Reproduced**: <how many consecutive runs produced the same result>

Rule from `PROVENANCE.md`: captures are the only binaries allowed in the tree. Do not add a
screenshot that contains a serial number, an Apple ID, a token or anything else personal.
