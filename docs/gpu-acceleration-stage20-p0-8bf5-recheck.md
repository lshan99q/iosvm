# Stage 20 P0 8bf5 recheck — same recipe, different result

**Verdict: 89.56% (<95) on the same 8bf5f838 recipe that gave 99.99% earlier.**
So the P0 ≥95% is **not reliably reproducible even on 8bf5f838**; the
SpringBoard content/layout varies between boots (missing bbox 46,142–729,1603,
the same widget/left-middle area).  Keep the historical caveat.

## Recipe

- system `8bf5f838d52e46d6c8bf77f13a5ad946`; root `/tmp/root-byref.apfs`
  `3207a35e`; host `b1e87d8320d0fea56f5dae0393e4120f`;
- `XCMD="glwire_local=1"`; P0-3 HOST_ENV (DUMP_AT 800; PAIR 1); `QEMU_SMP=1`.

## Numbers

- `ui=660s (316,377 B)`.
- f-panel `79d8bda3…` vs f-picture `20640c60…`: **89.56%** (missing
  116,925/9.60%, box 46,142–729,1603; extra 1,637/0.13%); f-n1 vs f-picture
  89.10; best 89.56 → FAIL.
- check_panel PASS (no mask); screenshots 316 K (full screen).

## Comparison

| boot | system | root | host | diff | note |
|---|---|---|---|---|---|
| P0-3 | 8bf | 3207a35e | 28f | 98.82% | historical |
| p0-repro (12:52) | 8bf | 3207a35e | b1e87d | **99.99%** | good run |
| p0-8bf5-recheck (15:42) | 8bf | 3207a35e | b1e87d | **89.56%** | bad run |
| p0-recheck (10:51) | 6be | b537 | 2be | 59.57% (f-picture black, host bug) | invalid |
| hostfix (12:01) | 6be | 3207a35e | b1e87d | 89.50% | 6be content |

- Same recipe, same files: the 99.99 vs 89.56 difference is run-to-run
  SpringBoard content (widget/layout region 46,142–729,1603), not system/lib.
- **Do not claim P0 "current reproducible"**; the 8bf is necessary but not
  sufficient.  A gate needs 2–3 boots; a single 99.99 is not enough.

Artifacts: `/host/iosvm/logs/s20-p0-8bf5-recheck/`.
