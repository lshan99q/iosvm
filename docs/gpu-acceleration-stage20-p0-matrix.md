# Stage 20 P0 adjacent-pair matrix — offline from existing dumps

**Question:** can "P0 = best adjacent pairing ≥95%" be saved?  **Answer: no.**

## Matrix (panel(N+k) vs picture(N); best of both row orientations)

### logs/s20-p0-recheck (6be; f-picture/f-n1 为黑)

| panel | vs f-picture (N) | vs f-n1-picture (N+1) |
|---|---|---|
| f-panel | 59.57% | 59.57% |
| f-n1-panel | **100.00%** (both black) | **100.00%** (both black) |

- The 100% pairs are both-black (check_panel: f-n1 MASK, f-picture 0 nonblack) → **invalid**, not evidence.
- The only content panel (f-panel) vs black picture = 59.57% (row orientation doesn't change it).

### logs/s20-p0-8bf5-recheck (8bf; both content)

| panel | vs f-picture (N) | vs f-n1-picture (N+1) |
|---|---|---|
| f-panel | 89.56% | 89.10% |
| f-n1-panel | 89.10% | 89.56% |

- Best = **89.56%**, worst = 89.10%; **all <95%**.

## Conclusion

- **No valid adjacent pairing reaches ≥95%**: the only ≥95 values are black-vs-black
  masks (invalid); the content pairs max at 89.56%.
- So "P0 = best adjacent pairing ≥95%" does **not** rescue the metric.  P0 is
  downgraded to **historical evidence + content criteria** (names/screenshot/
  panel/picture non-black), with 2–3 boot statistics if a numeric gate is ever
  needed.
- Same-image 99.99 vs 89.56 is run-to-run SpringBoard content variance, not a
  pairing choice.

Artifacts: `/host/iosvm/logs/s20-p0-matrix/` (report/meta; the source dirs
`logs/s20-p0-recheck/`, `logs/s20-p0-8bf5-recheck/`).
