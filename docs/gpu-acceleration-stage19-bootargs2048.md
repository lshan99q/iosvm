# Why 565 did not activate: boot-arg buffer was 512, cmdline is 607

`logs/s20-5651` cmdline:
`... glwire_upload_fast=1 glwire_dirty_upload=0 glwire_upload_565=1 glwire_ack_ms=0`
(length shown as 315 of 607 characters; the tail sits beyond 512).

`eagl_remote.c:190 remote_boot_arg()` copied `kern.bootargs` into a **512-byte** buffer, so
`glwire_upload_565=1` (and the trailing ack flag) was never visible; `remote_upload_565()` returned
0, the 11th request field stayed 0, and host 01c5 never entered its `rgb565_wanted` branch.
`eagl.c` had the same 512-byte pattern for `label_dash_rows`/`glyph_last_row`.

Fix: buffers 512 -> **2048** in `eagl_remote.c` (boot-arg reader) and `eagl.c` (the two optional
readers).  `eagl.c` init already uses 1024 and fits 607.

- lib **402,408 B / md5 44b27a8ce231d0f59ee2d5dc27a64506**
- host unchanged **01c5fc50e8189c815f795f74f9a850e3** (565 encoding)
- patch `logs/s19o-p1-bootargs2048.patch` (31 lines)
- default behavior unchanged (larger stack buffers only); 565 still default off.

Retest 5651 XCMD; now expect guest `upload RGB565` + `upload565 2436000 B ...`, host
`rgb565 sends 2436000 of 4872000 picture B`.
