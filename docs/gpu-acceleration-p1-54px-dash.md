# P1 reopen — 54px dashed line: tile content or sampling?

**Verdict: A — tile content (guest generation / row mapping), not sampling
magnification (B).**

The 90-high label tiles' bottom rows carry a near-opaque (alpha 241–253) dashed
line; the screen band matches those rows; no case was found where the tile row is
empty while the screen still shows the band.

## Boot

- 2026-11-10T05:12:08Z → 05:28:59Z; UI at **750 s / 309,171 B**; screenshot 319 K.
- `system=6be494f1` (C1 baseline), root `3207a35e`, `XCMD=glwire_local=1`,
  `GLWIRE_NO_PANEL=1`, `GLWIRE_TEXT_DRAW_LINE=1`,
  `GLWIRE_LABEL_DUMP`, `GLWIRE_TILE_DUMP`, `GLWIRE_DUMP_AT=800`.
- The first attempt with `GLWIRE_ATLAS_DUMP` + `GLWIRE_TILE_DUMP` and no
  `GLWIRE_LABEL_DUMP` stalled the host at frame 2; it was stopped and kept as
  `s20-p1-atlas-tile-stall*`.  The valid run is the tile/label/text boot above.

## Tile rows (alpha; RGB non-zero in lockstep)

| tile / source | row | alpha non-zero | alpha max | note |
|---|---|---|---|---|
| **Photos** (P0-3 `tile-f121-t9-178x90`) | 61 | **54** | 247 | the established 54 px dash; rows 58–60/62 = 0 |
| **FaceTime** (s20 `tile-f171-t9-178x90`) | 61/62 | **17 / 30** | 241 / 253 | dash spans two rows, 47 px total; 58–60 = 0 |
| **Wallet** (`label-label-2`, x=0 0,0 178x90) | 58–61 | **42/32/32/40** | 246 | dash/text bottom at 58–61; 62 = 0 |
| label-4 second label (y0=63) | 53–56 | **6/6/6/6** | 255/61 | dash at tile rows 53–56; 57–62 = 0 |
| **Weather screen** (shot 1.5x; label 113,433) | 61 | shot y 740/745: 46/48 bright, x 210–390 = **120 guest px** | — | corresponds to tile row 61 (guest y 494 → shot 741) |

- The 54 px case (Photos) is exactly the reported case; FaceTime 47 px, Wallet
  40 px, Weather screen 120 px — the band width follows the label text width.
- The rows are near-opaque, not empty: **B is excluded**.

## Crops

- `logs/s20-p1-54px-dash/shot-label-plus-band.png` — Weather + lower dash band +
  next icon row (2x from 1125x2436).
- `.../shot-label-lower40.png` — Weather lower 40 guest px (60 shot px).
- `.../shot-under-label40.png` — 40 guest px under the label.
- `.../tile-f171-alpha-4x.png` (FaceTime), `.../tile-f121-alpha-4x.png` (Photos),
  `.../label-2-alpha.png` (Wallet).

## A/B and nuance

- **A**: the dashes are written into the label tile's bottom rows.  The screen
  band is the tile content, not a linear-filter magnification of an empty row.
- Nuance: the row is not **178 px full-width**; it is a dash band of the label's
  text width (40–120 px).  In the A wording, "full width / near-opaque" should be
  read as the **full width of the dash band**, not of the whole 178 px tile.  The
  row is not empty, so B does not hold.
- Next (per A): inspect the **guest glyph/label tile generation** side — the
  130x42 glyph texture's last row / dots written into the 178x90 label tile;
  check row/stride/boundary copy.  Not the host sampler.

## Weather / Notes

- The Weather label crop is in hand (above); its own 178x90 tile was not among
  the first four label FBO dumps (those were Wallet and another label, plus the
  FaceTime tile f171), so the Weather row values are the screen crop + the
  same-batch Photos/FaceTime/Wallet tiles.
- Notes: the P1c 101x42 "Notes" tile and the 90-high 149x90/152x90 show the same
  bottom dot line, so they fall under A as well.

Artifacts (not committed): `/host/iosvm/logs/s20-p1-54px-dash/`.
