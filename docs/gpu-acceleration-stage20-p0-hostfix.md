# Stage 20 P0 hostfix — new host b1e87d + system 6be + root 3207a35e

**Verdict: host fix works — f-picture is content again (no mask); P0 diff
89.50%, still <95; the remaining 9.6% is the system 6be494f1 (8bf5f838 not on
disk).  So the P0-3 baseline (98.82%) is only reproducible on the historical
image, not on the current system.**

## Recipe

- root `/tmp/root-byref.apfs` **3207a35e** (P0-3's; old lib inside);
- system **6be494f1**; host **b1e87d8320d0fea56f5dae0393e4120f** (119,696 B,
  startup `poll -1 ms, infinite = wait for guest` — default path restored);
- `XCMD="glwire_local=1"`; P0-3 HOST_ENV (NO_PANEL; DUMP_AT 800; PAIR 1);
- `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=2400 UI_GRACE=300 MINSHOT=200000`.

## Evidence

- `ui=660s (313,333 B)`.
- f-panel `5819700f…` (49.97% non-black); f-picture `4483abd6…` **40.58%
  non-black, content** (the recheck's was 0.00% black).
- check_panel f: no mask; pairings f-panel vs f-picture **89.50%**; f-n1-panel
  vs f-picture **100.00%** → at least one valid pair.
- diff f: missing 116,836 (9.59%), box 46,142–729,1603; extra 2,489 (0.20%).
- read-back lines present (`last drawn into 2` / `reading from 2 instead`).

## P0 three

- screenshot ≥300 K ✓ (313,333 B).
- same-frame diff **89.50% <95 → FAIL**; this is the system 6be's
  SpringBoard/content difference, not a host/render artifact (host fix restored
  the picture).
- names: screenshot/f-panel full home screen (visual check needed; label dumps
  in the archive).

## Conclusion / handoff

- Host `2be814a7` f-picture black was the 59.57 artifact; `b1e87d` fixes it.
- The current system `6be494f1` does **not** reproduce the P0-3 98.82%:
  89.45–89.50% is the ceiling on the current system; the P0-3 baseline only the
  historical `8bf5f838` (not on disk) can reproduce.  Update the handoff/P0
  acceptance accordingly (or rebuild the 8bf system if its inputs are known).

Artifacts: `/host/iosvm/logs/s20-p0-hostfix/`.
