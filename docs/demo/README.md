# Demo evidence

One directory per milestone. Every file here is a capture, not a mock-up. Each capture carries a
`NOTES.md` beside it naming the commit and the IPSW version it came from — if that file is absent,
the capture is not evidence yet.

`*.png` and `*.gif` are the only binaries allowed in the tree (see `.gitignore`). A screenshot is
evidence; a video goes to a release attachment, not to git.

Per milestone:

| Milestone | Evidence |
|---|---|
| M0 | `m0/root-shell.png` — the guest prompt |
| M1 | `m1/windowserver.png` — `ps aux` showing it |
| M2 | `m2/probe.txt` — pasted probe output (text, not an image) |
| M3 | `m3/first-frame.png` — the acceptance frame |
| M4 | `m4/demo.gif` — the interaction recording |
