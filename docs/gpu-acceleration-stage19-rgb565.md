# RGB565 half payload: host encodes, guest expands

## Switch (default 0)

- host env **`GLWIRE_UPLOAD_565=1`**, guest boot-arg **`glwire_upload_565=1`** (env same).
  Host startup logs `rgb565=1`; guest logs `upload565=1 (RGB565)`.  The 5651 failure was the
  **512-byte kern.bootargs buffer**; the 607-char cmdline put the flag past it.  Buffers are now
  2048 (`s19o-p1-bootargs2048.patch`), so the guest sends the 11th field.
- guest adds an 11th R field; host sends `R 2*w*h` of little-endian RGB565 instead of BGRA.
- guest receives one 2-byte row at a time and expands to BGRA: directly into the IOSurface when
  `GLWIRE_UPLOAD_FAST=1`, otherwise into `remote_frame` (old full-row copy then runs).
- default 0: old 10-field request, old BGRA/full path; e2e default PASS.
- host also logs `rgb565 sends 2436000 of 4872000 picture B`; guest logs
  `upload565 2436000 B in X ms (Y MB/s)`.
- dirty and scale are bypassed when RGB565 is requested; use only one experiment at a time.

## Test

`XCMD="... glwire_upload_565=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`
NO_PANEL+NO_INPLACE+POLL16.  Payload 2.44 MB; target upload <150ms and guest ≥3 fps.  If upload
still ~350–700ms, the remaining is QEMU TCP plus guest 2.4M-pixel expansion, not row copy.

Lib **402,408 B / md5 58e0ce2f5d420b9f7f1848f3d187e859**; host **123,928 B / md5
d17265ae530396eca39cd8f2e3f19a8d**; patch `logs/s19o-p1-565.patch` and
`logs/s19o-p1-bootargs2048.patch`.  Default 0; host env only logs/forces 565 when both sides are set.
