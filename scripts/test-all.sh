#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/test-all.sh
#
# Cross-platform scope: this script targets Linux + macOS (POSIX
# shells).  Windows users should invoke it via WSL2 or run the
# underlying commands by hand -- the individual stages
# (`cmake --build`, `west twister`, `python -m pytest`,
# `clang-format`, `python scripts/validate_metadata.py`,
# `doxygen`) are all cross-platform on their own; this script is
# just a one-shot wrapper.  See docs/cross-platform-setup.md
# section 4 for native PowerShell equivalents.
#
# Single-command verifier for the Alp SDK.  Runs every test surface
# the project has locally-runnable (no HIL):
#
#   1. Plain-CMake / Yocto build + ctest
#   2. Plain-CMake / baremetal build (compile-only -- no tests yet)
#   3. Zephyr twister (skipped if ZEPHYR_BASE is unset)
#   4. clang-format diff vs HEAD~1 (skipped if no clang-format)
#   5. board.yaml metadata schema validate
#   6. Public/private text classifier
#   7. Doxygen zero-warnings build (skipped if no Doxygen)
#
# Each stage prints `[stage] PASS` or `[stage] FAIL`; the script
# returns non-zero if any required stage failed.  Skipped stages
# don't fail the run; they're logged as `[stage] SKIP <reason>`.
#
# Flags:
#   --quick           skip twister + Doxygen (the slow stages)
#   --yocto-only      run only stage 1 + format + metadata
#   --zephyr-only     run only stage 3 (requires ZEPHYR_BASE)
#   --no-clean        keep build directories between runs (faster)
#
# Examples:
#
#   bash scripts/test-all.sh
#   bash scripts/test-all.sh --quick
#   bash scripts/test-all.sh --yocto-only --no-clean
#
# Reference: docs/testing.md.  HIL coverage (real-hardware
# verification per docs/test-plan.md) is NOT part of this script;
# see docs/ci/HW-IN-LOOP.md for the runner contract.

set -uo pipefail

# Resolve repo root regardless of where the script is invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

# -------- Flag parsing --------------------------------------------------------

QUICK=0
YOCTO_ONLY=0
ZEPHYR_ONLY=0
NO_CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --quick)        QUICK=1 ;;
        --yocto-only)   YOCTO_ONLY=1 ;;
        --zephyr-only)  ZEPHYR_ONLY=1 ;;
        --no-clean)     NO_CLEAN=1 ;;
        -h|--help)
            sed -n '3,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "test-all.sh: unknown flag '$1' (try --help)" >&2
            exit 2
            ;;
    esac
    shift
done

# -------- Stage tracking ------------------------------------------------------

declare -a STAGE_NAMES STAGE_STATUS STAGE_NOTES

run_stage() {
    local name="$1"; shift
    echo
    echo "===== [${name}] ====="
    if "$@"; then
        STAGE_NAMES+=("${name}"); STAGE_STATUS+=("PASS"); STAGE_NOTES+=("")
        echo "[${name}] PASS"
    else
        local rc=$?
        STAGE_NAMES+=("${name}"); STAGE_STATUS+=("FAIL"); STAGE_NOTES+=("exit=${rc}")
        echo "[${name}] FAIL (exit=${rc})"
    fi
}

skip_stage() {
    local name="$1"; local reason="$2"
    echo
    echo "===== [${name}] SKIP: ${reason} ====="
    STAGE_NAMES+=("${name}"); STAGE_STATUS+=("SKIP"); STAGE_NOTES+=("${reason}")
}

# -------- Stage implementations -----------------------------------------------

stage_yocto_build_and_ctest() {
    local build_dir="build/yocto-test-all"
    if [ "${NO_CLEAN}" -eq 0 ]; then
        rm -rf "${build_dir}"
    fi
    cmake -B "${build_dir}" -S . \
        -DALP_OS=yocto -DALP_BUILD_TESTS=ON \
        -G "Unix Makefiles" \
        || return 1
    cmake --build "${build_dir}" --parallel || return 1
    ctest --test-dir "${build_dir}" --output-on-failure || return 1
}

stage_baremetal_build() {
    local build_dir="build/baremetal-test-all"
    if [ "${NO_CLEAN}" -eq 0 ]; then
        rm -rf "${build_dir}"
    fi
    cmake -B "${build_dir}" -S . -DALP_OS=baremetal \
        -G "Unix Makefiles" \
        || return 1
    cmake --build "${build_dir}" --parallel || return 1
}

stage_twister() {
    if [ -z "${ZEPHYR_BASE:-}" ]; then
        return 99
    fi
    python3 "${ZEPHYR_BASE}/scripts/twister" \
        --testsuite-root "${REPO_ROOT}/tests/zephyr" \
        --testsuite-root "${REPO_ROOT}/examples" \
        -p native_sim/native/64 \
        --inline-logs \
        --no-detailed-test-id
}

stage_clang_format() {
    if ! command -v clang-format >/dev/null 2>&1; then
        return 99
    fi
    local diff_tool
    diff_tool=$(ls /usr/share/clang/clang-format*/clang-format-diff.py 2>/dev/null | head -1)
    if [ -z "${diff_tool}" ]; then
        diff_tool=$(command -v clang-format-diff 2>/dev/null || true)
    fi
    if [ -z "${diff_tool}" ]; then
        return 99
    fi
    # Default to HEAD~1; consumers in CI override via $DIFF_BASE.
    local base="${DIFF_BASE:-HEAD~1}"
    if ! git rev-parse "${base}" >/dev/null 2>&1; then
        # Shallow clone -- nothing to diff against.
        return 99
    fi
    local out
    out=$(git diff -U0 "${base}" -- '*.c' '*.h' | python3 "${diff_tool}" -p1 || true)
    if [ -n "${out}" ]; then
        echo "${out}"
        return 1
    fi
}

stage_metadata_validate() {
    if [ ! -f scripts/validate_metadata.py ]; then
        return 99
    fi
    python3 scripts/validate_metadata.py || return 1
    if [ -f metadata/templates/board.yaml.example ] && \
       [ -f scripts/alp_project.py ]; then
        python3 scripts/alp_project.py \
            --input metadata/templates/board.yaml.example \
            --emit zephyr-conf \
            --output "$(mktemp)" \
            || return 1
    fi
}

stage_doc_yaml_fragments() {
    # Lints ```yaml fenced blocks in *.md against board.schema.json.
    # Catches README + tutorial drift after schema changes.  Skips if
    # the linter or schema isn't present (older checkouts).
    if [ ! -f scripts/lint_doc_yaml_fragments.py ]; then
        return 99
    fi
    if [ ! -f metadata/schemas/board.schema.json ]; then
        return 99
    fi
    python3 scripts/lint_doc_yaml_fragments.py || return 1
}

stage_public_private() {
    if [ ! -f scripts/check_public_private.py ]; then
        return 99
    fi
    python3 scripts/check_public_private.py || return 1
}

stage_pytest_scripts() {
    # Runs the full pytest suite under tests/scripts/ -- linter,
    # silicon-determined-field rejection (a3cd4fd regression lock),
    # topology default resolution (e3a4c6b regression lock), and
    # the existing loader / orchestrator / flash / EEPROM coverage.
    if [ ! -d tests/scripts ]; then
        return 99
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        return 99
    fi
    python3 -m pytest tests/scripts/ -q || return 1
}

stage_hil_spec_validate() {
    # Cheap host-side validation of every HiL smoke spec under
    # tests/hil/.  Catches stale board targets, missing example
    # paths, malformed YAML before a nightly HiL flash run.
    if [ ! -f tests/hil/run_smoke.py ]; then
        return 99
    fi
    for board_dir in tests/hil/*/; do
        # Skip the shared _common dir (no _runner.yaml).
        if [ -f "${board_dir}/_runner.yaml" ]; then
            python3 tests/hil/run_smoke.py --validate "${board_dir}" \
                > /dev/null || return 1
        fi
    done
}

stage_doxygen() {
    if ! command -v doxygen >/dev/null 2>&1; then
        return 99
    fi
    if [ ! -f Doxyfile ]; then
        return 99
    fi
    # Capture warnings + fail if any.
    local warn_log
    warn_log=$(mktemp)
    if ! WARN_LOGFILE="${warn_log}" doxygen Doxyfile >/dev/null 2>&1; then
        echo "doxygen exited non-zero"
        return 1
    fi
    if [ -s "${warn_log}" ]; then
        cat "${warn_log}"
        return 1
    fi
}

# -------- Orchestration -------------------------------------------------------

START=$(date +%s)

if [ "${ZEPHYR_ONLY}" -eq 1 ]; then
    if rc=$(stage_twister; echo $?) 2>/dev/null; then :; fi
    case "${rc:-}" in
        99) skip_stage "twister" "ZEPHYR_BASE not set" ;;
        0)  run_stage "twister" stage_twister ;;
        *)  run_stage "twister" stage_twister ;;
    esac
else
    run_stage "yocto-build-and-ctest" stage_yocto_build_and_ctest
    run_stage "baremetal-build"       stage_baremetal_build

    if [ "${YOCTO_ONLY}" -eq 0 ]; then
        if [ "${QUICK}" -eq 1 ]; then
            skip_stage "twister" "--quick"
        elif [ -z "${ZEPHYR_BASE:-}" ]; then
            skip_stage "twister" "ZEPHYR_BASE not set (run scripts/bootstrap.sh first)"
        else
            run_stage "twister" stage_twister
        fi
    fi

    if command -v clang-format >/dev/null 2>&1; then
        run_stage "clang-format-diff" stage_clang_format
    else
        skip_stage "clang-format-diff" "clang-format not installed"
    fi

    run_stage "metadata-validate" stage_metadata_validate

    # Documentation lint -- cheap, always runnable, no special tooling.
    if [ -f scripts/lint_doc_yaml_fragments.py ]; then
        run_stage "doc-yaml-fragments" stage_doc_yaml_fragments
    else
        skip_stage "doc-yaml-fragments" "scripts/lint_doc_yaml_fragments.py missing"
    fi

    if [ -f scripts/check_public_private.py ]; then
        run_stage "public-private" stage_public_private
    else
        skip_stage "public-private" "scripts/check_public_private.py missing"
    fi

    # Pytest -- subsumes metadata-validate's unittest coverage and adds
    # the linter + regression locks for a3cd4fd / e3a4c6b.
    if command -v python3 >/dev/null 2>&1 && [ -d tests/scripts ]; then
        run_stage "pytest-scripts" stage_pytest_scripts
    else
        skip_stage "pytest-scripts" "tests/scripts missing or no python3"
    fi

    # HiL spec validation -- host-side parse + board-target check
    # for every smoke spec under tests/hil/.  No hardware required.
    if [ -f tests/hil/run_smoke.py ]; then
        run_stage "hil-spec-validate" stage_hil_spec_validate
    else
        skip_stage "hil-spec-validate" "tests/hil/run_smoke.py missing"
    fi

    if [ "${QUICK}" -eq 0 ] && [ "${YOCTO_ONLY}" -eq 0 ]; then
        if command -v doxygen >/dev/null 2>&1 && [ -f Doxyfile ]; then
            run_stage "doxygen" stage_doxygen
        else
            skip_stage "doxygen" "doxygen / Doxyfile missing"
        fi
    fi
fi

END=$(date +%s)

# -------- Summary -------------------------------------------------------------

echo
echo "===== SUMMARY ($((END - START))s) ====="
fail_count=0
for i in "${!STAGE_NAMES[@]}"; do
    printf "  %-28s %s %s\n" "${STAGE_NAMES[$i]}" "${STAGE_STATUS[$i]}" "${STAGE_NOTES[$i]}"
    [ "${STAGE_STATUS[$i]}" = "FAIL" ] && fail_count=$((fail_count + 1))
done

if [ "${fail_count}" -gt 0 ]; then
    echo
    echo "${fail_count} stage(s) failed.  See per-stage output above."
    exit 1
fi
echo
echo "All runnable stages passed.  Real-hardware coverage is parked"
echo "behind HIL per docs/test-plan.md."
exit 0
