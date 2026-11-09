# RGB565 half payload: host encodes, guest expands

## Switch (default 0)

- host env **`GLWIRE_UPLOAD_565=1`**, guest boot-arg **`glwire_upload_565=1`** (env same).
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

Lib **402,408 B / md5 95607e1086cf29b1f2d21d217c1f34e1**; host **123,888 B / md5
01c5fc50e8189c815f795f74f9a850e3**; patch `logs/s19o-p1-565.patch` (107 lines).
