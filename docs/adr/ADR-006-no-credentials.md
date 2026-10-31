# ADR-006: no credentials in the tree, ever

- Status: Accepted
- Date: 2026-10-31
- Supersedes: —
- Superseded by: —

## Background

The design baseline warned that a leaked GitHub token had been pasted into a chat while the
documents were being written. That is the ordinary case, not an exotic one: tokens end up in a
command line, a screenshot, a `.git/config` URL, a CI log, a pasted log in an issue. A repository
is a permanent, mirrorable, crawler-indexed record, so a secret that enters it is compromised the
moment it is pushed — including force-pushed history, which forks and the GitHub Events API keep.

## Decision

1. **No secret in any file, any commit message, any fixture, any CI variable that is not a real
   GitHub secret.** `tools/provenance-check.py` scans for credential shapes and fails the build.
2. **The intended ways to authenticate**, in order of preference:
   - **SSH key** for pushing (`~/.ssh/id_ed25519_*`, mode 600, one key per machine);
   - **`gh auth login`** with a credential helper for issues/PRs;
   - **fine-grained PAT** only if a workflow needs one, scoped to this repository, ≤ 90 days,
     stored as an Actions secret and never printed. **Classic PATs with broad scopes are
     prohibited**: a `ghp_` token with `repo`+`workflow`+`delete_repo` is a hole in the shape of a
     convenience.
3. **Remote URLs never carry a token.** `git remote get-url origin` must be clean. A push that
   needs a token gets it from a credential helper or an environment variable in that shell.
4. **A leak is handled by revoking first, investigating second.** The point of a token is not
   recoverable; the point is to stop it working.

## If a credential lands in this repository

1. Revoke it at the issuer. Now. Before reading the rest of this list.
2. Rotate anything it could reach.
3. `git filter-repo --invert-paths --path <file>` over the whole history, force-push every branch
   and tag, tell forks (their refs break), and **assume the value was harvested** — GitHub's
   public repositories are crawled continuously.
4. Turn on secret scanning **and push protection** in the repository settings, so the next one is
   refused at the client.
5. Write it up in `docs/incidents/` with dates and what was accessible. No blame, no names.

## Consequences

**Positive**

- The repository is safe to mirror, zip and hand to a stranger, which the project needs.
- Contributors do not need credentials to build or test: `make test-open` needs no account at all.

**Negative**

- A first-time contributor has to set up an SSH key, which is more friction than pasting a token.
- Scanning for secrets means committing to keep the patterns current; a stale detector that only
  catches `ghp_` will miss the next format.

## Evidence

- The design baseline's own leak SOP (`docs/zh/git-rules.md` §5.4) already said "revoke first" —
  this ADR is that SOP made enforced rather than advisory.
- `tools/provenance-check.py` implements the scan (pattern list is narrow on purpose: a false
  positive costs a maintainer one review, a false negative costs the account).
