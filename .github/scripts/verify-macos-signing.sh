#!/usr/bin/env bash
# verify-macos-signing.sh — honest three-gate verification for a built macOS
# artefact (a .component / .vst3 / .app bundle, or a .pkg installer).
#
# Gates, run independently so a failure in one does not hide the others:
#   1. codesign --verify --strict --verbose=4 (bundles: .component/.vst3/.app)
#      or pkgutil --check-signature (a .pkg is a flat installer package, not
#      Mach-O code — codesign has no signature to verify on it; pkgutil is the
#      correct tool for a productsign signature on an installer)
#   2. spctl --assess                            (Gatekeeper's own opinion —
#                                                 requires notarization, not
#                                                 just a valid signature;
#                                                 --type install for a .pkg and
#                                                 for plug-in bundles, --type
#                                                 execute only for a .app —
#                                                 see the note at the gate)
#   3. stapler validate                          (a notarization ticket is
#                                                 physically stapled to the
#                                                 artefact, so Gatekeeper can
#                                                 assess it offline)
#
# Exit codes:
#   0  — all applicable gates pass
#   1  — at least one gate that should have passed did not (a real defect)
#   2  — usage error
#
# An UNSIGNED artefact is not a usage error: every gate is expected to fail
# for very specific, distinguishable reasons, and this script says so gate by
# gate instead of dying on the first codesign error. That is deliberate — see
# issue #31: this script must be able to report honestly on an unsigned build,
# because that is the state most local/CI runs exercise until the org holds a
# Developer ID Installer certificate.
#
# Usage:
#   verify-macos-signing.sh <path-to-.component-or-.vst3-or-.app-or-.pkg> [...]
#
# Machine-readable summary: pass --json to additionally emit one JSON object
# per artefact to stdout (in addition to the human-readable report on stderr).

set -u -o pipefail

JSON_OUT=0
ARGS=()
for arg in "$@"; do
  case "$arg" in
    --json) JSON_OUT=1 ;;
    *) ARGS+=("$arg") ;;
  esac
done

if [ "${#ARGS[@]}" -eq 0 ]; then
  echo "usage: $0 [--json] <artefact> [<artefact> ...]" >&2
  echo "  <artefact> is a .component, .vst3, .app bundle, or a .pkg installer" >&2
  exit 2
fi

overall_rc=0

# gate_result NAME STATUS DETAIL
# STATUS is one of: pass fail not-applicable
gate_result() {
  local name="$1" status="$2" detail="$3"
  case "$status" in
    pass) printf '  [PASS] %-11s %s\n' "$name" "$detail" >&2 ;;
    fail) printf '  [FAIL] %-11s %s\n' "$name" "$detail" >&2 ;;
    *)    printf '  [----] %-11s %s\n' "$name" "$detail" >&2 ;;
  esac
}

verify_bundle() {
  local target="$1"
  local is_pkg=0
  case "$target" in
    *.pkg) is_pkg=1 ;;
  esac

  local codesign_status spctl_status stapler_status
  local codesign_detail spctl_detail stapler_detail
  local codesign_out spctl_out stapler_out rc
  local gate1_name=codesign

  echo "== $target ==" >&2

  if [ "$is_pkg" -eq 1 ]; then
    # --- Gate 1 (pkg variant): pkgutil --check-signature ---------------------
    # A .pkg is a flat installer archive, not Mach-O code; codesign has
    # nothing to verify on it. pkgutil --check-signature is the equivalent
    # check for a productsign signature on an installer.
    gate1_name=pkgutil
    codesign_out=$(pkgutil --check-signature "$target" 2>&1)
    rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$codesign_out" | grep -qi "signed by a developer id installer"; then
      codesign_status=pass
      codesign_detail="signed by a Developer ID Installer identity"
    elif printf '%s' "$codesign_out" | grep -qi "not signed"; then
      codesign_status=fail
      codesign_detail="unsigned (expected without APPLE_INSTALLER_CERT_P12 — see issue #31)"
    else
      codesign_status=fail
      codesign_detail="$(printf '%s' "$codesign_out" | tail -1)"
    fi
  else
    # --- Gate 1: codesign --verify --strict --verbose=4 -----------------------
    # `--deep` is deliberately never used anywhere in this repo's signing path:
    # it lets codesign silently paper over inner bundles/binaries that were
    # signed with the wrong identity or not at all, by re-signing everything in
    # one pass instead of verifying what was already applied deliberately,
    # inside-out, in the release workflow. --strict additionally catches
    # structural issues (extra/missing files vs the sealed manifest) that a
    # non-strict verify accepts.
    codesign_out=$(codesign --verify --strict --verbose=4 "$target" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
      codesign_status=pass
      codesign_detail="valid signature, satisfies strict validation"
    elif printf '%s' "$codesign_out" | grep -qi "code object is not signed at all"; then
      codesign_status=fail
      codesign_detail="unsigned (expected on a plain dev build — see notes)"
    else
      codesign_status=fail
      codesign_detail="$(printf '%s' "$codesign_out" | tail -1)"
    fi
  fi
  gate_result "$gate1_name" "$codesign_status" "$codesign_detail"

  # --- Gate 2: spctl --assess -------------------------------------------------
  # Gatekeeper's own verdict. This is a strictly higher bar than gate 1: a
  # bundle can carry a perfectly valid ad-hoc or Developer ID signature and
  # still fail spctl if it has not been notarized (or, offline, if the
  # notarization ticket has not been stapled — see gate 3).
  #
  # Assessment type matters and is not cosmetic. `--type execute` asks "may
  # this be launched as an application?", which a .component or .vst3 can
  # never satisfy - they are loadable bundles with no LSMinimumSystemVersion
  # or executable stub, so Gatekeeper answers `rejected (the code is valid but
  # does not seem to be an app)` (exit 3) no matter how impeccably they are
  # signed and notarized. Assessing a plug-in bundle that way turns this gate
  # into a guaranteed false negative. `--type install` is the assessment a
  # host application's plug-in scan actually corresponds to, and it reports
  # `accepted source=Notarized Developer ID` for exactly the same bundles.
  # Measured against the shipped v0.4.0 artefacts, issue #31.
  local spctl_type=execute
  case "$target" in
    *.pkg)              spctl_type=install ;;
    *.component|*.vst3) spctl_type=install ;;
  esac
  spctl_out=$(spctl --assess --type "$spctl_type" --verbose=4 "$target" 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then
    spctl_status=pass
    spctl_detail="accepted"
  elif printf '%s' "$spctl_out" | grep -qi "no usable signature"; then
    spctl_status=fail
    spctl_detail="unsigned"
  elif printf '%s' "$spctl_out" | grep -qi "not signed at all"; then
    spctl_status=fail
    spctl_detail="unsigned"
  else
    spctl_status=fail
    spctl_detail="rejected: $(printf '%s' "$spctl_out" | tail -1)"
  fi
  gate_result spctl "$spctl_status" "$spctl_detail"

  # --- Gate 3: stapler validate -----------------------------------------------
  stapler_out=$(xcrun stapler validate "$target" 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then
    stapler_status=pass
    stapler_detail="notarization ticket stapled"
  else
    stapler_status=fail
    stapler_detail="no valid staple (unsigned, un-notarized, or notarized-but-not-stapled): $(printf '%s' "$stapler_out" | tail -1)"
  fi
  gate_result stapler "$stapler_status" "$stapler_detail"

  if [ "$codesign_status" != pass ] || [ "$spctl_status" != pass ] || [ "$stapler_status" != pass ]; then
    overall_rc=1
  fi

  if [ "$JSON_OUT" -eq 1 ]; then
    printf '{"artefact":"%s","%s":"%s","spctl":"%s","stapler":"%s"}\n' \
      "$target" "$gate1_name" "$codesign_status" "$spctl_status" "$stapler_status"
  fi
}

for target in "${ARGS[@]}"; do
  if [ ! -e "$target" ]; then
    echo "::error::$target does not exist" >&2
    overall_rc=1
    continue
  fi
  verify_bundle "$target"
done

exit $overall_rc
