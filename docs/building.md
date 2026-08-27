# Building from source

## Prerequisites

**macOS**

- Xcode (latest stable, with command-line tools installed: `xcode-select --install`)
- CMake ≥ 3.24 and Ninja, via [Homebrew](https://brew.sh):

  ```sh
  brew install cmake ninja
  ```

**Windows**

- Visual Studio 2022 (or the standalone [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)), with the "Desktop development with C++" workload
- CMake ≥ 3.24 (bundled with recent Visual Studio installs, or install separately)

JUCE 8.0.14 and Catch2 v3 are fetched automatically via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) — no manual JUCE checkout is required.

## Configure, build, test

**macOS**

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For a macOS Universal Binary (arm64 + x86_64), add the architectures flag to the configure step:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

**Windows**

```sh
cmake -B build
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

(The default Visual Studio generator is multi-config, so `--config Release` / `-C Release` select the configuration at build/test time rather than at configure time.)

## Speeding up repeat builds

CPM caches downloaded/fetched dependencies. Set `CPM_SOURCE_CACHE` to a persistent directory to avoid re-fetching JUCE and Catch2 on every clean build:

```sh
export CPM_SOURCE_CACHE="$HOME/.cache/CPM"
```

Set this before running `cmake -B build ...`. This is also how CI caches dependencies between runs (see [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)).

## Build artefacts

Built plugin formats (AU, VST3, Standalone) land under `build/Crypta_artefacts/`, in a `Release/` subdirectory for multi-config generators (e.g. the Windows Visual Studio generator) or directly under `Crypta_artefacts/` for single-config generators (e.g. Ninja), depending on generator and configuration.

## Releasing

Releases are cut by pushing a `v*` tag; nothing is built or uploaded by hand.
[`.github/workflows/release.yml`](../.github/workflows/release.yml) then runs
three jobs:

1. **`create-release`** — creates the GitHub release for the tag. The body is
   the matching `## [x.y.z]` section of [`CHANGELOG.md`](../CHANGELOG.md); if
   the tag has no CHANGELOG section the workflow logs a warning and falls back
   to GitHub's generated notes, so a missing entry degrades the notes instead
   of failing the release.
2. **`release-macos`** — Universal Binary (arm64 + x86_64) AU, VST3 and
   Standalone, code-signed with the Developer ID Application certificate in a
   throwaway keychain, notarized via `notarytool`, stapled, and then shipped
   **twice**: as a `.zip` of the three bundles, and as a `.pkg` installer that
   puts each format where a DAW scans for it. See
   [macOS `.pkg` installer](#macos-pkg-installer).
3. **`release-windows`** — VST3 and Standalone, zipped. **Unsigned** — see the
   note below.

Every uploaded asset — both archives and the installer — is accompanied by a
`.sha256` file, so a download can be verified with
`shasum -a 256 -c <asset>.sha256`.

The workflow only ever fires on tag pushes to this repo, so the signing
secrets are never exposed to pull requests from forks.

### Cutting a release

```sh
# 1. add the CHANGELOG section for the new version
# 2. bump project(Crypta VERSION x.y.z ...) in CMakeLists.txt
# 3. merge to main, then tag the merge commit
git tag -s vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
```

### Signing secrets

All signing material lives in **organization-level** GitHub Actions secrets
(`basilica-audio`, visibility `all`), shared by every plugin in the suite. The
repository itself holds no secrets. Only the names are listed here; the values
are set by Yves and never leave his machine in plaintext.

| Secret / variable | Scope | Used for |
|---|---|---|
| `APPLE_CERT_P12` | org secret | base64 Developer ID **Application** certificate + key, imported into the throwaway keychain |
| `APPLE_CERT_PASSWORD` | org secret | password for the above p12 |
| `APPLE_API_KEY_P8` | org secret | App Store Connect API key used by `notarytool` |
| `APPLE_API_KEY_ID` | org secret | key id for the above |
| `APPLE_API_ISSUER_ID` | org secret | issuer id for the above |
| `APPLE_TEAM_ID` | org variable | Apple team id (`M5WT732AY5`) |
| `APPLE_INSTALLER_CERT_P12` | org secret — **not yet set** | base64 Developer ID **Installer** certificate; signs the `.pkg` |
| `APPLE_INSTALLER_CERT_PASSWORD` | org secret — **not yet set** | password for the above p12 |

The two `APPLE_INSTALLER_CERT_*` entries are the only ones missing. Their
absence does not block a release: the `.pkg` still builds and still ships, just
unsigned and under a different filename. Adding them is a certificate task, not
a code change — nothing in this repo needs editing when they appear:

1. <https://developer.apple.com/account/resources/certificates/add> → **Developer
   ID** → **Developer ID Installer**. Apple requires the Account Holder for
   this, the same as for the Developer ID Application certificate.
2. Export it from Keychain Access as a password-protected `.p12`, base64-encode
   it (`base64 -i cert.p12 | pbcopy`), and set both secrets at **organization**
   level on `basilica-audio` with visibility `all`, matching the existing
   `APPLE_*` secrets.
3. Push the next `v*` tag. `release.yml` imports the certificate into the same
   throwaway keychain, finds the identity, and switches to the signed path
   automatically — the branch is conditioned on the identity being present in
   the keychain, not on a workflow edit.

Two nearby certificates are **not** substitutes, and neither is a workaround:

- **Developer ID *Application*** (`APPLE_CERT_P12`, already provisioned) signs
  the bundles, not the installer. `productsign` rejects it with
  `error: Could not find appropriate signing identity ... An installer signing
  identity (not an application signing identity) is required for signing
  flat-style products.`
- **3rd Party Mac Developer Installer** signs `.pkg` submissions to the Mac App
  Store. It is not part of the Developer ID trust path, so a `.pkg` signed with
  it is not accepted by `notarytool` for direct distribution.

### macOS `.pkg` installer

[`.github/scripts/build-macos-pkg.sh`](../.github/scripts/build-macos-pkg.sh)
assembles a three-choice distribution installer from the staging tree that
`release.yml` has already signed, notarized and stapled:

| Choice | Installs | Destination |
|---|---|---|
| Audio Unit (AU) | `Crypta.component` | `/Library/Audio/Plug-Ins/Components` |
| VST3 | `Crypta.vst3` | `/Library/Audio/Plug-Ins/VST3` |
| Standalone app | `Crypta.app` | `/Applications` |

The assembly lives in a script rather than inline in the workflow so the exact
code path that ships can be run locally against real artefacts:

```sh
.github/scripts/build-macos-pkg.sh \
  --stage path/to/stage --version 0.4.0 --out /tmp/crypta.pkg
```

where `stage/` holds `AU/Crypta.component`, `VST3/Crypta.vst3` and
`Standalone/Crypta.app` — the same layout as the shipped `-macos.zip`, so an
existing release archive can be unzipped and fed straight in.

Relocation is switched off on every component (`BundleIsRelocatable = false`),
because all three JUCE bundles share the `com.yvesvogl.crypta` bundle
identifier — a relocatable install would be free to drop the AU on top of
whichever copy the system finds first instead of the canonical plug-in folder.

> **Reading `PackageInfo`:** relocation is decided by the `<relocate>` element,
> **not** by the `relocatable="…"` attribute on `<pkg-info>`. That attribute
> reads `relocatable="false"` either way, so asserting on it proves nothing.
> `<relocate/>` (empty) means pinned; `<relocate><bundle id="…"/></relocate>`
> means the installer may follow an existing copy elsewhere on disk. The
> script's self-check asserts on the element.

#### Signed vs unsigned

The `.pkg` **always builds and always ships**. What varies is whether it is
signed and notarized, and the filename says which:

| Installer certificate | Asset | Signed | Notarized + stapled | User experience |
|---|---|---|---|---|
| present | `crypta-vX.Y.Z-macos.pkg` | yes | yes | opens normally |
| absent (today) | `crypta-vX.Y.Z-macos-unsigned.pkg` | no | no | one right-click → **Open** on first launch |

Shipping the unsigned installer is a deliberate choice, and it is **not** the
same as shipping unsigned plug-ins. Every bundle inside the payload carries its
own Developer ID signature and its own stapled notarization ticket, so once
installed the plug-ins are fully Gatekeeper-clean and load in a DAW without any
warning. Only the installer wrapper is unsigned. Compared with the zip — where
the user drags two bundles into two `/Library` folders by hand — an installer
that costs one right-click is the better default, not a worse one.

The structural assertions that *do* apply without a certificate run on every
build, signed or not, inside `build-macos-pkg.sh`: each component's
`install-location`, an empty `<relocate>`, the expected payload bundle, and
three install choices. `verify-macos-signing.sh` is deliberately **not** run
against an unsigned `.pkg`, because it would fail all three gates by design.

See issue #31 and the [signing secrets](#signing-secrets) table for what
switches this to the signed row.

#### Verified signature survival

Repackaging is the failure mode worth proving against: a copy that re-archives
rather than copies can strip an extended attribute, and a bundle whose sealed
contents change by one byte fails `codesign --verify --strict`. It does not
happen here, and that is measured rather than assumed. Against the shipped,
notarized, stapled `v0.4.0` artefacts:

- Every regular file in all three bundles hashes **identically** before
  packaging and after `pkgutil --expand-full` of the resulting `.pkg`. The
  staple ticket is an ordinary file inside the bundle
  (`Contents/CodeResources`, distinct from `Contents/_CodeSignature/`
  `CodeResources`), so it travels with the payload like any other.
- `verify-macos-signing.sh` reports 9/9 `[PASS]` and exits 0 on the bundles
  extracted back out of the `.pkg` — the same result as on the bundles
  straight out of the release zip. `codesign -dv` on the extracted AU still
  shows `Authority=Developer ID Application: Yves Vogl (M5WT732AY5)` and the
  original timestamp.

To reproduce:

```sh
ditto -x -k crypta-v0.4.0-macos.zip stage
.github/scripts/build-macos-pkg.sh --stage stage --version 0.4.0 --out /tmp/c.pkg
pkgutil --expand-full /tmp/c.pkg /tmp/c-expanded
.github/scripts/verify-macos-signing.sh \
  /tmp/c-expanded/au.pkg/Payload/Crypta.component \
  /tmp/c-expanded/vst3.pkg/Payload/Crypta.vst3 \
  /tmp/c-expanded/app.pkg/Payload/Crypta.app
```

**Still unverified:** a real `installer -pkg … -target /` system install, which
requires root. `pkgutil --expand-full` plus the `install-location` and
`<relocate>` assertions stand in for it — they confirm what the payload is and
where the installer is instructed to put it, but not the install itself.

#### Signing order

`--deep` is never used anywhere in this pipeline — it lets `codesign`
silently re-sign whatever it finds nested inside a bundle in one pass,
which papers over exactly the ordering bugs this note exists to avoid.
Crypta's bundles (`.component`, `.vst3`, `.app`) are flat: a single Mach-O
binary under `Contents/MacOS`, no embedded frameworks or helper tools, so
there is no nested code that needs its own signing pass before the bundle
itself. The order that matters is between artefact kinds, and the workflow
already applies it correctly:

1. **Sign each of the three bundles directly** with the Developer ID
   Application identity (`codesign --force --options runtime --timestamp
   --sign "$IDENTITY"`), one call per bundle — this *is* "inner before
   outer" for a flat bundle, since the bundle is both the innermost and
   outermost signable unit.
2. **Notarize and staple those bundles** (`notarytool submit --wait`, then
   `stapler staple`) before they go into the `.pkg` payload — so the AU,
   VST3 and standalone app carry their own valid Gatekeeper tickets
   independently of the installer that delivers them.
3. **Build the component packages** (`pkgbuild`) from the already-signed,
   notarized, stapled bundles. Component packages are not themselves
   code-signed — that is not a gap, it is how Apple's flat-package format
   works: `pkgbuild` component output is inert payload, and Installer.app
   only checks the signature on the top-level product package it opens.
4. **Assemble the distribution product** (`productbuild --distribution`)
   from the component packages.
5. **Sign the whole product package once**, at the outermost level
   (`productsign --sign "$INSTALLER_IDENTITY"`) — the *only* signature a
   `.pkg` needs or gets.
6. **Notarize and staple the signed `.pkg`** as its own submission,
   separate from the bundle notarization in step 2.

So for the bundles the rule is "sign each flat bundle once, notarize and
staple it before it is copied anywhere else"; for the `.pkg` the rule is
"never sign the component packages, sign only the final product package,
after assembly, before its own separate notarization pass."

#### Verifying an artefact

[`.github/scripts/verify-macos-signing.sh`](../.github/scripts/verify-macos-signing.sh)
checks all three Gatekeeper gates independently against any built artefact
(a `.component`/`.vst3`/`.app` bundle, or a `.pkg`):

```sh
.github/scripts/verify-macos-signing.sh path/to/Crypta.component path/to/Crypta.vst3 path/to/Crypta.app
.github/scripts/verify-macos-signing.sh path/to/crypta-vX.Y.Z-macos.pkg
```

It reports each gate — `codesign --verify --strict --verbose=4` (or
`pkgutil --check-signature` for a `.pkg`), `spctl --assess`, and
`xcrun stapler validate` — separately, so an artefact that fails can be
diagnosed at the exact gate it fails: an unsigned bundle fails all three
for "unsigned" reasons; a bundle signed with a real Developer ID but not
yet notarized passes `codesign` and fails `spctl`/`stapler` with
"Unnotarized Developer ID"; a fully signed, notarized, stapled artefact
passes all three. `release.yml` runs it on the bundles right after
stapling them, and again on the `.pkg` right after stapling it, so a
regression in any gate fails the release job instead of shipping a
Gatekeeper-blocked artefact.

The `spctl` gate picks its assessment type from the artefact kind, and the
choice is not cosmetic. `--type execute` asks "may this be launched as an
application?", which a `.component` or `.vst3` can never satisfy — they are
loadable bundles with no executable stub, so Gatekeeper answers
`rejected (the code is valid but does not seem to be an app)` no matter how
impeccably they are signed and notarized. The script therefore uses
`--type install` for plug-in bundles and for a `.pkg`, and `--type execute`
only for the standalone `.app`. Measured against the shipped `v0.4.0`
artefacts, the same two bundles that `--type execute` rejects are reported by
`--type install` as `accepted source=Notarized Developer ID`.

### Windows signing

Windows binaries ship unsigned; users get a SmartScreen warning on first run.
Authenticode signing needs a paid code-signing certificate, which was declined
for this phase — see issue #55 for the decision and what the wiring would look
like once a certificate exists.
