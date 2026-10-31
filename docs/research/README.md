# Research notes

Notes that bound a question before the project commits to an answer. They are not decisions: a
note says what is known and how to find out more; an ADR says what was chosen
(`docs/adr/`). Every non-obvious claim here carries a source, and each claim is labelled
**verified** (read in a primary source), **reported** (someone else says so) or **speculation**
(reasoning).

| Note | Question | State |
|---|---|---|
| `d1-prime-arm64e.md` | Does iOS 26 arm64e user space boot on a kernel we compile, the way Darwin 16.5 does? | open — the gate `docs/adr/D1-prime-userland-version.md` does not exist yet |
| `e2-kernel-port-surface.md` | How much of the kernel has to change between the Darwin 16.5 tag and the iOS 26 tag? (experiment E2 of the note above) | done — syscall and trap numbering unchanged; a handful of small changes; two research items |

## Why the labels matter

This project was once built on a confident wrong claim: that extracting an IPSW needs a Mac, and
that compiling XNU was only a fallback. Both were wrong (ADR-008, ADR-009). A note that cannot say
which of its sentences are evidence is how that happens again.
