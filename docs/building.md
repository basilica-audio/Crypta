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
   throwaway keychain, notarized via `notarytool`, stapled, and zipped.
3. **`release-windows`** — VST3 and Standalone, zipped. **Unsigned** — see the
   note below.

Every uploaded archive is accompanied by a `.sha256` file so a download can be
verified with `shasum -a 256 -c <asset>.sha256`.

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
| `APPLE_INSTALLER_CERT_P12` | org secret — **not yet set** | base64 Developer ID **Installer** certificate, needed for the `.pkg` |
| `APPLE_INSTALLER_CERT_PASSWORD` | org secret — **not yet set** | password for the above p12 |

### macOS `.pkg` installer

The workflow contains a complete `pkgbuild` / `productbuild` /
`productsign` / `notarytool` / `stapler` path that produces a three-choice
distribution installer:

| Choice | Installs | Destination |
|---|---|---|
| Audio Unit (AU) | `Crypta.component` | `/Library/Audio/Plug-Ins/Components` |
| VST3 | `Crypta.vst3` | `/Library/Audio/Plug-Ins/VST3` |
| Standalone app | `Crypta.app` | `/Applications` |

Relocation is switched off on every component (`BundleIsRelocatable = false`),
because all three JUCE bundles share the `com.yvesvogl.crypta` bundle
identifier — a relocatable install would be free to drop the AU on top of
whichever copy the system finds first instead of the canonical plug-in folder.

The step is **inert until an installer certificate exists**: a `.pkg` is only
worth shipping if it is signed with a *Developer ID Installer* certificate,
because `notarytool` refuses an unsigned `.pkg` and Gatekeeper blocks an
un-notarized installer — strictly worse for users than the plain zip that ships
today. "Developer ID Installer" is a separate Apple certificate type from the
"Developer ID Application" certificate this repo already uses; having one does
not give you the other. When `APPLE_INSTALLER_CERT_P12` is absent the step logs
a notice and exits 0, leaving the signed/notarized zip untouched. See issue #31.

### Windows signing

Windows binaries ship unsigned; users get a SmartScreen warning on first run.
Authenticode signing needs a paid code-signing certificate, which was declined
for this phase — see issue #55 for the decision and what the wiring would look
like once a certificate exists.
