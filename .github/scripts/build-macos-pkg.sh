#!/usr/bin/env bash
# build-macos-pkg.sh — assemble the macOS .pkg installer for Crypta (issue #31).
#
# Takes the staging tree that release.yml has already signed, notarized and
# stapled, and wraps it in a distribution installer that drops each format in
# the location a DAW actually scans:
#
#   stage/AU/*.component     -> /Library/Audio/Plug-Ins/Components
#   stage/VST3/*.vst3        -> /Library/Audio/Plug-Ins/VST3
#   stage/Standalone/*.app   -> /Applications
#
# Signing is OPTIONAL and deliberately separated from assembly. A .pkg is
# signed with a **Developer ID Installer** certificate, which is a *different*
# Apple certificate type from the Developer ID *Application* certificate that
# signs the bundles — holding one does not give you the other, and a .pkg
# cannot be signed with the Application identity. When no installer identity is
# supplied this script still emits a structurally correct *unsigned* .pkg
# rather than producing nothing: the bundles inside it are already signed,
# notarized and stapled individually, so once installed the plug-ins are fully
# Gatekeeper-clean and load in a DAW without any warning. Only the installer
# wrapper is unsigned, costing the user one right-click -> Open on first
# launch. See docs/building.md, "macOS .pkg installer".
#
# This lives in a script rather than inline in release.yml so that the exact
# code path which ships can be run locally against real artefacts — the
# packaging half of issue #31 was verified that way.
#
# Usage:
#   build-macos-pkg.sh --stage DIR --version X.Y.Z --out PATH
#                      [--identity "Developer ID Installer: ..."]
#                      [--keychain PATH] [--work DIR]
#
# Exit codes:
#   0  — a .pkg was produced at --out (signed if --identity was given)
#   1  — assembly or signing failed
#   2  — usage error

set -euo pipefail

STAGE="" VERSION="" OUT="" IDENTITY="" KEYCHAIN="" WORK=""

die() { echo "::error::$*" >&2; exit 1; }
usage() {
  echo "usage: $0 --stage DIR --version X.Y.Z --out PATH [--identity NAME] [--keychain PATH] [--work DIR]" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --stage)    STAGE="${2:-}"; shift 2 ;;
    --version)  VERSION="${2:-}"; shift 2 ;;
    --out)      OUT="${2:-}"; shift 2 ;;
    --identity) IDENTITY="${2:-}"; shift 2 ;;
    --keychain) KEYCHAIN="${2:-}"; shift 2 ;;
    --work)     WORK="${2:-}"; shift 2 ;;
    -h|--help)  usage ;;
    *)          echo "unknown argument: $1" >&2; usage ;;
  esac
done

[ -n "$STAGE" ] && [ -n "$VERSION" ] && [ -n "$OUT" ] || usage
[ -d "$STAGE" ] || die "stage directory does not exist: $STAGE"

if [ -z "$WORK" ]; then
  WORK=$(mktemp -d "${TMPDIR:-/tmp}/crypta-pkg.XXXXXX")
fi
mkdir -p "$WORK/root/au" "$WORK/root/vst3" "$WORK/root/app" "$WORK/comp"

# ---------------------------------------------------------------------------
# Collect the payload. Each format gets its own component root, because each
# one needs a different --install-location.
#
# cp -R (not ditto, not tar) preserves the bundles byte-for-byte including the
# embedded LC_CODE_SIGNATURE and the stapled notarization ticket that stapler
# wrote to Contents/CodeResources — both are ordinary bytes inside the bundle,
# so they travel with the payload through pkgbuild. That is asserted, not
# assumed: see the "signature survival" checks in docs/building.md.
# ---------------------------------------------------------------------------
collect() {
  local label="$1" src_glob="$2" dest_dir="$3"
  local matches=()
  # Word splitting on the glob is intentional: it arrives as an unexpanded
  # pattern so that this function, not the caller, decides how to fail.
  # shellcheck disable=SC2206
  matches=( $src_glob )
  if [ "${#matches[@]}" -eq 0 ] || [ ! -e "${matches[0]}" ]; then
    die "no $label artefact found matching $src_glob"
  fi
  cp -R "${matches[@]}" "$dest_dir/"
  echo "  $label: $(basename "${matches[0]}")"
}

echo "Collecting payload from $STAGE:"
collect AU         "$STAGE/AU/*.component"   "$WORK/root/au"
collect VST3       "$STAGE/VST3/*.vst3"      "$WORK/root/vst3"
collect Standalone "$STAGE/Standalone/*.app" "$WORK/root/app"

# ---------------------------------------------------------------------------
# Component packages.
#
# BundleIsRelocatable is switched off on every component. All three JUCE
# bundles carry the same CFBundleIdentifier (com.yvesvogl.crypta), so a
# relocatable install would be free to drop the AU on top of whichever copy
# the system happens to find first instead of the canonical plug-in folder.
#
# Component packages are never code-signed themselves — that is not a gap, it
# is how Apple's flat-package format works: pkgbuild output is inert payload,
# and Installer.app only checks the signature on the top-level product package.
# ---------------------------------------------------------------------------
build_component() {
  local name="$1" root="$2" dest="$3" ident="$4"
  local plist="$WORK/comp/$name.plist"
  pkgbuild --analyze --root "$root" "$plist" >/dev/null
  /usr/libexec/PlistBuddy -c "Add :0:BundleIsRelocatable bool false" "$plist" >/dev/null 2>&1 \
    || /usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$plist" >/dev/null
  pkgbuild --root "$root" --component-plist "$plist" \
    --identifier "$ident" --version "$VERSION" \
    --install-location "$dest" "$WORK/comp/$name.pkg" >/dev/null
  echo "  $name.pkg -> $dest ($ident $VERSION)"
}

echo "Building component packages:"
build_component au   "$WORK/root/au"   "/Library/Audio/Plug-Ins/Components" "com.yvesvogl.crypta.au"
build_component vst3 "$WORK/root/vst3" "/Library/Audio/Plug-Ins/VST3"       "com.yvesvogl.crypta.vst3"
build_component app  "$WORK/root/app"  "/Applications"                      "com.yvesvogl.crypta.standalone"

# ---------------------------------------------------------------------------
# Distribution definition. Written to a real file rather than piped through an
# indented heredoc: an XML declaration must be the very first thing in the
# document, so leading whitespace ahead of `<?xml` is a parse error.
# ---------------------------------------------------------------------------
cat > "$WORK/comp/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Crypta $VERSION</title>
    <options customize="always" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
        <line choice="app"/>
    </choices-outline>
    <choice id="au" title="Audio Unit (AU)" description="Installs Crypta.component into /Library/Audio/Plug-Ins/Components."><pkg-ref id="com.yvesvogl.crypta.au"/></choice>
    <choice id="vst3" title="VST3" description="Installs Crypta.vst3 into /Library/Audio/Plug-Ins/VST3."><pkg-ref id="com.yvesvogl.crypta.vst3"/></choice>
    <choice id="app" title="Standalone app" description="Installs Crypta.app into /Applications."><pkg-ref id="com.yvesvogl.crypta.standalone"/></choice>
    <pkg-ref id="com.yvesvogl.crypta.au" version="$VERSION">au.pkg</pkg-ref>
    <pkg-ref id="com.yvesvogl.crypta.vst3" version="$VERSION">vst3.pkg</pkg-ref>
    <pkg-ref id="com.yvesvogl.crypta.standalone" version="$VERSION">app.pkg</pkg-ref>
</installer-gui-script>
XML

mkdir -p "$(dirname "$OUT")"
productbuild --distribution "$WORK/comp/distribution.xml" \
  --package-path "$WORK/comp" "$WORK/comp/product.pkg" >/dev/null

# ---------------------------------------------------------------------------
# Sign the whole product package once, at the outermost level — the only
# signature a .pkg needs or gets.
# ---------------------------------------------------------------------------
if [ -n "$IDENTITY" ]; then
  productsign_args=( --sign "$IDENTITY" )
  [ -n "$KEYCHAIN" ] && productsign_args+=( --keychain "$KEYCHAIN" )
  productsign "${productsign_args[@]}" "$WORK/comp/product.pkg" "$OUT"
  echo "Signed installer: $OUT"
  echo "  identity: $IDENTITY"
else
  cp "$WORK/comp/product.pkg" "$OUT"
  echo "::warning::Built an UNSIGNED .pkg - no Developer ID Installer identity was supplied (issue #31). The bundles inside are individually signed, notarized and stapled; only the installer wrapper is unsigned, so users must right-click -> Open it once."
  echo "Unsigned installer: $OUT"
fi

echo "Installer size: $(du -h "$OUT" | cut -f1)"

# ---------------------------------------------------------------------------
# Structural self-check.
#
# Runs unconditionally, signed or not: a .pkg that installs to the wrong place
# is a silent failure — Installer.app reports success and the DAW simply never
# finds the plug-in. Asserting install-location and relocatable=false here is
# the only thing standing between a typo and a release that looks fine.
#
# `pkgutil --expand-full` extracts the real cpio payload, so this also
# confirms the payload survived assembly. It does not re-verify the bundle
# signatures — release.yml does that on stage/ before packaging, and the
# payload is a byte-for-byte copy (cp -R, no re-archiving).
# ---------------------------------------------------------------------------
CHECK_DIR="$WORK/expanded"
rm -rf "$CHECK_DIR"
pkgutil --expand-full "$OUT" "$CHECK_DIR" >/dev/null

check_component() {
  local name="$1" want_dest="$2" want_payload="$3"
  local info="$CHECK_DIR/$name.pkg/PackageInfo"
  [ -f "$info" ] || die "$OUT is missing the $name component package"

  local got_dest
  got_dest=$(/usr/bin/xmllint --xpath 'string(/pkg-info/@install-location)' "$info")
  [ "$got_dest" = "$want_dest" ] \
    || die "$name component installs to '$got_dest', expected '$want_dest'"

  # Relocation is decided by the <relocate> element, NOT by the
  # relocatable="..." attribute on <pkg-info>. That attribute reads
  # relocatable="false" whether or not BundleIsRelocatable was set, so
  # asserting on it is vacuous - measured both ways, issue #31. The element is
  # the real signal: `<relocate/>` empty means pinned, `<relocate><bundle
  # id="..."/></relocate>` means the installer may follow an existing copy of
  # that bundle id somewhere else on disk. Crypta's three bundles all share
  # com.yvesvogl.crypta, so a populated <relocate> would let the AU land on
  # top of whichever copy the system finds first.
  local relocate_entries
  relocate_entries=$(/usr/bin/xmllint --xpath 'count(/pkg-info/relocate/bundle)' "$info")
  [ "$relocate_entries" = "0" ] \
    || die "$name component has $relocate_entries <relocate> bundle entry/entries; it must be pinned to $want_dest"

  [ -e "$CHECK_DIR/$name.pkg/Payload/$want_payload" ] \
    || die "$name component payload does not contain $want_payload"

  echo "  [OK] $name -> $want_dest ($want_payload, not relocatable)"
}

echo "Verifying installer structure:"
check_component au   "/Library/Audio/Plug-Ins/Components" "Crypta.component"
check_component vst3 "/Library/Audio/Plug-Ins/VST3"       "Crypta.vst3"
check_component app  "/Applications"                      "Crypta.app"

CHOICES=$(installer -pkginfo -pkg "$OUT" | tail -n +2 | grep -c .)
[ "$CHOICES" -eq 3 ] || die "$OUT offers $CHOICES install choices, expected 3"
echo "  [OK] distribution offers 3 install choices"
