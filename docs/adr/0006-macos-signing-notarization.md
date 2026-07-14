# 6. macOS Developer ID signing and notarization for releases

* Status: accepted
* Deciders: Yves Vogl
* Date: 2026-07-14
* Related: [ADR 0001 — Use JUCE 8 as the plugin framework](0001-use-juce8-framework.md)

## Context and Problem Statement

Twist Your Guts is distributed **outside the Mac App Store** — users download a plugin bundle (AU `.component` and VST3 `.vst3`) directly from GitHub Releases and copy it into their local plug-in folders. Since macOS Catalina, Gatekeeper blocks or warns on unsigned/unnotarized software downloaded from the internet (it carries a quarantine attribute). Without signing and notarization, every user would have to manually override Gatekeeper (right-click → Open, or `xattr -d com.apple.quarantine`) for each format, which is a poor and support-heavy first-run experience. How should the project sign and notarize its macOS release artefacts, and how should the release pipeline authenticate to Apple's services to do so?

## Decision Drivers

* Must work for **direct/independent distribution**, not App Store distribution — the plugin is never submitted to the Mac App Store.
* Must be automatable in GitHub Actions without any interactive prompts (no Xcode GUI, no 2FA prompts at build time).
* Should reuse existing account infrastructure where possible rather than provisioning new Apple credentials from scratch.
* Must not regress `auval`/`pluginval` validation or existing CI (`.github/workflows/ci.yml`) which builds unsigned debug/local artefacts for validation only, not for distribution.
* Windows has no equivalent Gatekeeper-style hard block (SmartScreen is a softer, reputation-based warning), so the two platforms don't need symmetrical solutions in v1.0.

## Considered Options

* **Developer ID Application signing + hardened runtime + `notarytool` + stapling** (this decision)
* **Mac App Store distribution signing** (Apple Distribution certificate + App Store provisioning profile)
* **No signing / ad-hoc signing only** (ship unsigned zips, rely on users to bypass Gatekeeper manually)

## Decision Outcome

Chosen option: **Developer ID Application certificate + hardened runtime (`--options runtime`) + secure timestamp (`--timestamp`) + `xcrun notarytool` + `xcrun stapler staple`**, authenticated via an **App Store Connect API key** (reusing the same key the owner already uses for Kadenz TestFlight uploads). Team ID `M5WT732AY5` is referenced as a GitHub Actions repository variable (`vars.APPLE_TEAM_ID`), never hardcoded in workflow bodies.

This is the only option of the three that (a) matches the actual distribution channel (outside the App Store), (b) is fully non-interactive/automatable via API-key auth (no Apple ID password or 2FA prompt needed at submission time), and (c) gives end users a "just works" Gatekeeper experience once stapled.

### Consequences

* Good, because stapled, notarized AU/VST3 bundles open without any Gatekeeper warning or manual override for end users — the primary UX goal.
* Good, because API-key authentication (`--key`/`--key-id`/`--issuer`) is fully non-interactive and safe to run unattended in CI, unlike Apple-ID/password + app-specific-password auth (which additionally requires `--team-id` and does not apply here).
* Good, because the App Store Connect API key is reused from existing infrastructure (the same key pattern already used for Kadenz TestFlight uploads) — no new Apple-side credential provisioning process was needed.
* Good, because hardened runtime + secure timestamp are prerequisites Apple's notary service enforces anyway; baking them into the `codesign` invocation from the start avoids a rejected-submission feedback loop.
* Neutral, because the Developer ID Application certificate and its `.p12` export are a one-time manual step for the owner (Keychain Access export); this is documented in [`docs/releasing.md`](../releasing.md) rather than automated, since certificate issuance requires interactive Apple Developer Program authentication that cannot be scripted safely.
* Bad, because it introduces five long-lived GitHub Actions secrets (`APP_STORE_CONNECT_API_KEY_ID`, `APP_STORE_CONNECT_ISSUER_ID`, `APP_STORE_CONNECT_API_KEY_BASE64`, `DEVELOPER_ID_APP_CERT_BASE64`, `DEVELOPER_ID_APP_CERT_PASSWORD`) that must be rotated if the certificate expires (Developer ID Application certificates are valid for 5 years) or the API key is revoked.
* Bad, because the release workflow (`.github/workflows/release.yml`) is necessarily more complex than the CI workflow (temporary keychain lifecycle, secret-presence guard for dry runs, notarization polling, stapling, verification) — accepted as inherent complexity of the requirement, mitigated by a signing-guard dry-run path (`workflow_dispatch` without secrets present still exercises the build/package steps).
* Good, because the secret-presence guard is a hard gate on a real tag push: if any of the five secrets is missing when a `v*` tag is pushed, the workflow fails closed (`::error::` + `exit 1`) rather than silently publishing an unsigned build as the official release; the `release` job independently re-checks `needs.macos.outputs.has_secrets == 'true'` so the publish gate does not depend solely on earlier steps having behaved correctly. The soft warn-and-continue dry-run path is reserved exclusively for `workflow_dispatch` runs on a non-tag ref — and codesign/notarize/staple are additionally gated on the ref being a tag, so configuring secrets never turns a `workflow_dispatch` run into a real signing run.

## Pros and Cons of the Options

### Developer ID Application + notarytool (chosen)

* Good, because it is the Apple-sanctioned path for signing software distributed outside the App Store.
* Good, because `notarytool` with an App Store Connect API key is scriptable end-to-end with no interactive prompts.
* Good, because it directly reuses infrastructure (the API key) the owner already has for another project.
* Neutral, because it requires an active paid Apple Developer Program membership (already held by the owner; this is a sunk cost, not a new one incurred by this decision).
* Bad, because it introduces keychain lifecycle management in CI (temporary keychain creation, cert import, cleanup) that has no equivalent complexity on the Windows side.

### Mac App Store distribution signing

* Good, because it would be the correct choice **if** the plugin were ever submitted to the Mac App Store as an Audio Unit extension.
* Bad, because it is the **wrong channel entirely** for this project: Twist Your Guts is not submitted to the App Store, is not sandboxed for App Store review, and distributing an App-Store-signed binary outside the App Store is both against the intent of that signing identity and would not satisfy Gatekeeper's notarization requirements for direct distribution the same way Developer ID signing does.
* Bad, because App Store review would additionally gate every release on Apple's review turnaround and policies, which is unnecessary friction for a plugin that isn't sold through that channel.

### No signing / ad-hoc signing only

* Good, because it is the simplest possible pipeline — no certificates, no notarization, no keychain management.
* Bad, because every user on a current macOS version would see a Gatekeeper block ("Apple could not verify... is free of malware") for a plugin bundle carrying the quarantine attribute, requiring a manual bypass per format — a significant, avoidable adoption barrier for an audio plugin aimed at musicians, not developers.
* Bad, because it does not scale: the project would either need per-user support instructions for every release, or accept a materially worse first-run experience than comparable commercial and open-source plugins in the same category.

## Windows note

Windows VST3 builds are shipped **unsigned** for v1.0. Windows SmartScreen may show a reputation warning ("Windows protected your PC") on first run, which the user can dismiss via "More info" → "Run anyway" — this is a softer, one-time warning rather than a hard Gatekeeper-style block, and does not carry the same adoption-barrier risk as unsigned macOS distribution. A Windows code-signing certificate (EV or OV) is a documented future improvement, out of scope for v1.0; see [`docs/releasing.md`](../releasing.md) for the current caveat as surfaced to users.
