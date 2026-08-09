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
#   5. shellcheck over scripts/*.sh (skipped if shellcheck isn't installed)
#   6. bash -n parse of every shipped *.sh under REAL bash 3.2.57 in a
#      container (skipped, loudly, if podman/docker isn't on PATH --
#      cross-platform-zephyr.yml's macos-latest leg still covers it)
#   7. board.yaml metadata schema validate
#   8. Public/private text classifier
#   9. Required scripts/check_*.py gates (the same list
#      pr-metadata-validate.yml / pr-doc-drift.yml run as hard
#      gates -- see REQUIRED_GATE_SCRIPTS below)
#  10. Generated-files-in-sync (regenerate every single-sourced
#      artifact + fail on drift -- the pr-generated-files.yml gate)
#  11. Doxygen zero-warnings build (generates the pr-doxygen.yml
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
#   --list-required-gate-scripts
#                     print the scripts/check_*.py paths the
#                     required-gate-scripts stage would run (one per line,
#                     no execution) and exit -- a cheap probe for
#                     alp-sdk#1109's regression: this list must always
#                     match `quality_tasks.py --gate-scripts` 1:1.
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
cd "${REPO_ROOT}" || exit 1

# -------- Flag parsing --------------------------------------------------------

QUICK=0
YOCTO_ONLY=0
ZEPHYR_ONLY=0
NO_CLEAN=0
LIST_REQUIRED_GATE_SCRIPTS=0
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
        --list-required-gate-scripts) LIST_REQUIRED_GATE_SCRIPTS=1 ;;
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
    # `${arr[@]+"${arr[@]}"}` is the set-u-safe empty-array expansion: on
    # bash 3.2 (the macOS default) `"${_existing[@]}"` on an EMPTY array
    # trips `set -u` with "unbound variable" -- the `+` guard expands to
    # nothing when the array is empty/unset instead.  (Bit macOS/Windows
    # python-smoke via test_test_all_worktree.py.)
    for _m in ${_existing[@]+"${_existing[@]}"}; do
        [ -n "${_m}" ] && [ "${_m}" != "${REPO_ROOT}" ] && _modules+=("${_m}")
    done
    _modules+=("${REPO_ROOT}")
    _joined=$(IFS=';'; echo "${_modules[*]}")
    export EXTRA_ZEPHYR_MODULES="${_joined}"
    # Match pr-twister.yml FAITHFULLY: same testsuite roots (incl.
    # tests/unit) AND warnings-as-errors.  Twister builds strict in CI, so
    # a warning like -Werror=comment ('/*' inside a comment) fails there;
    # forcing CONFIG_COMPILER_WARNINGS_AS_ERRORS=y here catches that class
    # locally instead of on the PR (bit examples/.../u8g2 main.c, #650).
    python3 "${ZEPHYR_BASE}/scripts/twister" \
        --testsuite-root "${REPO_ROOT}/tests/unit" \
        --testsuite-root "${REPO_ROOT}/tests/zephyr" \
        --testsuite-root "${REPO_ROOT}/examples" \
        -p native_sim/native/64 \
        --extra-args=CONFIG_COMPILER_WARNINGS_AS_ERRORS=y \
        --inline-logs \
        --no-detailed-test-id
}

stage_shellcheck() {
    # Static-lint the project shell scripts so a shell bug (an SC2164
    # cd-without-guard, a `set -u` empty-array trap, a POSIX-portability
    # slip that only bites macOS's bash 3.2) is caught on Linux BEFORE it
    # reddens macOS/Windows python-smoke.  Resolve shellcheck on PATH or
    # the no-root ~/.local/bin install.
    local sc
    sc=$(command -v shellcheck 2>/dev/null || true)
    if [ -z "${sc}" ] && [ -x "${HOME}/.local/bin/shellcheck" ]; then
        sc="${HOME}/.local/bin/shellcheck"
    fi
    if [ -z "${sc}" ]; then
        return 99
    fi
    # test-all.sh is the load-bearing local-CI wrapper (it runs in a macOS
    # CI test, test_test_all_worktree.py), so lint it at warning level;
    # lint the rest of scripts/*.sh at error level to avoid drowning in
    # pre-existing style warnings.
    local rc=0
    "${sc}" -S warning scripts/test-all.sh || rc=1
    local other
    for other in scripts/*.sh; do
        [ "${other}" = "scripts/test-all.sh" ] && continue
        "${sc}" -S error "${other}" || rc=1
    done
    return "${rc}"
}

stage_bash32_parse() {
    # Parse every shell script the repo ships under REAL bash 3.2.57
    # (macOS's frozen system bash) inside a container -- catches the
    # class of defect shellcheck and `bash -n` on a modern bash both
    # miss: bash 3.2 keeps tracking single-quote state ACROSS a
    # heredoc body while scanning for the closing `)` of an enclosing
    # `$( )`, even when the heredoc tag is quoted (<<'PY'). An ODD
    # apostrophe count inside such a heredoc desyncs the parser and it
    # fails much later at an unrelated token -- see PR #1050, where a
    # single apostrophe added to a comment inside
    # scripts/bootstrap.sh:271's `<<'PY'` heredoc broke parsing 131
    # lines later. This is a REPRO, not a "second shellcheck": that
    # gate already runs (stage_shellcheck) and did not catch #1050.
    #
    # cross-platform-zephyr.yml's python-smoke job runs this same
    # `bash -n` sweep unconditionally on its macos-latest leg (real
    # bash 3.2.57, no container), so a missing runtime here is a SKIP,
    # not a gap: that CI leg still covers it.
    #
    # Never build bash 3.2.57 from source to avoid the container --
    # its shipped y.tab.c is stale against parse.y and the build fails
    # without `bison`.
    local runtime
    runtime=$(command -v podman 2>/dev/null || command -v docker 2>/dev/null || true)
    if [ -z "${runtime}" ]; then
        echo "stage_bash32_parse: no podman/docker on PATH -- SKIPPING the local bash-3.2 parse check."
        echo "stage_bash32_parse: this is still covered unconditionally by cross-platform-zephyr.yml's"
        echo "stage_bash32_parse: python-smoke job on its macos-latest leg (real bash 3.2.57, no container)."
        return 99
    fi
    # One container run, not one per file -- the container itself
    # (bash 3.2) loops the file list, since 24 separate `podman run`
    # spins would be needlessly slow.
    local -a files=()
    local f
    while IFS= read -r f; do
        files+=("${f}")
    done < <(git ls-files '*.sh')
    # An empty file list must never read as "0 broken files == PASS" --
    # that is a silent-empty-loop, the exact shape of gate this PR argues
    # against, just one level up (wrong cwd, git ls-files broke, REPO_ROOT
    # pointed somewhere without a .git). Fail loudly instead of parsing
    # nothing and calling it clean.
    if [ "${#files[@]}" -eq 0 ]; then
        echo "stage_bash32_parse: 'git ls-files *.sh' returned NO files from ${REPO_ROOT} -- refusing to report a silent pass."
        return 1
    fi
    # `"${files[@]}"` on a NON-empty array is fine, but this codepath must
    # stay safe even if `files` were ever empty -- bash 3.2 (inside the
    # container we're about to invoke, and on macOS outside it) trips
    # `set -u` "unbound variable" expanding `"${arr[@]}"` on an EMPTY array;
    # the `${arr[@]+"${arr[@]}"}` guard (already used at :211 above) expands
    # to nothing instead. This is the exact #654/#658 bash-3.2 crash class.
    "${runtime}" run --rm -v "${REPO_ROOT}:/w:ro" -w /w docker.io/library/bash:3.2 \
        bash -c '
            set -u
            # Assert the pinned image really is bash 3.2, same as the CI
            # step (cross-platform-zephyr.yml) asserts /bin/bash really is
            # 3.2 on macOS. docker.io/library/bash:3.2 is a mutable
            # Docker-official tag; if it ever moves off 3.2, this stage
            # would otherwise silently stop proving anything.
            ver=$(bash --version | head -1)
            case "$ver" in
                *"version 3.2"*) ;;
                *)
                    echo "stage_bash32_parse: container bash reports [$ver], not 3.2 -- the docker.io/library/bash:3.2 tag has drifted off real bash 3.2, so this stage no longer proves what it claims. Fix the pin before trusting it again."
                    exit 3
                    ;;
            esac
            rc=0
            for f in "$@"; do
                if ! bash -n "$f"; then
                    echo "stage_bash32_parse: bash -n FAILED on $f (see error above)"
                    rc=1
                fi
            done
            exit "$rc"
        ' _ ${files[@]+"${files[@]}"}
    local rc=$?
    # Distinguish a REAL result from this stage's own script (0 = clean,
    # 1 = a genuine bash -n parse failure, 3 = the version-pin assertion
    # above caught a drifted image) from a RUNTIME failure of the
    # container invocation itself (offline image pull, an SELinux denial
    # on the `-v ...:/w:ro` mount with no `,Z`, a rootless-docker
    # permission error) -- those surface as some OTHER exit code from
    # `podman run`/`docker run` and must never read as "a parse defect
    # shipped"; they mean the local check couldn't run, and CI's
    # macos-latest leg is still the unconditional backstop.
    case "${rc}" in
        0) return 0 ;;
        1) return 1 ;;
        3) return 1 ;;
        *)
            echo "stage_bash32_parse: '${runtime} run' itself failed (rc=${rc}) -- a container/runtime problem, NOT a parse defect. SKIPPING; cross-platform-zephyr.yml's python-smoke (macos-latest) leg still covers this unconditionally."
            return 99
            ;;
    esac
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
    # Exclude vendors/** + zephyr/** exactly like pr-static-analysis.yml's
    # clang-format gate: those subtrees keep their UPSTREAM project's style
    # (GigaDevice/Zephyr/Renesas-FSP), our .clang-format does not govern
    # them, and CI never flags them -- so neither should the local mirror.
    # (Without this, a vendored .c drift produced a FALSE local failure.)
    # tests/scripts/fixtures/rzv2n_svd/** is excluded on the same grounds:
    # verbatim BSD-3-Clause Renesas FSP header excerpts that
    # test_gen_rzv2n_cm33_svd.py asserts stay byte-identical to the real
    # module headers, so reformatting them would break the fixture.
    #
    # NOTE: this stage diffs against a COMMIT, so it cannot see untracked
    # files.  A brand-new .c/.h shows up here only once it is staged or
    # committed -- run it again after `git add`, or the gate passes on a
    # file it never read.
    local out
    out=$(git diff -U0 "${base}" -- '*.c' '*.h' ':!vendors/**' ':!zephyr/**' \
          ':!tests/scripts/fixtures/rzv2n_svd/**' \
          | python3 "${diff_tool}" -p1 || true)
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

stage_alp_lock() {
    # Mirrors pr-metadata-validate.yml's `alp.lock --check` step (#1045) --
    # previously the ONLY place that check ran, so a locally-green branch
    # could still redden CI on lock drift it never saw. No "script missing"
    # skip: this is a tracked repo file, so a missing/renamed script means
    # the gate itself vanished and that must redden, not SKIP silently
    # (same reasoning as stage_generated_files above).
    python3 scripts/west_commands/alp_lock.py --workspace . --check || return 1
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

stage_cross_platform_lint() {
    # Mirrors cross-platform-zephyr.yml's python-smoke step (alp-sdk#1032
    # A5) -- the ONLY place --fail-on-warning ran before this was three
    # legs of a non-required workflow, so a repo drifting back to N
    # findings had no local signal and no required check to catch it.
    python3 scripts/check_cross_platform.py --fail-on-warning || return 1
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

    # tests/parity/ is NOT under tests/scripts/, so the seam-1 comparator's own
    # 15 unit tests were excluded from this stage AND from parity-seam1.yml,
    # which invoked only the comparator. Run them here too so a local
    # `test-all.sh` green means the same thing CI's does.
    #
    # NOT the same as running the comparator: this stage still does NOT invoke
    # `tests/parity/seam1_field_diff.py` against the frozen oracle -- only
    # parity-seam1.yml does. A green test-all.sh therefore does NOT clear the
    # `parity-seam1` gate; run the comparator separately before pushing:
    #   python3 tests/parity/seam1_field_diff.py --sdk . --oracle tests/parity/oracle
    if [ -f tests/parity/test_seam1_field_diff.py ]; then
        python3 -m pytest tests/parity/test_seam1_field_diff.py -q || return 1
    fi
}

# The hard-gate scripts/check_*.py list is the registry's gate set, read at
# runtime from metadata/quality-tasks-v1.json (single source of truth, drift-
# gated by check_quality_registry.py) so this wrapper and CI can't diverge --
# the defect #608 flagged. Each entry here is a bare script name (the loop
# below prefixes scripts/); quality_tasks.py prints full paths, so strip the
# scripts/ prefix as we read.
REQUIRED_GATE_SCRIPTS=()
if command -v python3 >/dev/null 2>&1; then
    if ! _qgate_out="$(python3 "${REPO_ROOT}/scripts/quality_tasks.py" --gate-scripts 2>&1)"; then
        echo "FATAL: quality_tasks.py --gate-scripts failed (registry broken?):" >&2
        echo "${_qgate_out}" >&2
        exit 1
    fi
    while IFS= read -r _qpath; do
        # Defensive: quality_tasks.py now writes '\n' explicitly (alp-sdk#1109),
        # but strip a trailing '\r' here too in case its output ever reaches
        # this loop CRLF-terminated again (a stale/vendored copy, a Windows
        # pipe upstream) -- `IFS= read -r` does not strip it on its own, and
        # an unstripped '\r' makes every path below fail its `-f` existence
        # check and skip silently.
        _qpath="${_qpath%$'\r'}"
        [ -n "${_qpath}" ] && REQUIRED_GATE_SCRIPTS+=("${_qpath#scripts/}")
    done <<< "${_qgate_out}"
    if [ "${#REQUIRED_GATE_SCRIPTS[@]}" -eq 0 ]; then
        echo "FATAL: quality-tasks-v1.json yielded zero gate scripts" >&2
        exit 1
    fi
fi

# --list-required-gate-scripts: print exactly the paths
# stage_required_gate_scripts()'s loop below would print a
# "--- scripts/... ---" header for (same existence filter, zero
# execution) and exit -- the cheap probe the alp-sdk#1109 regression
# guard (tests/scripts/test_test_all_gate_coverage.py) diffs against
# `quality_tasks.py --gate-scripts` to prove this stage still finds
# every declared gate script.
if [ "${LIST_REQUIRED_GATE_SCRIPTS}" -eq 1 ]; then
    for _qscript in "${REQUIRED_GATE_SCRIPTS[@]}"; do
        _qspath="scripts/${_qscript}"
        [ -f "${_qspath}" ] && echo "${_qspath}"
    done
    exit 0
fi

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
    # pr-metadata-validate.yml "schema sweep" step (including its
    # rpmsg-imx93 exclusion -- see board-yaml-sweep-exclude.sh).
    if [ -f scripts/validate_board_yaml.py ]; then
        ran=1
        if [ -f metadata/templates/board.yaml.example ]; then
            echo "--- validate_board_yaml.py (canonical template) ---"
            python3 scripts/validate_board_yaml.py \
                --input metadata/templates/board.yaml.example || failed=1
        fi
        # shellcheck source=scripts/board-yaml-sweep-exclude.sh
        source "${REPO_ROOT}/scripts/board-yaml-sweep-exclude.sh"
        while IFS= read -r f; do
            echo "--- validate_board_yaml.py ${f} ---"
            python3 scripts/validate_board_yaml.py --input "${f}" || failed=1
        done < <(find examples tests -name board.yaml 2>/dev/null \
                  | grep -v "${BOARD_YAML_SWEEP_EXCLUDE_PATTERN}")
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
    # docs/doxygen/Doxyfile is the single source shared with
    # pr-doxygen.yml (#970) -- run the SAME full WARN_AS_ERROR build
    # here (it alone catches bad @ref / dead md links -- the coverage
    # script does NOT) instead of hand-maintaining a second Doxyfile
    # that drifts.  Per-run values that must not collide across
    # concurrent local runs (OUTPUT_DIRECTORY, WARN_LOGFILE) plus the
    # commit-varying PROJECT_NUMBER are appended on stdin, mirroring
    # the CI workflow's own stdin-override technique.
    if [ ! -f docs/doxygen/Doxyfile ]; then
        return 99
    fi
    local out_dir warn_log project_number
    out_dir=$(mktemp -d)
    warn_log=$(mktemp)
    project_number=$(git describe --tags --always 2>/dev/null || echo 0.1.0-pre)
    {
        cat docs/doxygen/Doxyfile
        printf 'OUTPUT_DIRECTORY = %s\n' "${out_dir}"
        printf 'WARN_LOGFILE = %s\n' "${warn_log}"
        printf 'PROJECT_NUMBER = "%s"\n' "${project_number}"
    } | "${dox}" - >/dev/null 2>&1 || true
    if [ -s "${warn_log}" ]; then
        cat "${warn_log}"
        return 1
    fi
}

# Single source: metadata/sdk_version.yaml declares MAJOR.MINOR.PATCH;
# the ABI snapshot files are named vMAJOR.MINOR (docs/abi/README.md).
# Deriving the path HERE -- instead of a hardcoded literal that some
# past release cut forgot to bump -- is what makes it impossible for
# this gate to keep regenerating a FROZEN historical snapshot against
# today's headers (issue #803): the path always tracks whatever
# metadata/sdk_version.yaml declares as current, this run and every
# run after the next release.  Shells out to abi_snapshot.py's own
# `--print-current-version` rather than re-parsing sdk_version.yaml
# here, so this and `pr-generated-files.yml` derive the label from ONE
# parse, not two that could drift (issue #826).  Prints nothing and
# returns nonzero if sdk_version.yaml is missing/unparsable.
# stage_abi_strict treats that as "prerequisite not available" (stage
# SKIP, same as a missing tool) -- but stage_generated_files must NOT:
# that stage's whole job is "regenerate + assert nothing drifted", and
# a snapshot it silently skips regenerating is a snapshot it can't
# check drift against either, which is exactly the false-PASS #795 was
# about.
abi_current_snapshot() {
    command -v python3 >/dev/null 2>&1 || return 1
    local version
    version=$(python3 scripts/abi_snapshot.py --print-current-version 2>/dev/null) || return 1
    printf 'docs/abi/%s-snapshot.json\n' "${version}"
}

# Main-only strict ABI gate (pr-abi-snapshot.yml triggers on main +
# release/** only): fail if the working headers drift from the committed
# CURRENT snapshot (derived from metadata/sdk_version.yaml; older
# snapshots are frozen historical records, see docs/abi/README.md).
# This is the release-grade check the `--target main` profile adds on
# top of the dev set.
stage_abi_strict() {
    command -v python3 >/dev/null 2>&1 || return 99
    [ -f scripts/abi_snapshot.py ] || return 99
    local snap
    snap=$(abi_current_snapshot) || return 99
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
                gen_portability_matrix gen_catalog gen_error_catalog
                gen_verification_status)
    local g rc
    local gen_total=0 gen_skipped=0
    for g in "${gens[@]}"; do
        [ -f "scripts/${g}.py" ] || continue
        gen_total=$((gen_total + 1))
        python3 "scripts/${g}.py" >/dev/null 2>&1
        rc=$?
        if [ "${rc}" -eq 99 ]; then
            # 99 = the generator refused for lack of a tool (clang-format;
            # see gen_soc_caps.py / gen_status_strings.py), not a
            # generator defect -- count it, don't fail the stage over it
            # (alp-sdk#1221). The files it would have written are simply
            # left as committed, so the diff check below stays honest.
            gen_skipped=$((gen_skipped + 1))
        elif [ "${rc}" -ne 0 ]; then
            echo "scripts/${g}.py failed"
            return 1
        fi
    done
    # gen_soc_peripheral_instances.py is NOT in the array above: unlike
    # every other generator here, it needs a resolvable Zephyr checkout
    # (the vendored SoC devicetree) and skips cleanly (exit 0, a
    # `skipped: ...` line) without one -- the other seven have no such
    # prerequisite and would silently swallow that line under the loop's
    # `>/dev/null 2>&1`.  Run it separately, unsuppressed, so the skip is
    # visible instead of a contributor seeing 16/16 PASS with no signal
    # that this specific fact went unchecked (#1154 PR review). When
    # $ZEPHYR_BASE (or the west-topdir zephyr/ fallback) DOES resolve --
    # the common case on a bootstrapped dev machine -- this actually
    # regenerates metadata/socs/renesas/rzv2n/n44.json in place, and the
    # diff check below catches real drift, same as every other generator.
    if [ -f scripts/gen_soc_peripheral_instances.py ]; then
        python3 scripts/gen_soc_peripheral_instances.py \
            || { echo "scripts/gen_soc_peripheral_instances.py failed"; return 1; }
    fi
    # ABI snapshot -- current working snapshot is derived from
    # metadata/sdk_version.yaml (older snapshots are frozen).
    if [ -f scripts/abi_snapshot.py ]; then
        local abi_snap abi_ver
        if ! abi_snap=$(abi_current_snapshot); then
            # Unlike stage_abi_strict, this is NOT a skip: this stage's
            # job is to regenerate the snapshot and prove it matches
            # what's committed, and it can't do either without knowing
            # which snapshot is current.  Passing anyway would be the
            # same silent "regen didn't happen, gate still went green"
            # defect as the `|| true` this stage no longer has (#795).
            echo "abi_current_snapshot failed -- cannot determine the current ABI snapshot (check metadata/sdk_version.yaml)"
            return 1
        fi
        abi_ver=$(basename "${abi_snap}" -snapshot.json)
        # No `|| true` here: this regen is the only thing that makes
        # the diff below able to see ABI drift, so swallowing its exit
        # code turns "the snapshot could not be regenerated" into a
        # PASS -- the local gate goes green and the PR goes red on the
        # exact drift this stage exists to catch (issue #795).  The
        # write guard in abi_snapshot.py exits 2 when the label is not
        # current, which is precisely a failure worth surfacing.
        if ! python3 scripts/abi_snapshot.py --version "${abi_ver}" \
                --output "${abi_snap}" >/dev/null; then
            echo "scripts/abi_snapshot.py failed to regenerate ${abi_snap}"
            return 1
        fi
    fi
    # `git diff` alone is blind to a brand-new file the regen step just
    # created (a not-yet-tracked board header, an added pinmux table,
    # ...) -- a plain diff --exit-code reports 0 and this stage stays
    # green even though a newly-needed generated file is missing from
    # the commit (#1128a, mirrors pr-generated-files.yml's own `git add
    # -N` step).  Mark every generated path intent-to-add so a new file
    # shows up as an addition in the diff below, without staging real
    # content. A `git add -N` pathspec that matches nothing (an expected
    # generated path flat-out missing from the tree) fails closed with
    # nothing staged -- swallowing that exit code would turn "can't even
    # see what I'm supposed to check" into the same unearned PASS #1128a
    # was about, so it's a hard failure here, not a silent no-op.
    if ! git add -N -- \
        include/alp docs/abi src/cap.c src/status_strings.c \
        metadata/catalog.json metadata/error-catalog.json metadata/pinmux \
        metadata/socs/renesas/rzv2n/n44.json \
        docs/portability-matrix.md docs/peripheral-support-matrix.md \
        docs/verification-status.md \
        docs/diagnostics 2>/dev/null; then
        echo "git add -N failed -- an expected generated path is missing from the tree"
        return 1
    fi
    # No line-mask here: abi_snapshot.py's own write-skip guard is what
    # keeps a no-op regen from touching the snapshot's "generated" line
    # at all (issue #1232), so this diff sees zero lines changed for
    # that case with no help needed -- matching pr-generated-files.yml's
    # own `git diff` step, which drops the mask for the same reason. A
    # real content change fails on its content lines regardless, so
    # masking "generated" would only ever hide a regression in that
    # guard, never a false positive. metadata/socs/renesas/rzv2n/n44.json
    # only actually moves above when gen_soc_peripheral_instances.py
    # found a resolvable Zephyr checkout (see the comment above its
    # invocation) -- when it didn't, this path is untouched and simply
    # contributes no diff, same as any other unaffected path in this
    # list.
    if ! git diff --quiet -- \
            include/alp docs/abi src/cap.c src/status_strings.c \
            metadata/catalog.json metadata/error-catalog.json metadata/pinmux \
            metadata/socs/renesas/rzv2n/n44.json \
            docs/portability-matrix.md docs/peripheral-support-matrix.md \
            docs/verification-status.md \
            docs/diagnostics 2>/dev/null; then
        echo "generated files are OUT OF SYNC -- regenerated in place; git add + commit:"
        git --no-pager diff --stat -- \
            include/alp docs/abi src/cap.c src/status_strings.c \
            metadata/catalog.json metadata/error-catalog.json metadata/pinmux \
            metadata/socs/renesas/rzv2n/n44.json \
            docs/portability-matrix.md docs/peripheral-support-matrix.md \
            docs/verification-status.md \
            docs/diagnostics 2>/dev/null | tail -20
        return 1
    fi

    # Every generator ran clean and nothing drifted -- but if one or more
    # skipped for lack of clang-format, that coverage gap is real and must
    # stay visible, not collapse into a plain PASS: SKIP, named (#1221).
    if [ "${gen_skipped}" -gt 0 ]; then
        echo "generated-files SKIP (${gen_skipped} of ${gen_total} generators need clang-format)"
        return 99
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

    # Shell-script static lint -- catches shell bugs (cd-without-guard,
    # set-u empty-array traps, POSIX slips) on Linux before they redden
    # macOS/Windows CI.  Skips cleanly if shellcheck isn't installed.
    if command -v shellcheck >/dev/null 2>&1 || [ -x "${HOME}/.local/bin/shellcheck" ]; then
        run_stage "shellcheck" stage_shellcheck
    else
        skip_stage "shellcheck" "shellcheck not installed (PATH or ~/.local/bin)"
    fi

    # bash 3.2 parse gate -- catches the class of defect PR #1050 hit
    # that shellcheck (above) and `bash -n` on a modern bash both miss
    # (see stage_bash32_parse for the apostrophe/heredoc mechanism).
    # Skips cleanly if neither podman nor docker is on PATH -- CI's
    # macOS leg (cross-platform-zephyr.yml python-smoke) still runs
    # this unconditionally with real bash 3.2.57, no container needed
    # there.
    if command -v podman >/dev/null 2>&1 || command -v docker >/dev/null 2>&1; then
        run_stage "bash32-parse" stage_bash32_parse
    else
        skip_stage "bash32-parse" "neither podman nor docker on PATH -- CI's macos-latest python-smoke leg runs this unconditionally with real bash 3.2.57"
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

    # Mirrors cross-platform-zephyr.yml's python-smoke --fail-on-warning
    # step (alp-sdk#1032 A5) so a repo drifting back to N findings is
    # caught locally, not only on three legs of a non-required workflow.
    if [ -f scripts/check_cross_platform.py ]; then
        run_stage "cross-platform-lint" stage_cross_platform_lint
    else
        skip_stage "cross-platform-lint" "scripts/check_cross_platform.py missing"
    fi

    # Required scripts/check_*.py gates -- see REQUIRED_GATE_SCRIPTS
    # above.  Keeps this wrapper's coverage aligned with the hard
    # gates pr-metadata-validate.yml / pr-doc-drift.yml run in CI.
    run_stage "required-gate-scripts" stage_required_gate_scripts

    # `check · generated files in sync` -- regenerate every single-sourced
    # artifact + fail on drift.  Catches the class of red that bit #623 /
    # #636 / #642 (new macro/symbol/gate without a committed regen).
    run_stage "generated-files" stage_generated_files

    # alp.lock --check -- both the dev (fast) and main (release-grade)
    # profiles run this unconditionally, same as metadata-validate above.
    # MUST run after generated-files: that stage rewrites metadata/catalog.json
    # and metadata/error-catalog.json in place, and both are now lock-covered
    # (#1045) -- checking the lock first would pass on pre-regen bytes and
    # leave a just-regenerated catalog unverified.
    run_stage "alp-lock" stage_alp_lock

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
