#!/usr/bin/env bash
#
# Crypta v1.0.0 QA gate - the machine-decidable half of docs/qa-checklist.md,
# as one command.
#
#   scripts/qa-gate.sh
#
# What this is
# ------------
# docs/qa-checklist.md splits the v1.0.0 gate (issue #34) in two: everything a
# machine can decide, and everything that needs ears, a DAW and taste. Part 1
# was prose pointing at CI. This script *is* Part 1: it builds every shipped
# format, runs every gate, records what each one measured, and exits non-zero
# if any of them failed. Its report is what gets pasted into the checklist's
# sign-off table, so the recorded result is a run that actually happened rather
# than a claim.
#
# It deliberately duplicates no logic that CI already owns:
#   - the Catch2 slices come from scripts/qa-gates.tsv, which is the single
#     place a gate is declared;
#   - the shipped formats are read out of CMakeLists.txt's FORMATS line, so the
#     gate cannot drift away from what is actually built;
#   - pluginval's pass/fail verdict is decided by .github/scripts/assert-pluginval-passed.sh,
#     the same script the workflow uses, at the same pinned version and the
#     same --strictness-level 10.
#
# What it does NOT decide
# -----------------------
# Anything in Part 2 of the checklist. No listening judgement is made, implied
# or approximated here. Two further things are out of reach on purpose:
#   - the installation smoke test, which needs the signed, notarized artefact
#     blocked behind #31 - this script runs against a local build/ output and
#     says so in its own report;
#   - the Standalone application's runtime behaviour. The Standalone is checked
#     for bundle structure only, and its signing state is recorded rather than
#     gated; nothing here launches a GUI application.
#
# Side effects
# ------------
# The AU gate installs the built .component into
# ~/Library/Audio/Plug-Ins/Components and restarts AudioComponentRegistrar,
# because macOS only resolves an AU that is registered with the system. The
# CMake build already does this (COPY_PLUGIN_AFTER_BUILD TRUE), so this is not
# a new side effect, but it is a real one: it replaces whatever Crypta was
# installed there.
#
# Exit code
# ---------
# 0 only if every selected gate passed. Any failure, and any gate whose filter
# matched no test case at all, is a non-zero exit.

set -euo pipefail

# ------------------------------------------------------------------------------
# Configuration
# ------------------------------------------------------------------------------

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

BUILD_DIR="$REPO_ROOT/build"
BUILD_CONFIG="Release"
REPORT_DIR=""
SKIP_BUILD=0
SKIP_VALIDATORS=0
SELECTED_GATES=""
GATE_MANIFEST="$SCRIPT_DIR/qa-gates.tsv"

# Pinned identically to .github/workflows/ci.yml. If either moves, both move.
PLUGINVAL_VERSION="v1.0.4"
PLUGINVAL_MACOS_SHA256="3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f"
PLUGINVAL_STRICTNESS="10"

# The plugin's four-character codes, as declared to juce_add_plugin().
AU_TYPE="aufx"
AU_SUBTYPE="Cryp"
AU_MANUFACTURER="Yvsv"

usage() {
    cat <<'USAGE'
Usage: scripts/qa-gate.sh [options]

Options:
  --build-dir DIR       CMake build directory (default: <repo>/build)
  --config CONFIG       Build configuration (default: Release)
  --report-dir DIR      Where to write the report (default: <build-dir>/qa-gate)
  --gates a,b,c         Run only these gates (default: all)
  --skip-build          Use the existing build as-is; do not configure or build
  --skip-validators     Skip pluginval and auval (no network, no AU install)
  --list-gates          Print every gate id and what it proves, then exit
  -h, --help            This message

Built-in gates (implemented in this script):
  formats               Every format in CMakeLists.txt's FORMATS line was built,
                        with the architectures the build asked for
  standalone-bundle     The Standalone .app is a structurally valid bundle; its
                        signing state is recorded, not gated (see #31)
  irs-manifest          Every shipped IR re-measured and checksum-matched against
                        resources/irs/manifest.json
  suite-full            ctest, the whole suite, exactly as CI runs it
  pluginval-vst3        pluginval --strictness-level 10 on the VST3
  pluginval-au          pluginval --strictness-level 10 on the AU (macOS)
  auval                 auval -strict (macOS)

Catch2 gates are declared in scripts/qa-gates.tsv; --list-gates prints them.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)      BUILD_DIR=$2; shift 2 ;;
        --config)         BUILD_CONFIG=$2; shift 2 ;;
        --report-dir)     REPORT_DIR=$2; shift 2 ;;
        --gates)          SELECTED_GATES=$2; shift 2 ;;
        --skip-build)     SKIP_BUILD=1; shift ;;
        --skip-validators) SKIP_VALIDATORS=1; shift ;;
        --list-gates)     LIST_ONLY=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

BUILD_DIR=$(mkdir -p "$BUILD_DIR" && cd -- "$BUILD_DIR" && pwd)
[ -n "$REPORT_DIR" ] || REPORT_DIR="$BUILD_DIR/qa-gate"
mkdir -p "$REPORT_DIR/logs"
REPORT_DIR=$(cd -- "$REPORT_DIR" && pwd)

# ------------------------------------------------------------------------------
# Result accumulation
#
# Parallel arrays rather than an associative array, because order is part of the
# report and bash 3.2 (the /bin/bash macOS ships) has no ordered maps.
# ------------------------------------------------------------------------------

GATE_IDS=()
GATE_STATUS=()
GATE_DETAIL=()
GATE_SECONDS=()
GATE_DESC=()
MEASUREMENTS=()

# Counted rather than derived from the array length: on the bash 3.2 that macOS
# ships, an empty array is "unset" and `set -u` aborts on the length expansion.
GATE_COUNT=0
MEASUREMENT_COUNT=0

# How many measurement lines a single gate may contribute to the report. The
# parameter sweep alone prints one per parameter per endpoint; the report is
# meant to be read.
MEASUREMENT_LIMIT=12
FAILURES=0

record() {
    # record <id> <status> <seconds> <detail> <description>
    GATE_IDS+=("$1")
    GATE_STATUS+=("$2")
    GATE_SECONDS+=("$3")
    GATE_DETAIL+=("$4")
    GATE_DESC+=("$5")
    GATE_COUNT=$((GATE_COUNT + 1))

    case "$2" in
        PASS) printf '  \033[32mPASS\033[0m  %-20s %s\n' "$1" "$4" ;;
        SKIP) printf '  \033[33mSKIP\033[0m  %-20s %s\n' "$1" "$4" ;;
        *)    printf '  \033[31mFAIL\033[0m  %-20s %s\n' "$1" "$4"; FAILURES=$((FAILURES + 1)) ;;
    esac
}

measure() {
    # measure <gate-id> <line>
    MEASUREMENTS+=("$1"$'\t'"$2")
    MEASUREMENT_COUNT=$((MEASUREMENT_COUNT + 1))
}

selected() {
    if [ -z "$SELECTED_GATES" ]; then return 0; fi
    case ",$SELECTED_GATES," in
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

now() { date +%s; }

load_average() {
    # One-minute load average, portable enough for macOS and Linux. Recorded
    # beside every timing-sensitive result on purpose: tests/CpuLoadTests.cpp
    # learned the hard way that a wall-clock number taken on a loaded machine
    # measures the machine's mood, not the plugin.
    uptime | sed -E 's/.*load averages?: *([0-9.]+).*/\1/'
}

# ------------------------------------------------------------------------------
# Gate manifest
# ------------------------------------------------------------------------------

if [ ! -f "$GATE_MANIFEST" ]; then
    echo "error: gate manifest not found at $GATE_MANIFEST" >&2
    exit 2
fi

if [ "${LIST_ONLY:-0}" = "1" ]; then
    echo "Catch2 gates (scripts/qa-gates.tsv):"
    while IFS=$'\t' read -r id filter measure_re description; do
        case "$id" in ''|\#*) continue ;; esac
        printf '  %-18s %-45s %s\n' "$id" "$filter" "$description"
    done < "$GATE_MANIFEST"
    echo
    usage
    exit 0
fi

# ------------------------------------------------------------------------------
# Environment
# ------------------------------------------------------------------------------

HOST_OS=$(uname -s)
HOST_ARCH=$(uname -m)
GIT_COMMIT=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY="clean"
if ! git -C "$REPO_ROOT" diff --quiet 2>/dev/null || ! git -C "$REPO_ROOT" diff --cached --quiet 2>/dev/null; then
    GIT_DIRTY="dirty"
fi
JUCE_TAG=$(grep -A2 'NAME JUCE' "$REPO_ROOT/CMakeLists.txt" | sed -nE 's/.*GIT_TAG[[:space:]]+([0-9A-Za-z._-]+).*/\1/p' | head -n 1)
PROJECT_VERSION=$(sed -nE 's/^project\(Crypta VERSION ([0-9.]+).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n 1)
LOAD_AT_START=$(load_average)
STARTED_AT=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

echo "==============================================================================="
echo "Crypta QA gate - docs/qa-checklist.md Part 1"
echo "==============================================================================="
echo "  repo            $REPO_ROOT"
echo "  commit          $GIT_COMMIT ($GIT_DIRTY)"
echo "  project version $PROJECT_VERSION"
echo "  JUCE            $JUCE_TAG"
echo "  host            $HOST_OS $HOST_ARCH, load average $LOAD_AT_START"
echo "  build dir       $BUILD_DIR ($BUILD_CONFIG)"
echo "  report          $REPORT_DIR"
echo

# ------------------------------------------------------------------------------
# Build
# ------------------------------------------------------------------------------

ARTEFACT_ROOT="$BUILD_DIR/Crypta_artefacts/$BUILD_CONFIG"
TESTS_BIN=""

build_gate() {
    local start; start=$(now)
    local log="$REPORT_DIR/logs/build.log"

    if [ "$SKIP_BUILD" = "1" ]; then
        record "build" "SKIP" "0" "--skip-build" "Configure and build every shipped format plus the test binary"
        return 0
    fi

    local generator=()
    if command -v ninja >/dev/null 2>&1; then generator=(-G Ninja); fi

    local rc=0
    {
        cmake -B "$BUILD_DIR" -S "$REPO_ROOT" "${generator[@]}" \
              -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" 2>&1
        cmake --build "$BUILD_DIR" --config "$BUILD_CONFIG" 2>&1
    } > "$log" 2>&1 || rc=$?

    local elapsed=$(( $(now) - start ))

    if [ "$rc" -ne 0 ]; then
        record "build" "FAIL" "$elapsed" "cmake failed, see logs/build.log" "Configure and build every shipped format plus the test binary"
        return 1
    fi

    record "build" "PASS" "$elapsed" "configured and built $BUILD_CONFIG" "Configure and build every shipped format plus the test binary"
}

locate_tests_binary() {
    local candidate
    for candidate in "$BUILD_DIR/Tests" "$BUILD_DIR/$BUILD_CONFIG/Tests" "$BUILD_DIR/$BUILD_CONFIG/Tests.exe" "$BUILD_DIR/Tests.exe"; do
        if [ -x "$candidate" ]; then TESTS_BIN="$candidate"; return 0; fi
    done
    return 1
}

# ------------------------------------------------------------------------------
# Built-in gate: every declared format was actually built
# ------------------------------------------------------------------------------

declared_formats() {
    # Read straight out of the build definition rather than being restated
    # here, so adding a format to juce_add_plugin() cannot leave this gate
    # quietly validating a subset of what ships.
    sed -nE 's/^[[:space:]]*FORMATS[[:space:]]+(.*)$/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n 1
}

format_extension() {
    case "$1" in
        AU)         echo "component" ;;
        AUv3)       echo "appex" ;;
        VST3)       echo "vst3" ;;
        VST)        echo "vst" ;;
        AAX)        echo "aaxplugin" ;;
        LV2)        echo "lv2" ;;
        Standalone) echo "app" ;;
        *)          echo "" ;;
    esac
}

VST3_PATH=""
COMPONENT_PATH=""
STANDALONE_PATH=""

find_format_artefact() {
    # <format> -> path, or empty. Bundles are directories on macOS and plain
    # files elsewhere, so both are accepted.
    local fmt=$1 ext path
    ext=$(format_extension "$fmt")
    [ -n "$ext" ] || return 0

    path=$(find "$ARTEFACT_ROOT" -maxdepth 2 \( -type d -o -type f \) -iname "*.$ext" 2>/dev/null | sort | head -n 1)

    if [ -z "$path" ] && [ "$fmt" = "Standalone" ]; then
        path=$(find "$ARTEFACT_ROOT" -maxdepth 3 -type f -perm -u+x -name "Crypta*" 2>/dev/null | sort | head -n 1)
    fi

    printf '%s' "$path"
}

resolve_artefacts() {
    # Run unconditionally, before any gate: --gates standalone-bundle must not
    # depend on --gates formats having run first to find the bundle for it.
    VST3_PATH=$(find_format_artefact VST3)
    COMPONENT_PATH=$(find_format_artefact AU)
    STANDALONE_PATH=$(find_format_artefact Standalone)
}

formats_gate() {
    local start; start=$(now)
    local formats; formats=$(declared_formats)
    local missing=() found=()

    if [ -z "$formats" ]; then
        record "formats" "FAIL" "0" "could not read FORMATS from CMakeLists.txt" "Every format declared to juce_add_plugin() was built"
        return 1
    fi

    local fmt ext path
    for fmt in $formats; do
        ext=$(format_extension "$fmt")

        if [ -z "$ext" ]; then
            missing+=("$fmt(unknown extension)")
            continue
        fi

        path=$(find_format_artefact "$fmt")

        if [ -z "$path" ]; then
            missing+=("$fmt")
            continue
        fi

        found+=("$fmt")

        # Record the architectures each shipped binary actually contains. A
        # release claiming a Universal Binary and shipping arm64-only is a
        # thing a checklist should be able to disprove.
        if [ "$HOST_OS" = "Darwin" ] && command -v lipo >/dev/null 2>&1; then
            local binary
            binary=$(find "$path" -type f -perm -u+x -path '*/MacOS/*' 2>/dev/null | head -n 1)
            [ -n "$binary" ] || binary="$path"
            if [ -f "$binary" ]; then
                measure "formats" "$fmt: $(lipo -archs "$binary" 2>/dev/null || echo 'not a Mach-O')"
            fi
        fi
    done

    local elapsed=$(( $(now) - start ))

    if [ ${#missing[@]} -gt 0 ]; then
        record "formats" "FAIL" "$elapsed" "declared but not built: ${missing[*]}" "Every format declared to juce_add_plugin() was built"
        return 1
    fi

    record "formats" "PASS" "$elapsed" "built: ${found[*]}" "Every format declared to juce_add_plugin() was built"
}

standalone_gate() {
    local start; start=$(now)
    local desc="The Standalone .app is a structurally valid bundle (it is not launched)"

    if [ "$HOST_OS" != "Darwin" ]; then
        record "standalone-bundle" "SKIP" "0" "macOS-only check" "$desc"
        return 0
    fi

    if [ -z "$STANDALONE_PATH" ] || [ ! -d "$STANDALONE_PATH" ]; then
        record "standalone-bundle" "FAIL" "0" "no Standalone .app found under $ARTEFACT_ROOT" "$desc"
        return 1
    fi

    local log="$REPORT_DIR/logs/standalone-bundle.log"
    local executable="$STANDALONE_PATH/Contents/MacOS/Crypta"
    local rc=0

    {
        echo "# plutil -lint"
        plutil -lint "$STANDALONE_PATH/Contents/Info.plist" 2>&1 || echo "PLIST_LINT_FAILED"
        echo "# file"
        file "$executable" 2>&1 || echo "EXECUTABLE_MISSING"
        echo "# codesign -dv"
        codesign -dv --verbose=2 "$STANDALONE_PATH" 2>&1 || echo "CODESIGN_READ_FAILED"
        echo "# codesign --verify"
        codesign --verify --strict "$STANDALONE_PATH" 2>&1 || echo "CODESIGN_VERIFY_FAILED"
    } > "$log" 2>&1

    grep -q 'PLIST_LINT_FAILED' "$log" && rc=1
    grep -q 'EXECUTABLE_MISSING' "$log" && rc=1
    grep -qE 'Mach-O' "$log" || rc=1

    local elapsed=$(( $(now) - start ))

    # The signing state is *recorded*, not gated, and here is why. A local
    # build is ad-hoc/linker-signed with no sealed resources, so
    # `codesign --verify` fails on it by construction - it would make this
    # gate permanently red on every developer machine while proving nothing
    # about the artefact users receive. Developer ID signing, notarization
    # and stapling are #31's gate and are asserted by
    # .github/scripts/verify-macos-signing.sh against the release artefact,
    # which is the only build where the question is meaningful.
    local signature authority
    signature=$(sed -nE 's/^Signature=(.*)$/\1/p' "$log" | head -n 1)
    authority=$(sed -nE 's/^Authority=(.*)$/\1/p' "$log" | head -n 1)
    [ -n "$signature" ] || signature="none"

    if [ -n "$authority" ]; then
        measure "standalone-bundle" "code signature: $signature, authority: $authority"

        # A build that claims a real signing authority must actually verify;
        # only the ad-hoc local case is exempt.
        if grep -q 'CODESIGN_VERIFY_FAILED' "$log"; then
            record "standalone-bundle" "FAIL" "$elapsed" "signed by '$authority' but codesign --verify failed" "$desc"
            return 1
        fi
    else
        measure "standalone-bundle" "code signature: $signature (local build; Developer ID signing is #31, gated by .github/scripts/verify-macos-signing.sh)"
    fi

    measure "standalone-bundle" "$(sed -nE 's/^Format=(.*)$/executable format: \1/p' "$log" | head -n 1)"

    if [ "$rc" -ne 0 ]; then
        record "standalone-bundle" "FAIL" "$elapsed" "see logs/standalone-bundle.log" "$desc"
        return 1
    fi

    record "standalone-bundle" "PASS" "$elapsed" "Info.plist lints, Mach-O executable present, signature: $signature" "$desc"
}

# ------------------------------------------------------------------------------
# Built-in gate: shipped impulse responses re-measured and checksum-matched
# ------------------------------------------------------------------------------

irs_gate() {
    local start; start=$(now)
    local desc="Bundled IRs re-measured and checksum-matched against their provenance record"
    local log="$REPORT_DIR/logs/irs-manifest.log"

    if ! command -v python3 >/dev/null 2>&1; then
        record "irs-manifest" "SKIP" "0" "python3 not on PATH" "$desc"
        return 0
    fi

    local rc=0
    python3 "$REPO_ROOT/tools/ir-synth/verify_irs.py" "$REPO_ROOT/resources/irs" \
        --expect-manifest "$REPO_ROOT/resources/irs/manifest.json" \
        --min-files 4 > "$log" 2>&1 || rc=$?

    local elapsed=$(( $(now) - start ))
    local count
    count=$(ls -1 "$REPO_ROOT/resources/irs"/*.wav 2>/dev/null | wc -l | tr -d ' ')
    measure "irs-manifest" "$count shipped .wav files verified against manifest.json"

    if [ "$rc" -ne 0 ]; then
        record "irs-manifest" "FAIL" "$elapsed" "see logs/irs-manifest.log" "$desc"
        return 1
    fi

    record "irs-manifest" "PASS" "$elapsed" "$count IRs verified" "$desc"
}

# ------------------------------------------------------------------------------
# Built-in gate: the whole suite through ctest, exactly as CI runs it
# ------------------------------------------------------------------------------

suite_gate() {
    local start; start=$(now)
    local desc="Every registered test case, through ctest, exactly as CI runs it"
    local log="$REPORT_DIR/logs/suite-full.log"
    local rc=0

    ctest --test-dir "$BUILD_DIR" --output-on-failure -C "$BUILD_CONFIG" > "$log" 2>&1 || rc=$?

    local elapsed=$(( $(now) - start ))
    local summary
    summary=$(grep -E '^[0-9]+% tests passed' "$log" | tail -n 1)
    [ -n "$summary" ] || summary="no ctest summary line"

    measure "suite-full" "$summary"
    measure "suite-full" "wall clock ${elapsed}s at load average $(load_average)"

    if [ "$rc" -ne 0 ]; then
        record "suite-full" "FAIL" "$elapsed" "$summary (see logs/suite-full.log)" "$desc"
        return 1
    fi

    record "suite-full" "PASS" "$elapsed" "$summary" "$desc"
}

# ------------------------------------------------------------------------------
# Catch2 slices, declared in scripts/qa-gates.tsv
# ------------------------------------------------------------------------------

catch_gate() {
    local id=$1 filter=$2 measure_re=$3 desc=$4
    local start; start=$(now)
    local log="$REPORT_DIR/logs/$id.log"
    local rc=0

    local args=("$filter")
    if [ "$measure_re" != "-" ]; then
        # -s makes Catch2 print the scoped INFO messages of *passing*
        # assertions too, which is where the measured figures live.
        args+=("-s")
    fi

    "$TESTS_BIN" "${args[@]}" > "$log" 2>&1 || rc=$?

    local elapsed=$(( $(now) - start ))

    # Catch2 v3 prints either "All tests passed (N assertions in M test cases)"
    # or a "test cases: ... | assertions: ..." block. Take whichever exists.
    local summary
    summary=$(grep -E 'All tests passed \(' "$log" | tail -n 1 | sed -E 's/^=*//' || true)
    if [ -z "$summary" ]; then
        summary=$(grep -E '^(test cases|assertions):' "$log" | tr '\n' ' ' | sed -E 's/[[:space:]]+/ /g' || true)
    fi
    [ -n "$summary" ] || summary="no Catch2 summary line"

    if [ "$measure_re" != "-" ]; then
        # Deduplicate: a gate spanning several sample rates prints the same
        # shape of line repeatedly, and the report wants the distinct facts,
        # not one line per repetition. Catch2's own scaffolding (the assertion
        # source line, the expansion, the file path) is dropped - only the
        # INFO text is a measurement.
        # Catch2 prints the name of each test case as a heading, and a name
        # like "T14: a dirac arrives..." is indistinguishable from a
        # measurement by pattern alone. Ask the binary for the names instead
        # of guessing at them.
        local names="$REPORT_DIR/logs/$id.names"
        "$TESTS_BIN" "$filter" --list-tests --verbosity quiet > "$names" 2>/dev/null || : > "$names"

        local matches total line
        matches=$(grep -hE "$measure_re" "$log" 2>/dev/null \
                  | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//' \
                  | grep -vE '^(CHECK|REQUIRE|CHECKED|WARN|with |PASSED|FAILED|Filters|Randomness|/|-|~|\.)' \
                  | grep -vE '!= nullptr|^$' \
                  | grep -vxF -f "$names" \
                  | grep -E '[0-9]|inf|nan' \
                  | sed -E 's@[[:space:]]*[/,]+$@@' \
                  | sort -u || true)
        total=$(printf '%s\n' "$matches" | grep -c . || true)

        if [ "${total:-0}" -gt "$MEASUREMENT_LIMIT" ]; then
            measure "$id" "_${total} distinct measurement lines; the $MEASUREMENT_LIMIT with the largest leading figure are shown._"
        fi

        # Sorted by the first number on the line, descending, so a truncated
        # list shows the worst cases rather than an arbitrary alphabetical slice.
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            measure "$id" "$line"
        done < <(printf '%s\n' "$matches" | sort -t: -k2 -gr | head -n "$MEASUREMENT_LIMIT")
    fi

    if [ "$rc" -ne 0 ]; then
        # rc != 0 also covers "no test cases matched", which is a gate that
        # stopped testing anything and must never read as a pass.
        record "$id" "FAIL" "$elapsed" "$summary (see logs/$id.log)" "$desc"
        return 1
    fi

    record "$id" "PASS" "$elapsed" "$summary" "$desc"
}

# ------------------------------------------------------------------------------
# Built-in gates: pluginval and auval
# ------------------------------------------------------------------------------

PLUGINVAL_BIN=""

fetch_pluginval() {
    local dir="$REPORT_DIR/pluginval"

    if [ "$HOST_OS" != "Darwin" ]; then
        return 1
    fi

    PLUGINVAL_BIN="$dir/pluginval.app/Contents/MacOS/pluginval"
    [ -x "$PLUGINVAL_BIN" ] && return 0

    mkdir -p "$dir"
    local zip="$dir/pluginval.zip"

    curl -sL -o "$zip" \
        "https://github.com/Tracktion/pluginval/releases/download/${PLUGINVAL_VERSION}/pluginval_macOS.zip" || return 1
    echo "${PLUGINVAL_MACOS_SHA256}  ${zip}" | shasum -a 256 -c - >/dev/null || return 1
    unzip -oq "$zip" -d "$dir" || return 1
    chmod +x "$PLUGINVAL_BIN"

    [ -x "$PLUGINVAL_BIN" ]
}

install_component() {
    [ -n "$COMPONENT_PATH" ] || return 1

    local components_dir="$HOME/Library/Audio/Plug-Ins/Components"
    local name; name=$(basename "$COMPONENT_PATH")

    mkdir -p "$components_dir"
    rm -rf "${components_dir:?}/${name:?}"
    cp -R "$COMPONENT_PATH" "$components_dir/"
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

    INSTALLED_COMPONENT="$components_dir/$name"
}

pluginval_gate() {
    local id=$1 target=$2 desc=$3
    local start; start=$(now)
    local log="$REPORT_DIR/logs/$id.log"

    if [ "$SKIP_VALIDATORS" = "1" ]; then
        record "$id" "SKIP" "0" "--skip-validators" "$desc"
        return 0
    fi

    if [ -z "$PLUGINVAL_BIN" ] || [ ! -x "$PLUGINVAL_BIN" ]; then
        record "$id" "FAIL" "0" "pluginval $PLUGINVAL_VERSION unavailable (download or checksum failed)" "$desc"
        return 1
    fi

    if [ -z "$target" ]; then
        record "$id" "FAIL" "0" "no artefact to validate" "$desc"
        return 1
    fi

    local rc=0
    "$PLUGINVAL_BIN" --strictness-level "$PLUGINVAL_STRICTNESS" --validate "$target" > "$log" 2>&1 || rc=$?

    local elapsed=$(( $(now) - start ))
    local verdict=""

    # The verdict is not ours to decide: the workflow's own assertion script
    # owns it, so a local run and a CI run cannot disagree about what "passed"
    # means.
    if "$REPO_ROOT/.github/scripts/assert-pluginval-passed.sh" "$log" "$rc" "$id" >> "$log" 2>&1; then
        verdict=$(grep -E '^pluginval \[' "$log" | tail -n 1)
        measure "$id" "strictness level $PLUGINVAL_STRICTNESS, pluginval $PLUGINVAL_VERSION: ${verdict:-SUCCESS}"
        record "$id" "PASS" "$elapsed" "${verdict:-SUCCESS}" "$desc"
        return 0
    fi

    verdict=$(grep -E '::error::' "$log" | tail -n 1)
    record "$id" "FAIL" "$elapsed" "${verdict:-see logs/$id.log}" "$desc"
    return 1
}

auval_gate() {
    local start; start=$(now)
    local desc="auval -strict accepts the installed AU component"
    local log="$REPORT_DIR/logs/auval.log"

    if [ "$HOST_OS" != "Darwin" ]; then
        record "auval" "SKIP" "0" "macOS-only" "$desc"
        return 0
    fi

    if [ "$SKIP_VALIDATORS" = "1" ]; then
        record "auval" "SKIP" "0" "--skip-validators" "$desc"
        return 0
    fi

    local rc=0
    auval -strict -v "$AU_TYPE" "$AU_SUBTYPE" "$AU_MANUFACTURER" > "$log" 2>&1 || rc=$?

    local elapsed=$(( $(now) - start ))
    local verdict
    verdict=$(grep -E 'AU VALIDATION (SUCCEEDED|FAILED)' "$log" | tail -n 1 | sed -E 's/^[[:space:]]*//')
    [ -n "$verdict" ] || verdict="no auval verdict line"

    measure "auval" "$verdict"

    if [ "$rc" -ne 0 ]; then
        record "auval" "FAIL" "$elapsed" "$verdict (see logs/auval.log)" "$desc"
        return 1
    fi

    record "auval" "PASS" "$elapsed" "$verdict" "$desc"
}

# ------------------------------------------------------------------------------
# Run
# ------------------------------------------------------------------------------

echo "Build"
if selected "build"; then build_gate || true; fi

if ! locate_tests_binary; then
    echo
    echo "error: no Tests binary under $BUILD_DIR - cannot run any Catch2 gate." >&2
    echo "       Run without --skip-build, or point --build-dir at a built tree." >&2
    exit 2
fi

resolve_artefacts

echo
echo "Artefacts"
if selected "formats"; then formats_gate || true; fi
if selected "standalone-bundle"; then standalone_gate || true; fi
if selected "irs-manifest"; then irs_gate || true; fi

echo
echo "Test suite"
if selected "suite-full"; then suite_gate || true; fi

echo
echo "Gates (scripts/qa-gates.tsv)"
while IFS=$'\t' read -r id filter measure_re description; do
    case "$id" in ''|\#*) continue ;; esac
    selected "$id" || continue
    catch_gate "$id" "$filter" "$measure_re" "$description" || true
done < "$GATE_MANIFEST"

echo
echo "Validators"
if [ "$SKIP_VALIDATORS" != "1" ] && [ "$HOST_OS" = "Darwin" ]; then
    fetch_pluginval || echo "  (pluginval unavailable - the gates below will fail)"
    install_component || true
fi

if selected "pluginval-vst3"; then pluginval_gate "pluginval-vst3" "$VST3_PATH" "pluginval --strictness-level $PLUGINVAL_STRICTNESS on the VST3" || true; fi

if [ "$HOST_OS" = "Darwin" ]; then
    if selected "pluginval-au"; then pluginval_gate "pluginval-au" "${INSTALLED_COMPONENT:-$COMPONENT_PATH}" "pluginval --strictness-level $PLUGINVAL_STRICTNESS on the AU" || true; fi
    if selected "auval"; then auval_gate || true; fi
fi

# ------------------------------------------------------------------------------
# Report
# ------------------------------------------------------------------------------

FINISHED_AT=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
LOAD_AT_END=$(load_average)
CPU_COUNT=$( (getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1) )

MD="$REPORT_DIR/report.md"
JSON="$REPORT_DIR/report.json"

{
    echo "# Crypta QA gate report"
    echo
    echo "Generated by \`scripts/qa-gate.sh\`. This is the machine-decidable half of"
    echo "\`docs/qa-checklist.md\` (issue #34). It decides nothing about how the plugin sounds."
    echo
    echo "| | |"
    echo "|---|---|"
    echo "| Commit | \`$GIT_COMMIT\` ($GIT_DIRTY) |"
    echo "| Project version | $PROJECT_VERSION |"
    echo "| JUCE | $JUCE_TAG |"
    echo "| Host | $HOST_OS $HOST_ARCH, $CPU_COUNT CPUs |"
    echo "| Load average (start / end) | $LOAD_AT_START / $LOAD_AT_END |"
    echo "| Build | $BUILD_DIR ($BUILD_CONFIG) |"
    echo "| pluginval | $PLUGINVAL_VERSION, \`--strictness-level $PLUGINVAL_STRICTNESS\` |"
    echo "| Started / finished (UTC) | $STARTED_AT / $FINISHED_AT |"
    echo

    awk -v load="$LOAD_AT_END" -v cpus="$CPU_COUNT" 'BEGIN {
        if (load + 0 > cpus + 0)
            print "> **This machine was loaded while the gate ran** (load average " load " against " cpus " CPUs).\n> Pass/fail verdicts are unaffected - none of them is a wall-clock assertion - but any\n> duration in the table below is noise, not a measurement.\n"
    }'

    echo "## Gates"
    echo
    echo "| Gate | Result | Time | Detail | Proves |"
    echo "|---|---|---|---|---|"
    i=0
    while [ $i -lt $GATE_COUNT ]; do
        printf '| `%s` | %s | %ss | %s | %s |\n' \
            "${GATE_IDS[$i]}" "${GATE_STATUS[$i]}" "${GATE_SECONDS[$i]}" \
            "$(printf '%s' "${GATE_DETAIL[$i]}" | sed 's/|/\\|/g')" \
            "$(printf '%s' "${GATE_DESC[$i]}" | sed 's/|/\\|/g')"
        i=$((i + 1))
    done
    echo

    if [ $MEASUREMENT_COUNT -gt 0 ]; then
        echo "## Measurements"
        echo
        echo "Figures the gates printed while passing, not thresholds they were held to."
        echo
        last=""
        for entry in "${MEASUREMENTS[@]}"; do
            gate=${entry%%$'\t'*}
            line=${entry#*$'\t'}
            if [ "$gate" != "$last" ]; then
                echo
                echo "**\`$gate\`**"
                echo
                last=$gate
            fi
            echo "- $line"
        done
        echo
    fi

    echo "## What this report does not cover"
    echo
    echo "- **The listening gate.** Every subjective item is in Part 2 of \`docs/qa-checklist.md\`"
    echo "  and none of it is decided here."
    echo "- **The installation smoke test.** This ran against a local \`$BUILD_CONFIG\` build, not"
    echo "  against a signed, notarized artefact - that is blocked on #31 (Developer ID secrets)."
    echo "- **Standalone runtime behaviour.** The Standalone was checked as a bundle; it was not launched."
    echo "- **CPU cost.** \`tests/CpuLoadTests.cpp\` (\`[.cpu]\`) is hidden from the default run on purpose:"
    echo "  a wall-clock assertion on a shared machine measures the machine."
} > "$MD"

{
    printf '{\n'
    printf '  "commit": "%s",\n' "$GIT_COMMIT"
    printf '  "worktree": "%s",\n' "$GIT_DIRTY"
    printf '  "project_version": "%s",\n' "$PROJECT_VERSION"
    printf '  "juce": "%s",\n' "$JUCE_TAG"
    printf '  "host_os": "%s",\n' "$HOST_OS"
    printf '  "host_arch": "%s",\n' "$HOST_ARCH"
    printf '  "cpu_count": %s,\n' "$CPU_COUNT"
    printf '  "load_average_start": %s,\n' "$LOAD_AT_START"
    printf '  "load_average_end": %s,\n' "$LOAD_AT_END"
    printf '  "pluginval_version": "%s",\n' "$PLUGINVAL_VERSION"
    printf '  "pluginval_strictness": %s,\n' "$PLUGINVAL_STRICTNESS"
    printf '  "started_at": "%s",\n' "$STARTED_AT"
    printf '  "finished_at": "%s",\n' "$FINISHED_AT"
    printf '  "failures": %s,\n' "$FAILURES"
    printf '  "gates": [\n'
    i=0
    while [ $i -lt $GATE_COUNT ]; do
        sep=","
        [ $((i + 1)) -eq $GATE_COUNT ] && sep=""
        printf '    { "id": "%s", "status": "%s", "seconds": %s, "detail": "%s" }%s\n' \
            "${GATE_IDS[$i]}" "${GATE_STATUS[$i]}" "${GATE_SECONDS[$i]}" \
            "$(printf '%s' "${GATE_DETAIL[$i]}" | sed 's/\\/\\\\/g; s/"/\\"/g')" "$sep"
        i=$((i + 1))
    done
    printf '  ]\n'
    printf '}\n'
} > "$JSON"

echo
echo "==============================================================================="
printf 'Gates: %d run, %d failed\n' "$GATE_COUNT" "$FAILURES"
echo "Report: $MD"
echo "        $JSON"
echo "==============================================================================="

[ "$FAILURES" -eq 0 ] || exit 1
