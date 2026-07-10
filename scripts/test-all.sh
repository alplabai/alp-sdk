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
#   7. Required scripts/check_*.py gates (the same list
#      pr-metadata-validate.yml / pr-doc-drift.yml run as hard
#      gates -- see REQUIRED_GATE_SCRIPTS below)
#   8. Generated-files-in-sync (regenerate every single-sourced
#      artifact + fail on drift -- the pr-generated-files.yml gate)
#   9. Doxygen zero-warnings build (generates the pr-doxygen.yml
#      Doxyfile inline; finds doxygen on PATH or in ~/doxybin)
#
# Each stage prints `[stage] PASS` or `[stage] FAIL`; the script
# returns non-zero if any required stage failed.  A stage function
# signals "prerequisite not available" by returning exit code 99;
# run_stage() turns that into `[stage] SKIP`, never `FAIL` -- skipped
# stages don't fail the run.
#
# Worktree-safe: this script resolves its own location via
# `${BASH_SOURCE[0]}`, so REPO_ROOT is always the checkout the script
# was invoked from -- including a `git worktree add` checkout, whose
# `.git` is a *file* (a gitlink), not a directory.  The twister stage
# additionally pins that same REPO_ROOT as the LAST entry of
# EXTRA_ZEPHYR_MODULES (appended, not just defaulted when unset) so a
# worktree's own sources always win module-name resolution, even when
# an inherited EXTRA_ZEPHYR_MODULES already points elsewhere -- other
# modules already listed there are preserved, not dropped.
#
# Flags:
#   --target dev      FAST profile a dev PR is graded on: skip the slow
#                     release-only full CMake builds + Doxygen.  Use before
#                     opening a PR that targets `dev`.
#   --target main     THOROUGH release-grade profile: every stage PLUS the
#                     main-only strict ABI-snapshot diff (pr-abi-snapshot.yml,
#                     which triggers on main + release/** only).  Use before a
#                     PR that targets `main` / cutting a release.
#                     (No --target = the historical "full" run: every stage
#                     except the main-only ABI strict diff.)
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
# TARGET selects a CI profile matching the branch a PR targets:
#   dev  -- the FAST set a dev PR is graded on (skip the slow release-only
#           full CMake builds + Doxygen); for rapid integration iteration.
#   main -- the THOROUGH release-grade set: everything dev runs PLUS the
#           full yocto/baremetal builds, the Doxygen build, and the
#           main-only strict ABI-snapshot diff (pr-abi-snapshot.yml, which
#           triggers on `main` + `release/**` only).
#   full -- (default, no flag) every stage except the main-only ABI strict
#           diff -- the historical test-all.sh behavior, unchanged.
TARGET=full

while [ $# -gt 0 ]; do
    case "$1" in
        --quick)        QUICK=1 ;;
        --yocto-only)   YOCTO_ONLY=1 ;;
        --zephyr-only)  ZEPHYR_ONLY=1 ;;
        --no-clean)     NO_CLEAN=1 ;;
        --target)       shift; TARGET="${1:-}" ;;
        --target=*)     TARGET="${1#--target=}" ;;
        --dev)          TARGET=dev ;;
        --main)         TARGET=main ;;
        -h|--help)
            sed -n '3,68p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "test-all.sh: unknown flag '$1' (try --help)" >&2
            exit 2
            ;;
    esac
    shift
done

case "${TARGET}" in
    dev|main|full) ;;
    *)
        echo "test-all.sh: --target must be 'dev' or 'main' (got '${TARGET}')" >&2
        exit 2
        ;;
esac

# -------- Stage tracking ------------------------------------------------------

declare -a STAGE_NAMES STAGE_STATUS STAGE_NOTES

run_stage() {
    local name="$1"; shift
    echo
    echo "===== [${name}] ====="
    "$@"
    local rc=$?
    # Convention: a stage function returns 99 to mean "a prerequisite
    # (tool / optional script / env var) isn't available here" -- that
    # is a SKIP, never a FAIL, regardless of which stage it came from.
    if [ "${rc}" -eq 99 ]; then
        STAGE_NAMES+=("${name}"); STAGE_STATUS+=("SKIP"); STAGE_NOTES+=("prerequisite unavailable")
        echo "[${name}] SKIP (prerequisite unavailable)"
    elif [ "${rc}" -eq 0 ]; then
        STAGE_NAMES+=("${name}"); STAGE_STATUS+=("PASS"); STAGE_NOTES+=("")
        echo "[${name}] PASS"
    else
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
    # Pin THIS checkout as the alp-sdk Zephyr module -- always, even
    # if EXTRA_ZEPHYR_MODULES is already set (e.g. exported from a
    # shell rc pointing at a primary checkout, per docs/local-ci.md).
    # Without this, running test-all.sh from a `git worktree add`
    # checkout compiled tests from the worktree against alp-sdk
    # sources from wherever EXTRA_ZEPHYR_MODULES already pointed -- a
    # silent mixed-revision link (#608).
    #
    # zephyr_module.py's parse_modules() keys modules by module NAME
    # (see zephyr/scripts/zephyr_module.py) and a later entry with the
    # same name overwrites an earlier one, so appending REPO_ROOT as
    # the LAST entry makes it win a name collision against a
    # differently-pathed alp-sdk module earlier in the list --
    # without dropping any other (non-alp-sdk) module already listed.
    local -a _existing=() _modules=()
    local _m _joined
    IFS=';' read -ra _existing <<< "${EXTRA_ZEPHYR_MODULES:-}"
    for _m in "${_existing[@]}"; do
        [ -n "${_m}" ] && [ "${_m}" != "${REPO_ROOT}" ] && _modules+=("${_m}")
    done
    _modules+=("${REPO_ROOT}")
    _joined=$(IFS=';'; echo "${_modules[*]}")
    export EXTRA_ZEPHYR_MODULES="${_joined}"
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
    # The helper ships under two names depending on how it was
    # installed: the apt/`/usr/share/clang/...` layout names it
    # `clang-format-diff.py`; the pip `clang-format` wheel (see
    # docs/testing.md) puts a same-named `clang-format-diff.py` on
    # PATH, while some distros symlink an extensionless
    # `clang-format-diff`.  Check PATH for both spellings before
    # falling back to the apt path glob.
    local diff_tool
    diff_tool=$(command -v clang-format-diff.py 2>/dev/null || true)
    if [ -z "${diff_tool}" ]; then
        diff_tool=$(command -v clang-format-diff 2>/dev/null || true)
    fi
    if [ -z "${diff_tool}" ]; then
        diff_tool=$(ls /usr/share/clang/clang-format*/clang-format-diff.py 2>/dev/null | head -1)
    fi
    if [ -z "${diff_tool}" ]; then
        echo "clang-format is installed but no clang-format-diff(.py) helper was found on PATH or under /usr/share/clang -- skipping"
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

# Registry of the scripts/check_*.py (+ a couple of validate_*.py)
# gates that pr-metadata-validate.yml / pr-doc-drift.yml run as hard,
# non-informational CI gates -- i.e. every gate below is a required
# check today, not the "informational" ones (check_test_coverage.py,
# check_cross_platform.py) CI itself doesn't fail on.  Kept as one
# list so this wrapper and CI can't silently drift apart: wiring a
# script into a workflow as a hard gate and NOT adding it here is the
# defect #608 flagged (check_stub_issues.py failed on review while
# this wrapper never ran it).
REQUIRED_GATE_SCRIPTS=(
    check_pin_conflicts.py
    check_e1m_pinout.py
    check_inference_backend_parity.py
    check_e1m_route_capability.py
    check_emit_snapshots.py
    check_stub_issues.py
    check_vendor_ext_tags.py
    check_public_header_purity.py
    check_local_paths.py
    check_sw_fallback_tags.py
    check_som_bundle.py
    check_chip_manifest_parity.py
    check_chip_header_status.py
    check_example_portability.py
    check_doc_drift.py
    check_version_doc_sync.py
)

stage_required_gate_scripts() {
    if ! command -v python3 >/dev/null 2>&1; then
        return 99
    fi
    local script path failed=0 ran=0
    for script in "${REQUIRED_GATE_SCRIPTS[@]}"; do
        path="scripts/${script}"
        if [ ! -f "${path}" ]; then
            continue
        fi
        ran=1
        echo "--- ${path} ---"
        python3 "${path}" || failed=1
    done

    # board.yaml schema sweep -- canonical template + every
    # examples/*/board.yaml + tests/*/board.yaml, mirroring the
    # pr-metadata-validate.yml "schema sweep" step.
    if [ -f scripts/validate_board_yaml.py ]; then
        ran=1
        if [ -f metadata/templates/board.yaml.example ]; then
            echo "--- validate_board_yaml.py (canonical template) ---"
            python3 scripts/validate_board_yaml.py \
                --input metadata/templates/board.yaml.example || failed=1
        fi
        while IFS= read -r f; do
            echo "--- validate_board_yaml.py ${f} ---"
            python3 scripts/validate_board_yaml.py --input "${f}" || failed=1
        done < <(find examples tests -name board.yaml 2>/dev/null)
    fi

    # gd32-bridge protocol vectors must not drift from the generator
    # (mirrors the pr-metadata-validate.yml gd32-bridge step).
    if [ -f firmware/gd32-bridge/tests/gen_protocol_vectors.py ]; then
        ran=1
        echo "--- gd32-bridge protocol vectors --check ---"
        python3 firmware/gd32-bridge/tests/gen_protocol_vectors.py --check \
            || failed=1
    fi

    if [ "${ran}" -eq 0 ]; then
        return 99
    fi
    return "${failed}"
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
    # Resolve a doxygen binary: PATH first, then the no-root tarball
    # install at ~/doxybin (see running-local-ci).
    local dox
    dox=$(command -v doxygen 2>/dev/null || true)
    if [ -z "${dox}" ] && [ -x "${HOME}/doxybin/doxygen" ]; then
        dox="${HOME}/doxybin/doxygen"
    fi
    if [ -z "${dox}" ]; then
        return 99
    fi
    # The repo ships NO committed Doxyfile -- pr-doxygen.yml generates one
    # inline.  Reproduce that Doxyfile FAITHFULLY here so the full
    # WARN_AS_ERROR build (which alone catches bad @ref / dead md links --
    # the coverage script does NOT) runs locally before the PR.  Keep
    # INPUT / EXCLUDE_PATTERNS / WARN_AS_ERROR identical to
    # .github/workflows/pr-doxygen.yml.
    local cfg warn_log
    cfg=$(mktemp)
    warn_log=$(mktemp)
    cat > "${cfg}" <<EOF
PROJECT_NAME           = "Alp SDK"
OUTPUT_DIRECTORY       = $(mktemp -d)
INPUT                  = include/alp
RECURSIVE              = YES
EXTRACT_ALL            = YES
EXTRACT_STATIC         = NO
GENERATE_HTML          = NO
GENERATE_LATEX         = NO
QUIET                  = YES
WARN_AS_ERROR          = FAIL_ON_WARNINGS
WARN_LOGFILE           = ${warn_log}
OPTIMIZE_OUTPUT_FOR_C  = YES
JAVADOC_AUTOBRIEF      = YES
USE_MDFILE_AS_MAINPAGE = README.md
INPUT                 += README.md VERSIONS.md CONTRIBUTING.md TRADEMARKS.md docs \
                         chips/README.md vendors/alif/README.md \
                         vendors/deepx-dxm1/README.md \
                         vendors/gd32_firmware_library/README.md \
                         firmware/cc3501e/README.md keys/README.md \
                         meta-alp-sdk/README.md \
                         metadata/library-profiles/README.md \
                         zephyr/sysbuild/aen/README.md
EXCLUDE_PATTERNS       = */superpowers/*
EOF
    "${dox}" "${cfg}" >/dev/null 2>&1 || true
    if [ -s "${warn_log}" ]; then
        cat "${warn_log}"
        return 1
    fi
}

# Main-only strict ABI gate (pr-abi-snapshot.yml triggers on main +
# release/** only): fail if the working headers drift from the committed
# CURRENT snapshot (v0.9; older are frozen).  This is the release-grade
# check the `--target main` profile adds on top of the dev set.
stage_abi_strict() {
    command -v python3 >/dev/null 2>&1 || return 99
    [ -f scripts/abi_snapshot.py ] || return 99
    local snap="docs/abi/v0.9-snapshot.json"
    [ -f "${snap}" ] || return 99
    if ! python3 scripts/abi_snapshot.py --diff "${snap}"; then
        echo "ABI drift vs ${snap} -- regen + commit (bump snapshot after a release)"
        return 1
    fi
}

# Reproduce pr-generated-files.yml: regenerate every single-sourced
# artifact, then fail if any committed copy drifted.  This is the
# `check · generated files in sync` gate -- the one that reddens a PR
# when a new macro/symbol/gate/example didn't get its generated file
# regenerated + committed.  A nonzero exit means "run the regenerators
# and commit the result" (the tree is left regenerated for you to add).
stage_generated_files() {
    command -v python3 >/dev/null 2>&1 || return 99
    local gens=(gen_soc_caps gen_status_strings gen_board_header
                gen_pinmux_capability gen_support_matrix
                gen_portability_matrix gen_catalog gen_error_catalog)
    local g
    for g in "${gens[@]}"; do
        [ -f "scripts/${g}.py" ] || continue
        python3 "scripts/${g}.py" >/dev/null 2>&1 || { echo "scripts/${g}.py failed"; return 1; }
    done
    # ABI snapshot -- current working snapshot is v0.9 (older are frozen).
    if [ -f scripts/abi_snapshot.py ]; then
        python3 scripts/abi_snapshot.py --version v0.9 \
            --output docs/abi/v0.9-snapshot.json >/dev/null 2>&1 || true
    fi
    # Ignore only the snapshot's "generated" date line, like the CI gate.
    if ! git diff --quiet --ignore-matching-lines='"generated":' -- \
            include/alp docs/abi src/cap.c src/status_strings.c \
            metadata/catalog.json metadata/pinmux docs/portability-matrix.md \
            docs/diagnostics 2>/dev/null; then
        echo "generated files are OUT OF SYNC -- regenerated in place; git add + commit:"
        git --no-pager diff --stat --ignore-matching-lines='"generated":' -- \
            include/alp docs/abi src/cap.c src/status_strings.c \
            metadata/catalog.json metadata/pinmux docs/portability-matrix.md \
            docs/diagnostics 2>/dev/null | tail -20
        return 1
    fi
}

# -------- Orchestration -------------------------------------------------------

START=$(date +%s)

if [ "${ZEPHYR_ONLY}" -eq 1 ]; then
    # Run the suite exactly once.  run_stage() already turns a 99
    # return (ZEPHYR_BASE unset) into SKIP, so no pre-check/case
    # dance -- and no second, output-hiding invocation -- is needed.
    run_stage "twister" stage_twister
else
    # The full plain-CMake builds are release-grade -- the fast `dev`
    # profile skips them (dev PRs iterate on twister + the cheap gates).
    if [ "${TARGET}" = "dev" ]; then
        skip_stage "yocto-build-and-ctest" "--target dev (release-grade build)"
        skip_stage "baremetal-build"       "--target dev (release-grade build)"
    else
        run_stage "yocto-build-and-ctest" stage_yocto_build_and_ctest
        run_stage "baremetal-build"       stage_baremetal_build
    fi

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

    # Required scripts/check_*.py gates -- see REQUIRED_GATE_SCRIPTS
    # above.  Keeps this wrapper's coverage aligned with the hard
    # gates pr-metadata-validate.yml / pr-doc-drift.yml run in CI.
    run_stage "required-gate-scripts" stage_required_gate_scripts

    # `check · generated files in sync` -- regenerate every single-sourced
    # artifact + fail on drift.  Catches the class of red that bit #623 /
    # #636 / #642 (new macro/symbol/gate without a committed regen).
    run_stage "generated-files" stage_generated_files

    # Main-only: the strict ABI-snapshot diff gate that pr-abi-snapshot.yml
    # runs on `main` + `release/**` only.  The `--target main` release-grade
    # profile adds it; dev/full skip it (generated-files already regenerates
    # the snapshot, but the strict diff-vs-committed is a main-branch gate).
    if [ "${TARGET}" = "main" ]; then
        run_stage "abi-strict" stage_abi_strict
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

    if [ "${QUICK}" -eq 0 ] && [ "${YOCTO_ONLY}" -eq 0 ] && [ "${TARGET}" != "dev" ]; then
        # stage_doxygen generates the CI Doxyfile itself + finds doxygen on
        # PATH or in ~/doxybin, so no committed Doxyfile is needed.  The fast
        # dev profile skips it (Doxygen is one of the slow stages).
        if command -v doxygen >/dev/null 2>&1 || [ -x "${HOME}/doxybin/doxygen" ]; then
            run_stage "doxygen" stage_doxygen
        else
            skip_stage "doxygen" "doxygen not installed (PATH or ~/doxybin)"
        fi
    elif [ "${TARGET}" = "dev" ]; then
        skip_stage "doxygen" "--target dev (slow release-grade stage)"
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
