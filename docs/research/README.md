# Research notes

Notes that bound a question before the project commits to an answer. They are not decisions: a
note says what is known and how to find out more; an ADR says what was chosen
(`docs/adr/`). Every non-obvious claim here carries a source, and each claim is labelled
**verified** (read in a primary source), **reported** (someone else says so) or **speculation**
(reasoning).

| Note | Question | State |
|---|---|---|
| `d1-prime-arm64e.md` | Does iOS 26 arm64e user space boot on a kernel we compile, the way Darwin 16.5 does? | **answered** — `docs/adr/D1-prime-userland-version.md`: 16.5 is the target, iOS 26 deferred on licensing |
| `linux-kernel-build.md` | Can the kernel be built on Linux with no Mac at all, and what replaces Xcode's tools? | **in progress** — every source compiles; the final link stops on 9 symbols, hypotheses recorded |
| `apfs-writer-validation.md` | Is the APFS container we write with a vendored tool actually a filesystem, or only a file 7-Zip tolerates? | **validated** — Apple's `fsck_apfs` accepts it; what `hdiutil` additionally needs, and why it does not matter |
| `e2-kernel-port-surface.md` | How much of the kernel has to change between the Darwin 16.5 tag and the iOS 26 tag? (experiment E2 of the note above) | done — syscall and trap numbering unchanged; a handful of small changes; two research items |
| `how-to-reproduce-the-visible-screen.md` | What exactly has to be done, from an empty shell, to see the system's own screen in QEMU? | the procedure, with the three things that stop people (2026-11-01) |

The note's addendum (2026-10-31) records what the sibling project's own document settles: the arm64e
iOS 26 kernel boots in QEMU, the kernel side of the user space is ready, and what remains is **data
and a build host**, not kernel work.

## Why the labels matter

This project was once built on a confident wrong claim: that extracting an IPSW needs a Mac, and
that compiling XNU was only a fallback. Both were wrong (ADR-008, ADR-009). A note that cannot say
which of its sentences are evidence is how that happens again.
