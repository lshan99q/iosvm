# ADR-007: two build environments — superseded

- Status: Superseded by ADR-009
- Date: 2026-10-31
- Supersedes: — (this ADR is the baseline's ADR-007, restated here so the chain is visible)
- Superseded by: ADR-009

## What this ADR said

The design baseline proposed splitting the build across two machines: a Linux host for the
run-time and the host-side code, and a Mac for firmware extraction and for compiling anything
against the iPhoneOS SDK. It argued that `ipsw`/`hdiutil` and the SDK are Apple tooling that
cannot be obtained legally on Linux, so v1.0 would not promise a Linux-only end-to-end path.

## Why it was superseded

The premise is half wrong. **Firmware extraction does not need a Mac.** HakiOS's
`sysimage/unpack.sh` performs the whole extraction — `.dmg` members out of the IPSW, APFS
containers via 7-Zip ≥ 23, LZBITMAP-completed files via its own `unzbm` — with nothing but
`python3`, `7z` and `cc`. That was verified on this host: both tools are present and 7-Zip 23.01
reads APFS. See ADR-009 for the decision that replaced this one.

The SDK half remains true: compiling `guest/metal-plugin` against the iPhoneOS SDK needs the SDK,
which needs Apple's toolchain. ADR-009 keeps that confined to `guest/` and drops the Mac
requirement from M0 and M1 entirely.

## Evidence

- **The Mac requirement was measured, not argued.** `sysimage/unpack.sh` in the upstream HakiOS
  tree lists `python3`, `7z` (>= 23 for APFS) and `cc` as its only requirements, and this
  host satisfies all three: verified 2026-10-31 (`python3 3.12.3`, `7-Zip 23.01`, `gcc 13.3.0`).
- **The SDK half was not measured** — no Mac was available during this work. That is why it stays
  an assumption, and why `guest/` is deferred to M2 with the Mac step confined to `ci/mac.md`.
- A contributor with no Mac can verify everything above in one command: `tools/doctor.sh`.

## Kept for the record

- The *shape* of the split survives in ADR-009: a Linux tier that is the merge gate, and a
  Mac-only manual step for the guest plugin (`ci/mac.md`).
- The original text of this decision is ADR-007 in `docs/zh/engineering.md`.
