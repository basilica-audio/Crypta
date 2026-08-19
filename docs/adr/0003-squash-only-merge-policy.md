# 3. Squash-only merge policy on `main`

* Status: accepted
* Deciders: Yves Vogl
* Date: 2026-07-14
* Related: [ADR 0004 — Host the repository in an organization](0004-organization-hosting.md)

## Context and Problem Statement

`main` is protected by two mechanisms that were adopted independently:

1. **Classic branch protection** with `required_linear_history` — no merge commits may
   ever land on `main`.
2. A **"Signed commits on main" ruleset** (`required_signatures`) — every commit that
   lands on `main` must carry a valid signature.

GitHub offers three merge methods for pull requests: merge commit, rebase, and squash.
Which of them can actually be used under these constraints, and what should the
repository's policy be?

## Decision Drivers

* Linear history on `main` is wanted (bisectability, a readable first-parent log).
* Every commit on `main` must be signature-verified (the ruleset is non-negotiable).
* The merge method must work through GitHub's own UI/API — no local push workarounds,
  since `main` only moves via pull requests.

## Considered Options

* **Merge commits**
* **Rebase merges**
* **Squash merges**

## Decision Outcome

Chosen option: **squash merges, as the only enabled merge method**. The two protection
mechanisms together eliminate the alternatives:

* Merge commits are rejected outright by `required_linear_history`.
* Rebase merges produce new commits that GitHub does **not** re-sign (the original
  signatures are invalidated by rewriting), so the `required_signatures` ruleset
  rejects them.
* Squash merges produce a single new commit created **and signed by GitHub** (web-flow
  key), which satisfies both the linear-history requirement and the signature ruleset.

The repository settings enforce this: squash is enabled, merge commits and rebase
merges are disabled.

### Consequences

* Good, because `main` stays strictly linear with exactly one commit per pull request,
  and the pull-request title becomes the commit subject (kept in Conventional Commits
  form, e.g. `feat(dsp): ... (#69)`).
* Good, because every commit on `main` is signature-verified without exception.
* Accepted authorship consequence: squash commits on `main` are authored with the
  GitHub-noreply address and signed by GitHub's web-flow key, while local working
  commits remain `yves.vogl@mac.com` with a personal GPG signature. `git log main`
  therefore shows GitHub as the signer, not the personal key — this is expected, not a
  misconfiguration.
* Bad, because a pull request's individual commit granularity is collapsed into one
  commit; fine-grained history survives only in the pull request itself.
* Neutral, because release tags remain locally created, GPG-signed annotated tags
  (`git tag -s`) pointing at squash commits — tag signatures are unaffected by the
  merge method.
