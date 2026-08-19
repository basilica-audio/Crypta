# 4. Host the repository in an organization, not a personal account

* Status: accepted
* Deciders: Yves Vogl
* Date: 2026-07-14
* Related: [ADR 0003 — Squash-only merge policy on `main`](0003-squash-only-merge-policy.md)

## Context and Problem Statement

The repository started life as the personal repository `yves-vogl/twist-your-guts`.
A personal namespace ties the project's identity, permissions, and CI configuration to
one user account. Should the repository move to a GitHub organization, and what
governance changes — if any — should the transfer introduce?

## Decision Drivers

* Project identity should be separable from a single personal account (future
  co-maintainers, org-level secrets for release signing, consistent branding across
  sibling plugin repositories).
* The transfer must not silently change governance: existing branch protection,
  rulesets, and Actions permissions should carry over verified, not assumed.

## Considered Options

* **Stay in the personal namespace** (`yves-vogl/twist-your-guts`)
* **Transfer to an organization**

## Decision Outcome

Chosen option: **transfer to an organization**. On 2026-07-14 the repository moved from
`yves-vogl/twist-your-guts` to `metal-up-your-ass/twist-your-guts`.

The governance state was verified after the transfer, and deliberately **no new
constraints were added**:

* required approving reviews: **0** (single-maintainer flow unchanged)
* no `CODEOWNERS`
* Actions policy: `allowed_actions: all`
* branch protection (`required_linear_history`, required status checks) and the
  "Signed commits on main" ruleset carried over intact — together these are what make
  squash the only usable merge method (see ADR 0003)

### Consequences

* Good, because org-level ownership decouples the project from a personal account and
  allows org-level encrypted secrets (release signing certificates) shared across the
  plugin suite.
* Good, because the transfer was verified to be governance-neutral — no accidental
  tightening or loosening of who can merge what.
* Neutral, because GitHub serves permanent redirects from the old namespace, so
  existing clones and links keep working.
* Bad, because organization-level settings (member privileges, Actions policy) become
  a second place besides the repository itself where governance can change.

## Postscript (2026-07)

As part of the suite-wide move to Basilica Audio naming, the repository was
subsequently renamed **Crypta** (see the plugin-identity rename entry in
[`CHANGELOG.md`](../../CHANGELOG.md)) and now lives at `basilica-audio/Crypta`; the
old namespaces redirect. This changed the organization and the name, but not the
decision this ADR records: the repository is org-hosted, and the transfer-verified
governance state described above still applies (re-verified at the time of writing:
squash-only merge settings, 0 required approvals, no `CODEOWNERS`, linear history +
signed-commits ruleset active).
