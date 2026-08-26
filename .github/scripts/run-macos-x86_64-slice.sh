#!/usr/bin/env bash
#
# Execute the macOS Universal Binary's x86_64 slice of the test suite.
#
# Usage: run-macos-x86_64-slice.sh <tests-binary> [extra Catch2 arguments...]
#
# Why this exists
# ---------------
# ci.yml configures macOS with -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64", so the
# Tests binary is a Universal Binary. macOS then runs the slice that matches the
# host, and every GitHub-hosted macOS runner is Apple Silicon - so the x86_64
# slice was compiled, linked, uploaded inside the artefact, and never executed
# on any commit. That is issue #104.
#
# It is not a hypothetical gap. Both defects closed by PR #102 were found by
# building for x86_64 by hand and running the result under Rosetta, and neither
# was reachable by any job the matrix ran at the time:
#
#   - #99  juce::dsp's denormal guard, util::snapToZero() (JUCE 8.0.14,
#          juce_dsp/juce_dsp.h), is `#if JUCE_INTEL` and zeroes a filter's state
#          at the END of every process() call, so the snap events landed on the
#          host's block boundaries: change the buffer size, change the output.
#          Worst case -75.90 dB / 1.3437e-03 peak; bit-exact across all 8017
#          assertions after JUCE_DSP_ENABLE_SNAP_TO_ZERO=0.
#   - #100 tests/GoldenRenderTests.cpp gated its sample-exact memcmp branch on
#          `#if JUCE_MAC` where the property that decides it is the
#          architecture. A macOS x86_64 build takes the strict branch and fails.
#
# windows-latest is a genuine x86_64 target and runs the whole suite, which is
# what proves #99's fix on real Intel hardware today. What nothing covered is
# the Apple-clang-on-Intel combination that the shipped Universal Binary
# actually contains.
#
# What running under Rosetta does and does not prove
# --------------------------------------------------
# The instructions executed here are the ones Apple clang emitted for the
# x86_64 slice - the same SSE/FMA selection, the same libm entry points, the
# same denormal-control semantics the compiled code asks for - so the code path
# under test is the one that ships. Rosetta then translates those instructions
# to run on Arm hardware, so this is not a measurement of Intel silicon;
# windows-latest is, and stays in the matrix for that reason. Empirically this
# route is sufficient: it reproduced the Windows/MSVC block-schedule failures
# configuration for configuration to within 0.2 dB, which is the experiment
# that identified #99's cause.
#
# What it runs
# ------------
# The whole suite, in one process, over exactly the set of test cases ctest runs
# on the arm64 slice - Catch2 hides the `[.cpu]` wall-clock cases from both by
# default, so the two selections are the same. One process rather than
# catch_discover_tests' one-process-per-case, because ~250 Rosetta process
# starts cost more than the tests themselves do.
#
# Before running, it asserts rather than assumes:
#   1. the binary really contains an x86_64 slice;
#   2. Rosetta is present, installing it if the image does not ship it;
#   3. the child process really was translated, so this can never silently
#      degrade into a second arm64 run wearing an x86_64 label;
#   4. both slices register the identical set of test cases - a TEST_CASE
#      behind an `#if` on the architecture would otherwise simply not exist in
#      this run, and "All tests passed" would be printed anyway. That is #100's
#      shape, one level up.

set -euo pipefail

BINARY=${1:?usage: run-macos-x86_64-slice.sh <tests-binary> [extra Catch2 arguments...]}
shift

fail() {
  echo "::error::macOS x86_64 slice: ${1}"
  exit 1
}

note() { echo "==> $*"; }

# --- 1. The binary must contain the slice this script claims to run. ----------
# If CMAKE_OSX_ARCHITECTURES ever loses x86_64, `arch -x86_64` fails with a bare
# "Bad CPU type in executable"; naming the cause here is cheaper to read.
[ -x "$BINARY" ] || fail "no executable at '${BINARY}'."

ARCHS=$(lipo -archs "$BINARY" 2>/dev/null || true)
note "lipo -archs ${BINARY}: ${ARCHS:-<none>}"
case " $ARCHS " in
  *" x86_64 "*) ;;
  *) fail "'${BINARY}' has no x86_64 slice (found: ${ARCHS:-none}). The macOS configure step must keep -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\"." ;;
esac

# --- 2. Rosetta must be available. --------------------------------------------
# GitHub's macOS arm64 image does not document Rosetta as installed, so this
# probes for it and installs it only if it is missing, rather than assuming
# either way. The installer is a no-op when it is already present, and it adds
# no billed resource - it is an Apple download onto a runner already running.
if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  note "Rosetta 2 is not available on this image - installing it."
  sudo softwareupdate --install-rosetta --agree-to-license
fi

arch -x86_64 /usr/bin/true 2>/dev/null \
  || fail "cannot execute x86_64 code even after attempting to install Rosetta 2."

# --- 3. The child must really be translated. ----------------------------------
# Without this, a future macOS or `arch` change that silently ran the native
# slice would turn this step into a second arm64 run reported as x86_64
# coverage - a coverage claim rather than coverage.
CHILD_ARCH=$(arch -x86_64 /usr/bin/uname -m)
CHILD_TRANSLATED=$(arch -x86_64 /usr/sbin/sysctl -n sysctl.proc_translated 2>/dev/null || echo "?")
note "translated child reports: uname -m=${CHILD_ARCH}, sysctl.proc_translated=${CHILD_TRANSLATED}"

[ "$CHILD_ARCH" = "x86_64" ] \
  || fail "a child started with 'arch -x86_64' reports uname -m=${CHILD_ARCH}; it is not running the x86_64 slice."

# sysctl.proc_translated is 1 only under Rosetta. On a genuine Intel host it
# reads 0, which is equally correct - the slice would then be running natively -
# so it is reported rather than asserted on.

# --- 4. Both slices must register the same test cases. ------------------------
# Catch2 registers a TEST_CASE at static-initialisation time, so a case behind
# an `#if` on the architecture does not exist at all in the other slice, and a
# run that never contained it still prints "All tests passed". Listing costs
# milliseconds in both slices, so the set is compared rather than trusted.
LIST_ARM=$(mktemp)
LIST_X86=$(mktemp)
LIST_DIFF=$(mktemp)
trap 'rm -f "$LIST_ARM" "$LIST_X86" "$LIST_DIFF"' EXIT

arch -arm64  "$BINARY" --list-tests --verbosity quiet | LC_ALL=C sort > "$LIST_ARM"
arch -x86_64 "$BINARY" --list-tests --verbosity quiet | LC_ALL=C sort > "$LIST_X86"

if ! diff -u "$LIST_ARM" "$LIST_X86" > "$LIST_DIFF" 2>&1; then
  echo "::group::test cases registered by each slice (- arm64, + x86_64)"
  cat "$LIST_DIFF"
  echo "::endgroup::"
  fail "the two slices do not register the same test cases, so one architecture is not running what the other is."
fi

CASE_COUNT=$(grep -c . "$LIST_X86" || true)
note "both slices register the same ${CASE_COUNT} test cases"

# --- 5. Run it. ---------------------------------------------------------------
LOG_DIR=${SLICE_LOG_DIR:-x86_64-slice-logs}
mkdir -p "$LOG_DIR"
LOG="${LOG_DIR}/macos-x86_64-tests.log"

note "running the x86_64 slice: arch -x86_64 ${BINARY} $*"
START=$(date +%s)
set +e
arch -x86_64 "$BINARY" "$@" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e
ELAPSED=$(( $(date +%s) - START ))

RESULT_LINE=$(grep -E '^(All tests passed|test cases:|assertions:|Failed )' "$LOG" | tr '\n' ' ' || true)

note "x86_64 slice finished in ${ELAPSED}s with exit code ${RC}"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### macOS x86_64 slice (Rosetta)"
    echo
    echo "| | |"
    echo "|---|---|"
    echo "| binary | \`${BINARY}\` (\`${ARCHS}\`) |"
    echo "| translated | \`uname -m=${CHILD_ARCH}\`, \`sysctl.proc_translated=${CHILD_TRANSLATED}\` |"
    echo "| test cases | ${CASE_COUNT}, identical set on both slices |"
    echo "| runtime | ${ELAPSED}s |"
    echo "| result | ${RESULT_LINE:-see the job log} |"
  } >> "$GITHUB_STEP_SUMMARY"
fi

if [ "$RC" -ne 0 ]; then
  fail "the suite failed on x86_64 (exit code ${RC}) where the arm64 slice was green. See the log above - this is the class of defect issue #104 exists to surface."
fi

echo "macOS x86_64 slice OK: ${CASE_COUNT} test cases, ${ELAPSED}s, exit code 0."
