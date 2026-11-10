# Stage 20 P0 reproduction — 8bf5f838 + root 3207a35e + host b1e87d

**Verdict: P0 3/3 reproduces on the historical system `8bf5f838` at 99.99%
same-frame diff.** The current system `6be494f1` gives 89.50% — the P0 baseline
is only reproducible with `8bf5f838`; the P0-3 98.82% was the same system (this
run 99.99%, slightly cleaner).

## Recipe

- system `/tmp/system-8bf5f838.keep` = `8bf5f838d52e46d6c8bf77f13a5ad946`
  (copied into userland/system.apfs for this boot; the keep file stays);
- root `/tmp/root-byref.apfs` **3207a35e** (P0-3's);
- host `b1e87d8320d0fea56f5dae0393e4120f` (default poll -1);
- `XCMD="glwire_local=1"`; P0-3 HOST_ENV (DUMP_AT 800; PAIR 1); `QEMU_SMP=1`.

## Numbers

- `ui=690s (309,275 B)`.
- f-panel `2af9b8ae…` vs f-picture `0727d52d…`: **99.99%** (missing 0, extra 0,
  148/1,218,000 px outside 8); check_panel PASS.
- f-n1 vs f-picture 89.53 (the pair-N+1 cross; ignore).
- P0 three on 8bf5f838: screenshot 309,275 ≥300 K ✓; same-frame 99.99% ≥95% ✓;
  names — full home screen (visual in the archive; P0-3 reference ≥14).

## Two baselines (deliverable note)

| system | P0 same-frame diff | status |
|---|---|---|
| **8bf5f838** (P0-3) | **98.82%** (this run **99.99%**) | P0 baseline; 3/3 holds |
| 6be494f1 (current) | 89.50% | does NOT reproduce; P0 3/3 fails |

- **Subsequent experiments that need the P0 criterion must use `8bf5f838`**
  (keep: `/tmp/system-8bf5f838.keep`, md5 `8bf5f838…`; also
  `/tmp/system.apfs.bak-s20`).  Do not overwrite the keep file on rebuilds.
- The current system 6be stays as the boot/experiment system; the P0 gate is on
  8bf5f838 only.

Artifacts: `/host/iosvm/logs/s20-p0-repro/`.
