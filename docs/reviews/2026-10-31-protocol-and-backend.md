# Review 2026-10-31 — the protocol and the backend

Two adversarial reviews, one per component, run the day each was written. Both were asked to
reproduce the author's claims rather than read them, and to run the experiment that matters more
than any reading: **delete the guard, and see whether the suite notices.**

The result is recorded here in full, including what was wrong with this project's own statements,
because the value of a review is what happens to the findings afterwards — and because the same
class of mistake appeared twice in one day.

Status key: **fixed** — changed, verified, and pinned in the main repository by a submodule commit;
**accepted** — judged not worth changing, with the reasoning recorded. Nothing here was left as
"known but unowned", and every "fixed" was re-run by the coordinator against the reviewer's own
reproducer before the status changed.

---

## iosvm-proto (reviewed at commit `91694ad`)

### BLOCKER — an empty ring could refuse a record that fits, permanently

Counters were indices in `[0, size)`. After a wrap, an empty ring has `head == tail == k > 0`, and
`reserve()` took the wrap path and refused whenever the record did not fit in `size - k` — even
though the whole body was free. Every retry returned NULL, and **no public call could move the
counters again**: a legal sequence of records wedged the ring for good.

Reproducer: body 256 with a 136-byte record; body 65576 with the 65568-byte maximum record.

The one-line "fix" the reviewer warned against would publish `head == tail` and make the record look
like an empty ring. The representation had to change: counters became **cumulative offsets** with an
explicit padding marker, so an empty ring can always accept a record that fits.

Status: **fixed** — `iosvm-proto` 863ad43. Counters are cumulative offsets modulo the largest
multiple of the body size that fits in `uint32_t` (`P = floor(2^32/size)*size`), so a counter wrap
leaves `counter % size` unchanged; an empty ring pads and restarts instead of refusing. Both of the
reviewer's reproducers now pass: the wedge probe prints `no wedge`, and the under-filled reservation
is delivered once instead of forever.

### IMPORTANT — `WRITE`'s inline length was never checked against the envelope

`payload_len = 24` with an inner `len = 100` was accepted, and `payload_len = 65552` with
`len = 1`. The in-repo interpreter re-checked, so no out-of-bounds read was reachable *here* — but a
host written to the documentation ("the returned payload length always equals what the opcode
requires") would over-read.

Status: **fixed** — `iosvm-proto` 863ad43.

### MINOR — length arithmetic before validation

The truncation test formed `payload_len + 16` first, which wraps on ILP32 and turns a TRUNCATED into
a BOUNDS (with one test failing on a 32-bit host). Fixed by comparing against `len - hdr` after
`len >= hdr`, and the precedence of TRUNCATED over BAD_ALIGN is now documented.

Status: **fixed** — `iosvm-proto` 863ad43 (compare before adding, and the precedence is documented).

### MINOR — four tests that could not fail

This is the finding worth the most. Each was proved by mutation — remove the property, the suite
stays green:

| Test | What it claimed | What it did |
|---|---|---|
| "1000 messages wrap around" | exercises the wrap path | 32-byte records divide a 256-byte body exactly, so `head` never wrapped |
| "wrap padding" | exercises the tail-gap skip | ran it on an **empty** ring (`tail == head`), the one case with nothing to protect |
| "refused command mutates nothing" | nothing changes | the sequence counter does change; the case never looked at it |
| "neighbouring bytes are untouched" | no overrun | only checked the 16 bytes *before* the write, so a trailing overrun was invisible |

Status: **fixed** — 14 ring cases, including a 40-byte padding wrap, a wrap with unconsumed data, the
counter wrap at `P`, and the wedge reproducer. A 1M-step differential fuzz (500,567 records, 57,815
padding skips, five body sizes, three of them not powers of two) found no corruption.

### MINOR — documentation that contradicted the code

- "usable capacity is `size - 1`" — true only for a ring that has never wrapped, and the reason the
  BLOCKER went unnoticed.
- counters "advance modulo uint32_t" and are "indexed with `offset % size`" — the old code kept them
  in `[0, size)`; an independent implementation following the text would be wrong.
- `iosvm_ring.h` claimed "a second process can attach to it", with no attach API and a `memset` in
  `init()`.
- `BLIT_FILL`'s value was documented "native byte order" but written little-endian.

Status: **fixed** — all four now say what the code does.

### MINOR — UBSan did not gate

`-fsanitize=undefined` without `-fno-sanitize-recover` prints and exits 0, so a UBSan regression
passed CI while ASan failures aborted.

Status: **fixed** — `-fno-sanitize-recover=all`, proven with a two-line signed-overflow program that
exits 0 without the flag and 1 with it.

### NIT

Unknown ack statuses pass through and stringify as "unknown status" rather than mapping to
`IOSVM_ERR_UNKNOWN_OP`; `README` said "the four commands" over a list of five; an unreachable
`bytes == 0` branch in the fake GPU.

Status: **fixed**.

### What held up

- A differential model over 15 body sizes × 1M random producer/consumer steps under ASan found no
  reservation overlapping unconsumed data, no corrupted or reordered record, no non-zero wrap
  padding, no counter out of range.
- 3M random hostile buffers produced no read past the supplied length; the header is read only after
  the length check; the little-endian accessors are correct on a big-endian host.
- Every wire struct has a `sizeof` assert and every field an offset assert; `docs/protocol.md`'s
  worked example reproduces byte for byte.

---

## iosvm-vkbridge (reviewed at commit `7371126`)

### BLOCKER — the probe could not tell the device from a `memcpy`

The reviewer copied the repository, allocated the blit buffers `HostVisible`, replaced the blit with
a host `memcpy` and the fill with a host fill, added an empty recording so `submit()` still
succeeded — and **all eight probe checks printed `[ok]` on both lavapipe and the V100**. Replacing
the screenshot's render pass with a pure CPU layout function also printed 8/8.

So the probe verified the bytes and said nothing about the mechanism. The code did use
`vkCmdCopyBuffer`/`vkCmdFillBuffer` — but a reader was entitled to believe the check established
that, and it did not. The M2 status text in `docs/phases.md` and the README made exactly that
overclaim; both have been corrected.

The fix is a **device-side oracle**: a shader that runs on the device, reads the blitted bytes and
writes a checksum word the host then verifies. A host-side implementation cannot produce it. On a
UMA driver (lavapipe) the memory-type check is unavailable, which makes the oracle the *only*
evidence there — so it is not optional.

Status: **fixed** — `iosvm-vkbridge` b72bb8c. Each of `blit_copy`, `blit_fill` and `screenshot` now
takes an attestation buffer and a nonce; a compute shader recorded into the same command buffer reads
the data that was operated on and writes a nonce-derived checksum the host never touches. On a device
with a non-host-visible device-local heap the working buffers must also land in it; on a UMA driver
the probe prints one `[info]` line and lets the oracle carry the proof. Both of the reviewer's cheats
fail now: on the V100 the memory-type assertion catches it, on lavapipe the oracle does.

### IMPORTANT — the screenshot had the same weakness

GPU output was compared against a CPU model that a CPU implementation would return. Same oracle
needed.

Status: **fixed** — same commit; the frame is read back through the device and its checksum attested.

### IMPORTANT — ordering assertions that could not fail

`CompletionOrder` was fed values in wait order (always ascending), and "40 command buffers in order"
checked 40 submissions and a counter, not distinct command buffers. The unit tests test the detector,
not the queue — which is legitimate, but the probe line implied otherwise.

Status: **fixed** — the ordering is now observed by polling unwaited batches and asserting the signal
order is a permutation of them.

### IMPORTANT — the screenshot error path destroyed in-flight objects

After a successful `vkQueueSubmit`, a failing fence wait (timeout, device lost) let the cleanup
destroy the pending `VkFence`, the command buffer, the image and its memory **while still in use**.

Status: **fixed** — wait idle (checked) before cleanup on post-submit errors; on device loss, prefer
leaking to destroying in-flight handles.

### MINOR

- The probe checked only the PNG's 8-byte signature; a corrupt IDAT would still pass. The written PNG
  itself was decoded independently by the reviewer and is correct (valid stored-block deflate,
  adler32, CRCs, 22 colours, pixel-identical between the two devices).
- The "copy time" includes command-buffer creation, submit and fence wait; an empty submit measured
  0.07 ms on lavapipe and **4.36 ms** on the V100, so the number is not a transfer measurement.
- Every submission's fence was retained until `Replay` destruction — unbounded growth for a long
  replay.
- `make test` printed `[skip]` and exited 0 for a *misconfigured* ICD, not just for an absent device.
- README claimed copy sizes must be multiples of 4 "which Vulkan requires" — true for
  `vkCmdFillBuffer` sizes, not for `vkCmdCopyBuffer` sizes.
- CI used the deprecated `VK_ICD_FILENAMES` with a Mesa-version-specific ICD filename.

Status: **fixed** — the PNG is decoded and its pixels compared; fences are reaped after their wait; a
misconfigured ICD is an error while an absent device is a skip; the README describes what the probe
does and does not establish; and the deprecated `VK_ICD_FILENAMES` is replaced by `VK_DRIVER_FILES`.

### What held up

- The blit really does use `vkCmdCopyBuffer`/`vkCmdFillBuffer`; host `memcpy` exists only on the
  staging upload/readback path.
- Memory type selection, usage flags, the image barrier before `vkCmdCopyImageToBuffer`, one fresh
  fence per submission (never reused, never needing a reset), submit/wait results checked, and
  destruction order are all sound.
- The PNG writer is genuinely zlib-free and correct: stored blocks with valid LEN/NLEN, big-endian
  Adler-32, valid chunk CRCs, byte-identical output from two different devices.
- The colour count in the probe is computed from the actual image, not hardcoded.

---

## The pattern

Both reviews found the same shape of defect, in different code: **a test whose expected value was
produced by the same implementation it was checking**, or an assertion that could not fail because
its inputs were already sorted. Neither was a hard bug to fix; both were hard to see.

The rule that comes out of it, now applied to new tests: *a check that has never been observed to
fail is not evidence.* Delete the guard and confirm the suite turns red — or the guard is decorative.
