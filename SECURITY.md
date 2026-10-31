# Security policy

IOSVM runs an operating system you did not write, on a virtual machine, with a device model that
moves commands between two privilege domains. That is a large attack surface, and some of the bugs
in it are security bugs rather than build bugs.

## Supported versions

Only the latest tagged release and `main` are supported. This project has no LTS and no backport
policy — it is a research project with best-effort maintenance.

## Reporting

**Do not open a public issue, and do not paste a reproducer into a chat.**

Use GitHub's private vulnerability reporting: `Security` tab → `Report a vulnerability`. If that is
unavailable, mail the maintainer address in `MAINTAINERS.md`.

Include: the version or commit, the host (distro, QEMU version, Vulkan driver), what you expected,
what happened, and the smallest input that shows it. Rotate any credential you had to use to
reproduce before you send the report.

You can expect an acknowledgement within 7 days and a decision within 30. Nothing here is
guaranteed by contract.

## In scope

| | |
|---|---|
| QEMU device misuse | guest → host memory corruption, out-of-bounds in `hw/`, descriptor parsing |
| The transport protocol | deserialisation bugs, ring desynchronisation, unknown-opcode handling |
| The host backend | command replay that lets a guest command reach host memory it should not |
| Credential exposure | any secret reachable from the tree or history |
| Supply chain | a submodule, dependency or Action that is not what it claims |

## Out of scope

- Anything requiring Apple's activation servers, Secure Enclave or FairPlay keys.
- The security of the iOS user space itself, which is Apple's to fix — and which the contributor
  supplies, so we cannot patch it.
- Escaping the VM through a host kernel or GPU driver bug. Report those upstream; they are not
  ours. (They are, however, a good reason the guest is treated as hostile in our own code.)
- Denial of service against a single-user local VM: the guest can crash its own VM by design.

## What we will not do

We will not add a "fix" that makes the guest claim to be a real device in order to satisfy a
remote check. If a vulnerability report's remedy is an authenticity bypass, it is not a remedy
(ADR-006, `PROVENANCE.md`).
