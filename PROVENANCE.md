# Provenance

Every file in this repository is either written here from scratch, or listed in `NOTICE`. No file
is copied from an IPSW, a firmware bundle, a device dump or any other Apple distribution.

## What is never in this repository

| Kind | Why | Where it lives instead |
|---|---|---|
| IPSW, `*.dmg`, `bootkc`, `dtree`, `*.im4p`, `*.im4m` | Apple's copyrighted material | The contributor's own disk, outside the tree |
| `firmware/`, `userland/` | Derived from an IPSW | Same — git-ignored, never staged |
| dyld shared cache, `.metallib` from a system | Same | Same |
| Fonts, images, sounds from a system | Same | Same |
| Activation records, tickets, certificates | Legal exposure | Nowhere — the project does not do activation |
| Credentials of any kind | Repo compromise | A password manager; see `docs/adr/ADR-006-no-credentials.md` |

`tools/provenance-check.py` enforces this in CI and in the pre-commit hook. It fails the build on:

1. any path under `firmware/` or `userland/` being tracked;
2. any tracked file with a forbidden extension (`*.ipsw`, `*.dmg`, `*.apfs`, `*.im4p`, `*.im4m`,
   `*.tc`, `bootkc`, `dtree`);
3. any tracked file that is not UTF-8 text and is not declared as an allowed binary below;
4. any file whose first bytes look like a Mach-O, a DMG trailer (`koly`), an IMG4 payload
   (`IM4P`) or an APFS container (`NXSB`);
5. any tracked file that contains a credential-shaped string (GitHub tokens, private keys,
   AWS keys, generic `password=` assignments);
6. any tracked script that downloads Apple content (`*.ipsw`, `*.dmg`, `updates.cdn-apple.com`,
   `mesu.apple.com`, `appldnld.apple.com`).

## Allowed binary files

The list is deliberately empty. If a binary ever has to be committed, add it here **with its
source and its SHA256**, in the same pull request, and say why in the commit body. `PROVENANCE.md`
is a compliance-sensitive path: changing it needs a maintainer review (see
`docs/adr/ADR-006-no-credentials.md`; compliance-sensitive paths need a maintainer review).

| Path | What it is | Made from | SHA256 |
|---|---|---|---|
| — | — | — | — |

## The tool chain is not downloaded for you

`tools/unpack-ipsw.sh` takes a path to an IPSW **on the contributor's disk**. It has no URL in it,
and adding one is a review veto. The same applies to every script under `tools/`.
