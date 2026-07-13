# Releasing

This document is the runbook for cutting a Twist Your Guts release: what secrets the pipeline needs, how to generate them, and how to trigger a release. See [`.github/workflows/release.yml`](../.github/workflows/release.yml) for the pipeline itself and [ADR 0006](adr/0006-macos-signing-notarization.md) for why it's designed this way.

> **Status:** this pipeline is **dormant** until (a) the secrets below are set on the repository and (b) a `v*` tag is pushed. It has been validated for YAML correctness and `actionlint` cleanliness, and the CMake target names/artefact paths it references have been cross-checked against `CMakeLists.txt`, but it has **not** been exercised end-to-end (no real certificate, no real notarization run, no real tag push) — treat the first real release as the actual first test of this pipeline.

## Overview

Pushing a `v*` tag (e.g. `v1.0.0`) triggers `.github/workflows/release.yml`, which:

1. Builds a macOS **Universal** (arm64 + x86_64) Release of the AU and VST3 formats, signs them with a **Developer ID Application** certificate, notarizes them with Apple's notary service, and staples the notarization ticket to each bundle.
2. Builds a Windows Release VST3 (unsigned).
3. Publishes both as zip assets on a GitHub Release for the tag, with the matching `CHANGELOG.md` section as the release notes body.

The same workflow can also be run manually via `workflow_dispatch` (Actions tab → "Release" → "Run workflow") **without** a tag — this is a structural dry-run: it builds and packages the macOS and Windows artefacts but skips signing/notarization/stapling, and it never publishes a GitHub Release (that job only runs on an actual `v*` tag push). A `workflow_dispatch` run is **always** a structural dry run, even if the five signing secrets are already configured on the repository — the codesign/notarize/staple steps require both the secrets to be present *and* the ref to be a `refs/tags/v*` ref, so secrets alone can never trigger real signing outside of a tag push.

**A `v*` tag push with missing or misnamed signing secrets now hard-fails the workflow** rather than silently publishing an unsigned build as the official release. If any of the five required secrets is absent when a tag is pushed, the "Check signing/notarization secrets" step exits non-zero and the `release` job (which also gates on `needs.macos.outputs.has_secrets == 'true'`) never runs — no GitHub Release is created. Fix the secrets and re-push the tag (or re-run the workflow for that tag) once they're provisioned.

## Required secrets

Five repository secrets, all consumed by `release.yml` by name (must match exactly):

| Secret | What it is |
|---|---|
| `APP_STORE_CONNECT_API_KEY_ID` | The App Store Connect API key's Key ID (e.g. `ABC123XYZ9`) |
| `APP_STORE_CONNECT_ISSUER_ID` | The API key's Issuer ID (a UUID) |
| `APP_STORE_CONNECT_API_KEY_BASE64` | Base64 of the `AuthKey_<KEY_ID>.p8` private key file |
| `DEVELOPER_ID_APP_CERT_BASE64` | Base64 of the exported **Developer ID Application** certificate, as a `.p12` |
| `DEVELOPER_ID_APP_CERT_PASSWORD` | The password chosen when exporting that `.p12` |

Plus one repository **variable** (not a secret — it's not sensitive, and the workflow reads it via `vars.APPLE_TEAM_ID`):

| Variable | Value |
|---|---|
| `APPLE_TEAM_ID` | `M5WT732AY5` |

If this variable is already set on the repository, no action is needed. If not:

```sh
gh variable set APPLE_TEAM_ID --repo yves-vogl/twist-your-guts --body "M5WT732AY5"
```

### Reusing the App Store Connect API key

The App Store Connect API key is the **same key already used for Kadenz TestFlight uploads** — it does not need to be regenerated. If you still have the `.p8` file and its Key ID / Issuer ID from setting up Kadenz, you can reuse those same three values directly; skip to [setting the secrets](#setting-the-secrets-yourself) below.

If you need to generate a new one (e.g. the old key was revoked, or you want a dedicated key for this project):

1. Go to [App Store Connect](https://appstoreconnect.apple.com) → **Users and Access** → **Integrations** → **App Store Connect API**.
2. Click **Generate API Key** (or the **+** button under "Team Keys").
3. Give it a name (e.g. "Twist Your Guts CI notarization") and an **Access** role — "Developer" is sufficient for notarization; it does not need "Admin".
4. Download the `AuthKey_<KEY_ID>.p8` file **immediately** — Apple only lets you download it once.
5. Note the **Key ID** (shown in the key list) and the **Issuer ID** (shown at the top of the Integrations page, a UUID shared by all your team's keys).

### Exporting the Developer ID Application certificate

1. Open **Keychain Access** on a Mac where the Developer ID Application certificate (and its private key) already exists, under team `M5WT732AY5`. If it doesn't exist yet, create one first via **Xcode → Settings → Accounts → [your Apple ID] → Manage Certificates → + → Developer ID Application**, or via the [Apple Developer certificates portal](https://developer.apple.com/account/resources/certificates/list).
2. In Keychain Access, locate the certificate (it should show a disclosure triangle revealing its private key underneath — you need **both**, not just the certificate).
3. Select **both** the certificate and its private key (cmd-click to multi-select), right-click → **Export 2 items…**.
4. Save as `DeveloperIDApplication.p12`, choose a strong password when prompted (this becomes `DEVELOPER_ID_APP_CERT_PASSWORD`).

### Setting the secrets yourself

**Run these yourself** — paste the values when prompted, do not hand them to an assistant or paste them into chat:

```sh
gh secret set APP_STORE_CONNECT_API_KEY_ID --repo yves-vogl/twist-your-guts
gh secret set APP_STORE_CONNECT_ISSUER_ID --repo yves-vogl/twist-your-guts
gh secret set APP_STORE_CONNECT_API_KEY_BASE64 --repo yves-vogl/twist-your-guts < <(base64 -i AuthKey_XXXX.p8)
gh secret set DEVELOPER_ID_APP_CERT_BASE64 --repo yves-vogl/twist-your-guts < <(base64 -i DeveloperIDApplication.p12)
gh secret set DEVELOPER_ID_APP_CERT_PASSWORD --repo yves-vogl/twist-your-guts
```

For the two `_BASE64` secrets, `gh secret set NAME < <(base64 -i file)` reads the base64-encoded content directly from the process substitution without it ever touching your shell history or an intermediate file. If you prefer typing the value interactively instead, running `gh secret set NAME --repo yves-vogl/twist-your-guts` with no input redirection will prompt you to paste it.

For `APP_STORE_CONNECT_API_KEY_ID`, `APP_STORE_CONNECT_ISSUER_ID`, and `DEVELOPER_ID_APP_CERT_PASSWORD`, just run the bare `gh secret set NAME --repo yves-vogl/twist-your-guts` command and paste the value at the prompt.

Verify what's set (this only lists secret **names**, never values):

```sh
gh secret list --repo yves-vogl/twist-your-guts
gh variable list --repo yves-vogl/twist-your-guts
```

## Cutting a release

Once the secrets and variable above are set:

```sh
git tag -s v1.0.0 && git push origin v1.0.0
```

(`-s` creates a signed tag — recommended but not required by the workflow, which triggers on any `v*` tag regardless of signature.)

Pushing the tag runs `release.yml`, which builds, signs, notarizes, staples, and publishes a GitHub Release at `https://github.com/yves-vogl/twist-your-guts/releases/tag/v1.0.0` with the `## [1.0.0]` section of `CHANGELOG.md` as its body (make sure that section exists in `CHANGELOG.md` **before** tagging — if it's missing, the release is still created, but with a generic placeholder body and a workflow warning).

If a release for that tag already exists (e.g. re-running after fixing a build issue), the workflow updates the existing release's notes and re-uploads the assets (`--clobber`) rather than failing.

## Windows: unsigned VST3 caveat

The Windows VST3 build is shipped **unsigned** in v1.0. On first run, Windows SmartScreen may show a "Windows protected your PC" warning. This is expected for the moment. Users can proceed via **More info → Run anyway**. This is a **one-time reputation warning**, not a hard block like macOS Gatekeeper's response to an unsigned/unnotarized download — see [ADR 0006](adr/0006-macos-signing-notarization.md#windows-note) for why this is an accepted trade-off for v1.0 rather than something the pipeline works around. A Windows code-signing certificate (and associated `signtool` step) is a documented future improvement, not yet scoped.

## Dry-running without secrets

To sanity-check that the build/package steps still work (e.g. after a CMake or artefact-path change) without touching any signing infrastructure:

1. Go to the **Actions** tab → **Release** workflow → **Run workflow** → select the branch → **Run workflow**.
2. On a `workflow_dispatch` run, the codesign/notarize/staple steps never execute regardless of secret state, because they additionally require the ref to be a `refs/tags/v*` ref. If the five secrets aren't set, the `macos` job's "Check signing/notarization secrets" step logs `::warning::Signing/notarization secrets are absent — skipping sign/notarize/staple (dry-run build only)` (a `workflow_dispatch` run is the only case where this is a warning rather than a hard failure). Either way, the build proceeds to produce an **unsigned** packaged zip artifact.
3. The unsigned dry-run zip and its workflow artifact are named with a `-unsigned-dryrun` suffix (e.g. `TwistYourGuts-macOS-unsigned-dryrun.zip`, artifact `twist-your-guts-macos-unsigned-dryrun`) so it can never be mistaken for a real signed release asset. It is uploaded as a normal workflow artifact, not a GitHub Release — the `release` job only runs on a tag push where signing actually succeeded.

This is useful for validating CMake/target/path changes land correctly before an actual tagged release exercises the full signing chain.
