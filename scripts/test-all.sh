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
#   4. clang-format diff vs merge-base origin/dev (skipped if no clang-format)
#   5. shellcheck over every shipped *.sh (repo-wide `git ls-files
#      '*.sh'`; skipped if that tool isn't installed)
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
# Each stage prints `[stage] PASS` or `[stage] FAIL`.  A stage function
# signals "prerequisite not available" (a missing tool, env var, or
# importable module -- e.g. ZEPHYR_BASE unset, or a python3 that can't
# `import natsort`/`pytest`) by returning exit code 99;
# run_stage() turns that into `[stage] SKIP`, never `FAIL` -- a missing
# prerequisite is not a defect in the tree.
#
# Exit codes:
#   0 -- every stage that ran passed, AND nothing was skipped for a
#        missing prerequisite.  A complete run for the chosen --target.
#   1 -- at least one stage FAILED.  See the per-stage output above the
#        summary.
#   2 -- no stage FAILED, but at least one REQUIRED stage was SKIPPED
#        for a missing prerequisite (tagged `[GAP]` in the summary) --
#        e.g. twister SKIPping because ZEPHYR_BASE is unset.  This is a
#        PARTIAL run: something that should have been checked was not,
#        so "0 failures" must not be read as "fully verified"
#        (alp-sdk#1396).  Install/configure the missing prerequisite
#        and re-run.
#
# A `[GAP]`-tagged SKIP is not the same thing as a plain SKIP: `--quick`
# and `--target dev` also skip stages (twister, doxygen, the release-only
# CMake builds), but those are a DELIBERATE, in-scope choice for the
# profile requested -- they print with no `[GAP]` tag and do not affect
# the exit code, because the run is still complete for what it claims to
# cover.  Same word ("SKIP") in the per-stage line either way; the
# summary's `[GAP]` tag and the exit code are what distinguish "chose not
# to run this" from "tried to run this and couldn't".
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
#                     opening a PR that targets `dev`.  Twister is skipped
#                     when scripts/select_checks.py proves no changed path
#                     (vs the merge base with origin/dev, plus uncommitted
#                     and untracked files) is a native_sim build input --
#                     e.g. a docs/changelog/Yocto/pytest-only change.  Any
#                     doubt (unknown path, lookup failure, no origin/dev)
#                     runs it.  --select-base REF overrides the base ref.
#                     When it cannot prove that, twister still does NOT run
#                     the full ~270-config set locally: it runs a bounded
#                     SMOKE subset (the suites the changed files sit in plus
#                     the fixed SMOKE_SUITES in scripts/select_checks.py,
#                     ~2-3 min) and the row reads `PASS (smoke; full set runs
#                     in CI)`.  The full set is CI's sharded pr-twister job.
#   --full            always run the FULL twister set here (no selection, no
#                     smoke subset).  `--target main` does too.
#   --target main     THOROUGH release-grade profile: every stage PLUS the
#                     main-only strict ABI-snapshot diff (pr-abi-snapshot.yml,
#                     which triggers on main + release/** only).  Use before a
#                     PR that targets `main` / cutting a release.
#                     (No --target = every stage except the main-only ABI
#                     strict diff; its twister stage is the same changed
#                     suites + smoke subset as --target dev, not the full
#                     set.)
#   --quick           skip twister + Doxygen (the slow stages)
#
# Environment:
#   ALP_TWISTER_JOBS  cap twister's concurrent TEST INSTANCES (passed through
#                     as `-j`).  Unset = twister's own default, one per core.
#                     NOTE this alone is NOT enough -- each instance runs its
#                     own ninja at full core parallelism underneath, so the
#                     real compiler count is this value TIMES the core count.
#                     The stage caps that inner build too; see the OOM note on
#                     stage_twister below.  Setting it also turns the
#                     twister/read-only-stage overlap off (see below).
#   ALP_GATE_SERIAL=1 run every stage strictly one after another with live
#                     output.  By default twister runs in the background and
#                     the read-only stages run as a bounded concurrent pool
#                     (slow ones first) -- whether or not twister runs -- with
#                     their output printed in stage order afterwards; the
#                     tree-writing stages (generated-files, alp-lock,
#                     abi-strict and the `pytest-repo-writes` row) run only
#                     once twister and the whole pool have finished.
#                     Overlap is also skipped when ALP_TWISTER_JOBS is set or
#                     MemAvailable < ALP_GATE_MIN_MEM_KB (default 12582912).
#   ALP_GATE_STAGE_JOBS
#                     max read-only stages running at once in the pool
#                     (default 4).
#   `gate_duplicate` pytest tests (a check the public-private /
#                     required-gate-scripts stages already ran) are deselected
#                     from the pytest stage of a full run; CI's plain pytest
#                     sweep keeps them.
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
FORCE_FULL=0
# Set when the local twister stage runs the bounded smoke subset (see the
# orchestration): TWISTER_SUITES are the suite directories it builds.
TWISTER_SMOKE=0
TWISTER_SUITES=()
SELECT_BASE=origin/dev
LIST_REQUIRED_GATE_SCRIPTS=0
# TARGET selects a CI profile matching the branch a PR targets:
#   dev  -- the FAST set a dev PR is graded on (skip the slow release-only
#           full CMake builds + Doxygen); for rapid integration iteration.
#   main -- the THOROUGH release-grade set: everything dev runs PLUS the
#           full yocto/baremetal builds, the Doxygen build, and the
#           main-only strict ABI-snapshot diff (pr-abi-snapshot.yml, which
#           triggers on `main` + `release/**` only).
#   full -- (default, no flag) every stage except the main-only ABI strict
#           diff.  Its twister stage behaves like dev's: skipped when
#           select_checks.py proves nothing native_sim builds changed,
#           otherwise the changed suites + the smoke subset -- NOT the full
#           set, which is CI's sharded pr-twister job.  Only --target main
#           and --full run the full twister set locally.
TARGET=full

while [ $# -gt 0 ]; do
    case "$1" in
        --quick)        QUICK=1 ;;
        --yocto-only)   YOCTO_ONLY=1 ;;
        --zephyr-only)  ZEPHYR_ONLY=1 ;;
        --no-clean)     NO_CLEAN=1 ;;
        --full)         FORCE_FULL=1 ;;
        --select-base)  shift; SELECT_BASE="${1:-}" ;;
        --select-base=*) SELECT_BASE="${1#--select-base=}" ;;
        --target)       shift; TARGET="${1:-}" ;;
        --target=*)     TARGET="${1#--target=}" ;;
        --dev)          TARGET=dev ;;
        --main)         TARGET=main ;;
        --list-required-gate-scripts) LIST_REQUIRED_GATE_SCRIPTS=1 ;;
        -h|--help)
            sed -n '3,/^set -uo pipefail/{/^set -uo pipefail/!p;}' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

declare -a STAGE_NAMES STAGE_STATUS STAGE_NOTES STAGE_KIND
# STAGE_KIND is only meaningful for STAGE_STATUS=SKIP rows, and is one of:
#   scope -- deliberately out of scope for THIS run (an explicit --quick /
#            --target flag chose not to run it; --target dev's release-grade
#            doxygen/yocto/baremetal stages are the canonical example). The
#            run is still COMPLETE for the profile it claims to be.
#   gap   -- the stage tried to run and could not, for a missing tool /
#            env var / importable module / script. Real local coverage did
#            NOT happen, regardless of how the run otherwise reads (issue
#            alp-sdk#1396: a `ZEPHYR_BASE` gap here once still printed "All
#            runnable stages passed." and exited 0).
# PASS/FAIL rows carry "" so the four arrays stay index-aligned.

# Wall seconds per stage, index-aligned with the arrays above.
declare -a STAGE_SECS

# _record_stage NAME RC SECS -- classify a stage's exit code, append its row,
# print its one-line verdict.  Shared by the live path (run_stage) and the
# overlapped path (finish_overlap) so both read identically.
_record_stage() {
    local name="$1" rc="$2" secs="$3"
    # Convention: a stage function returns 99 to mean "a prerequisite
    # (tool / optional script / env var / importable module) isn't
    # available here" -- that is always a SKIP, never a FAIL, and always
    # STAGE_KIND=gap: a stage function only returns 99 because something
    # it needed to actually run was missing, which is a gap in coverage
    # by construction -- never a deliberate scope choice (those are
    # expressed via skip_stage()'s explicit "scope" callers below,
    # before run_stage() is ever reached for that stage this run).
    if [ "${rc}" -eq 99 ]; then
        STAGE_NAMES+=("${name}"); STAGE_STATUS+=("SKIP"); STAGE_KIND+=("gap"); STAGE_NOTES+=("prerequisite unavailable"); STAGE_SECS+=("${secs}")
        echo "[${name}] SKIP (prerequisite unavailable) (${secs}s)"
    elif [ "${rc}" -eq 0 ]; then
        # A smoke twister pass is a deliberate scope choice, not full coverage:
        # say so on the row so a local green is never read as the full set.
        local note=""
        if [ "${name}" = "twister" ] && [ "${TWISTER_SMOKE:-0}" = "1" ]; then
            note="smoke; full set runs in CI"
        fi
        STAGE_NAMES+=("${name}"); STAGE_STATUS+=("PASS"); STAGE_KIND+=(""); STAGE_NOTES+=("${note}"); STAGE_SECS+=("${secs}")
        echo "[${name}] PASS${note:+ (${note})} (${secs}s)"
    else
        STAGE_NAMES+=("${name}"); STAGE_STATUS+=("FAIL"); STAGE_KIND+=(""); STAGE_NOTES+=("exit=${rc}"); STAGE_SECS+=("${secs}")
        echo "[${name}] FAIL (exit=${rc}) (${secs}s)"
    fi
}

run_stage() {
    local name="$1"; shift
    echo
    echo "===== [${name}] ====="
    local t0="${SECONDS}"
    "$@"
    local rc=$?
    _record_stage "${name}" "${rc}" "$((SECONDS - t0))"
}

skip_stage() {
    local name="$1"; local reason="$2"; local kind="${3:-}"
    case "${kind}" in
        scope|gap) ;;
        *)
            echo "test-all.sh: internal error: skip_stage '${name}' called with kind='${kind}', want scope|gap" >&2
            exit 70
            ;;
    esac
    echo
    echo "===== [${name}] SKIP: ${reason} ====="
    STAGE_NAMES+=("${name}"); STAGE_STATUS+=("SKIP"); STAGE_KIND+=("${kind}"); STAGE_NOTES+=("${reason}"); STAGE_SECS+=("0")
}

# -------- Overlapped execution ------------------------------------------------
#
# Two things run concurrently by default, instead of one stage after another:
#   * twister (~70-90 % of a --target dev run when it is not skipped) starts in
#     the BACKGROUND (launch_bg), and
#   * every READ-ONLY stage (launch) joins a bounded pool of at most
#     ALP_GATE_STAGE_JOBS (default 4) concurrent stages, started longest-first
#     (POOL_FIRST) so the pool's tail is the short stages.
# The pool runs whether or not twister is backgrounded: a run that skips
# twister (docs-only change) still overlaps its slow stages with each other.
#
# Stages that WRITE the tree (generated-files, alp-lock, abi-strict,
# pytest -m repo_writes) are queued with writer_stage and run only after
# twister AND every pool stage has been reaped: regenerating headers under a
# live twister build flakes it (`ALP_SOC_REF_STR undeclared`).
#
# Every overlapped stage's output goes to a file and is printed, in script
# order, once everything is done, so the log reads the same as a serial run.
# bash 3.2: no `wait -n`, no associative arrays -- parallel arrays plus one
# file trio (.out/.res/.skip) per stage under DEFER_DIR.
#
# Overlap is OFF (strictly serial, live output, today's behaviour) when any of:
#   ALP_GATE_SERIAL=1   the explicit opt-out, for debugging;
#   ALP_TWISTER_JOBS    set -- the documented "this host is memory-tight"
#                       signal (see the OOM note on stage_twister); running
#                       pytest and the gate scripts beside a capped twister
#                       would undo that cap;
#   MemAvailable        below ALP_GATE_MIN_MEM_KB (default 12 GiB) at start --
#                       twister alone peaks near the OOM line on a 20-core host.
# Whenever stages overlap, each pool stage runs with capped concurrency of its
# own (ALP_GATE_JOBS / CMAKE_BUILD_PARALLEL_LEVEL default 4, pytest -n
# min(8, nproc/2)) so the pool and twister together stay inside the machine.
_decide_overlap() {
    if [ "${ALP_GATE_SERIAL:-0}" = "1" ]; then
        return 1
    fi
    if [ -n "${ALP_TWISTER_JOBS:-}" ]; then
        echo "test-all.sh: ALP_TWISTER_JOBS is set -- running stages serially (no overlap)."
        return 1
    fi
    local avail_kb min_kb="${ALP_GATE_MIN_MEM_KB:-12582912}"
    avail_kb="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || true)"
    if [ -n "${avail_kb}" ] && [ "${avail_kb}" -lt "${min_kb}" ]; then
        echo "test-all.sh: MemAvailable ${avail_kb} kB < ${min_kb} kB -- running stages serially (no overlap)."
        return 1
    fi
    return 0
}
OVERLAP=1
_decide_overlap || OVERLAP=0
DEFER_DIR=""
# Initialised to () (not merely declared): `${#a[@]}` on a declared-only array
# is an unbound-variable error under `set -u` on newer bash.
DEFER_ORDER=(); WRITER_NAMES=(); WRITER_FNS=(); POOL_NAMES=(); POOL_FNS=()
RUN_PIDS=(); RUN_NAMES=()
TWISTER_PID=""
# The slowest read-only stages, started first (measured on the 20-core gate
# host: pytest-scripts ~160 s, required-gate-scripts ~55-390 s, bash32-parse ~57 s).
POOL_FIRST="pytest-scripts required-gate-scripts bash32-parse"

# EXIT trap: stop the background twister's and every running pool stage's whole
# process group (wrapper subshell, python, ninja, cc1 ...), and on an abnormal
# exit print any captured output that was never shown so a failure is not
# silently lost.
_overlap_cleanup() {
    local rc=$? f p
    for p in ${RUN_PIDS[@]+"${RUN_PIDS[@]}"} ${TWISTER_PID:+"${TWISTER_PID}"}; do
        kill -TERM -- "-${p}" 2>/dev/null || kill -TERM "${p}" 2>/dev/null
        wait "${p}" 2>/dev/null
    done
    RUN_PIDS=(); TWISTER_PID=""
    if [ -n "${DEFER_DIR}" ]; then
        if [ "${rc}" -ne 0 ]; then
            for f in "${DEFER_DIR}"/*.out; do
                [ -f "${f}" ] || continue
                echo "--- captured output never printed: ${f##*/} ---"
                cat "${f}"
            done
        fi
        rm -rf "${DEFER_DIR}"
    fi
}

_defer_init() {
    if [ -z "${DEFER_DIR}" ]; then
        DEFER_DIR="$(mktemp -d)"
        trap _overlap_cleanup EXIT
        trap 'exit 130' INT
        trap 'exit 143' TERM
    fi
}

# launch NAME FN -- a read-only stage.  Overlap off: live, like run_stage.
# Overlap on: queued for the pool (started by finish_overlap).
launch() {
    if [ "${OVERLAP}" -eq 0 ]; then run_stage "$@"; return; fi
    _defer_init
    DEFER_ORDER+=("$1")
    POOL_NAMES+=("$1"); POOL_FNS+=("$2")
}

# _start_detached NAME FN -- run FN in its own process group (`set -m` so
# _overlap_cleanup can kill the whole tree; non-interactive bash would leave it
# in ours), output and result to files.  Sets STARTED_PID.
_start_detached() {
    local name="$1" fn="$2"
    set -m
    (
        t0="${SECONDS}"
        export ALP_GATE_JOBS="${ALP_GATE_JOBS:-4}"
        export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
        export ALP_GATE_OVERLAP=1
        # </dev/null: the job is in its own process group, so a stage that
        # read the terminal would be stopped by SIGTTIN and hang the run.
        "${fn}" >"${DEFER_DIR}/${name}.out" 2>&1 </dev/null
        rc=$?
        echo "${rc} $((SECONDS - t0))" >"${DEFER_DIR}/${name}.res"
    ) &
    STARTED_PID=$!
    set +m
}

# launch_bg NAME FN -- twister: starts NOW, in the background.
launch_bg() {
    if [ "${OVERLAP}" -eq 0 ]; then run_stage "$@"; return; fi
    _defer_init
    DEFER_ORDER+=("$1")
    echo "===== [$1] started in the background; read-only stages run beside it ====="
    _start_detached "$1" "$2"
    TWISTER_PID="${STARTED_PID}"
}

# launch_skip NAME REASON KIND -- a skip keeps its place in the output order.
launch_skip() {
    if [ "${OVERLAP}" -eq 0 ]; then skip_stage "$@"; return; fi
    _defer_init
    DEFER_ORDER+=("$1")
    printf '%s\n%s\n' "$2" "${3:-}" >"${DEFER_DIR}/$1.skip"
}

# writer_stage NAME FN -- a stage that writes the tree; runs after everything.
writer_stage() {
    if [ "${OVERLAP}" -eq 0 ]; then run_stage "$@"; return; fi
    WRITER_NAMES+=("$1"); WRITER_FNS+=("$2")
}

# _run_pool -- run the queued read-only stages, at most ALP_GATE_STAGE_JOBS at a
# time, POOL_FIRST stages first, then the rest in script order.  Returns once
# every one has finished.  A stage is finished when its .res exists or its
# process is gone (killed before it could write one -> read as FAIL later).
_run_pool() {
    local jobs="${ALP_GATE_STAGE_JOBS:-4}" n="${#POOL_NAMES[@]}" i j pri
    local next=0 alive
    local -a order=() keep_pids keep_names
    [ "${jobs}" -ge 1 ] 2>/dev/null || jobs=4
    for pri in ${POOL_FIRST}; do
        for ((i = 0; i < n; i++)); do
            [ "${POOL_NAMES[$i]}" = "${pri}" ] && order+=("${i}")
        done
    done
    for ((i = 0; i < n; i++)); do
        case " ${POOL_FIRST} " in
            *" ${POOL_NAMES[$i]} "*) ;;
            *) order+=("${i}") ;;
        esac
    done
    while [ "${next}" -lt "${n}" ] || [ "${#RUN_PIDS[@]}" -gt 0 ]; do
        keep_pids=(); keep_names=()
        for ((j = 0; j < ${#RUN_PIDS[@]}; j++)); do
            alive=1
            if [ -f "${DEFER_DIR}/${RUN_NAMES[$j]}.res" ] || ! kill -0 "${RUN_PIDS[$j]}" 2>/dev/null; then
                alive=0
            fi
            if [ "${alive}" -eq 1 ]; then
                keep_pids+=("${RUN_PIDS[$j]}"); keep_names+=("${RUN_NAMES[$j]}")
            else
                wait "${RUN_PIDS[$j]}" 2>/dev/null
            fi
        done
        RUN_PIDS=(${keep_pids[@]+"${keep_pids[@]}"}); RUN_NAMES=(${keep_names[@]+"${keep_names[@]}"})
        while [ "${#RUN_PIDS[@]}" -lt "${jobs}" ] && [ "${next}" -lt "${n}" ]; do
            i="${order[$next]}"
            _start_detached "${POOL_NAMES[$i]}" "${POOL_FNS[$i]}"
            RUN_PIDS+=("${STARTED_PID}"); RUN_NAMES+=("${POOL_NAMES[$i]}")
            next=$((next + 1))
        done
        [ "${#RUN_PIDS[@]}" -gt 0 ] && sleep 0.2
    done
}

# finish_overlap -- run the pool, join twister, print every captured stage in
# script order, then run the queued tree-writing stages live.
finish_overlap() {
    [ -n "${DEFER_DIR}" ] || return 0
    if [ "${#POOL_NAMES[@]}" -gt 0 ]; then
        _run_pool
    fi
    if [ -n "${TWISTER_PID}" ]; then
        echo
        echo "===== waiting for the background twister stage ====="
        wait "${TWISTER_PID}"
        TWISTER_PID=""
    fi
    local name rc secs i sreason skind
    for name in ${DEFER_ORDER[@]+"${DEFER_ORDER[@]}"}; do
        if [ -f "${DEFER_DIR}/${name}.skip" ]; then
            { read -r sreason; read -r skind; } <"${DEFER_DIR}/${name}.skip"
            # Positional pass-through: skip_stage still validates scope|gap.
            set -- "${name}" "${sreason}" "${skind}"
            skip_stage "$@"
            continue
        fi
        echo
        echo "===== [${name}] ====="
        cat "${DEFER_DIR}/${name}.out"
        rm -f "${DEFER_DIR}/${name}.out"
        rc=1; secs=0
        read -r rc secs 2>/dev/null <"${DEFER_DIR}/${name}.res" || true
        _record_stage "${name}" "${rc}" "${secs}"
    done
    for i in ${WRITER_NAMES[@]+"${!WRITER_NAMES[@]}"}; do
        run_stage "${WRITER_NAMES[$i]}" "${WRITER_FNS[$i]}"
    done
}

# `import jsonschema` succeeding says NOTHING about whether that
# jsonschema has the API the gate scripts call.  `Draft202012Validator`
# arrived in jsonschema 4.0; a distro-packaged 3.2.0 imports cleanly and
# then dies partway through a stage with
#
#     AttributeError: module 'jsonschema' has no attribute
#     'Draft202012Validator'. Did you mean: 'Draft3Validator'?
#
# which surfaced as FOUR stages FAILING on a clean tree
# (metadata-validate, doc-yaml-fragments, required-gate-scripts,
# generated-files) instead of SKIPping for the missing prerequisite they
# actually have -- alp-sdk#1423.  That is #1396's harm ("go red on a
# clean tree") reached by a different route: #1396 fixed the module
# being ABSENT, this is the module being PRESENT BUT TOO OLD, which no
# `import` check can catch.
#
# Probe the attribute, not the import -- the same shape
# stage_pytest_scripts uses for pytest.  Deliberately NOT
# fixed inside the 18 `scripts/*.py` files that call
# `Draft202012Validator`: those are also run DIRECTLY by
# pr-metadata-validate.yml and friends, where a 99 exit is a failing
# check, so teaching them to exit 99 would trade a wrong local verdict
# for a red CI.  The wrong verdict is test-all.sh's, so the fix is
# test-all.sh's.
have_jsonschema_2020() {
    python3 -c 'import jsonschema; jsonschema.Draft202012Validator' >/dev/null 2>&1
}

require_jsonschema_2020() {
    local stage="$1"
    if ! have_jsonschema_2020; then
        local found
        found="$(python3 -c 'import jsonschema; print(jsonschema.__version__)' 2>/dev/null || echo 'not installed')"
        echo "${stage}: python3 ($(command -v python3)) has jsonschema ${found}, which has no Draft202012Validator (needs jsonschema >= 4.0). Activate the zephyrproject venv or: pip install 'jsonschema>=4'."
        return 99
    fi
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

# Pin THIS checkout as the alp-sdk Zephyr module for every stage.  Called at
# orchestration start, before any stage forks: a background stage (#2472
# overlap) exports into its own subshell only, so pinning from inside
# stage_twister left pytest-scripts and required-gate-scripts, whose
# `west build --cmake-only` probes need the module, with no alp boards.
pin_alp_zephyr_module() {
    # Always, even if EXTRA_ZEPHYR_MODULES is already set (e.g. exported from a
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
}

stage_twister() {
    if [ -z "${ZEPHYR_BASE:-}" ]; then
        return 99
    fi
    # ZEPHYR_BASE being set proves nothing about the python3 that will
    # actually run twister below: on a system interpreter without
    # natsort, twister's own import chain
    # (zephyr/scripts/pylib/twister/twisterlib/hardwaremap.py does
    # `from natsort import natsorted`) raises ModuleNotFoundError before
    # a single test runs -- previously that read as this stage FAILING
    # ("nothing wrong with the tree" reported red) instead of the
    # missing-prerequisite SKIP it actually is (alp-sdk#1396). Same
    # `return 99` idiom as the ZEPHYR_BASE check above.
    if ! python3 -c 'import natsort' >/dev/null 2>&1; then
        echo "stage_twister: python3 ($(command -v python3)) cannot import natsort, which twister's own hardwaremap.py requires. Activate the zephyrproject venv (or: pip install natsort) so a python3 with it resolves first on PATH."
        return 99
    fi
    # ccache survives a fresh clone / new worktree only if the object key is
    # path-independent.  Zephyr's build uses ccache when it is on PATH, but
    # ccache's defaults hash absolute source paths and, because the build
    # compiles with -g, the build directory itself (hash_dir) -- so every
    # new checkout path was a cold cache.  Relativise paths under this
    # checkout and stop hashing the cwd, unless the user's ccache config
    # already made a choice (an env var here would override it).
    if command -v ccache >/dev/null 2>&1; then
        if [ -z "${CCACHE_BASEDIR:-}" ] && [ -z "$(ccache -k base_dir 2>/dev/null)" ]; then
            export CCACHE_BASEDIR="${REPO_ROOT}"
        fi
        if [ -z "${CCACHE_HASHDIR:-}${CCACHE_NOHASHDIR:-}" ] \
            && [ "$(ccache -k hash_dir 2>/dev/null)" = "true" ]; then
            export CCACHE_NOHASHDIR=1
        fi
    fi
    # Match pr-twister.yml FAITHFULLY: same testsuite roots (incl.
    # tests/unit) AND warnings-as-errors.  Twister builds strict in CI, so
    # a warning like -Werror=comment ('/*' inside a comment) fails there;
    # forcing CONFIG_COMPILER_WARNINGS_AS_ERRORS=y here catches that class
    # locally instead of on the PR (bit examples/.../u8g2 main.c, #650).
    # Concurrency cap.  Twister defaults to ONE BUILD JOB PER CORE and each build
    # runs its own parallel ninja underneath, so the real compiler count is well
    # above the core count.  Measured on the 20-core / 31 GB bench gateway: a
    # default-parallelism run OOM-killed the machine, the kernel reaping cc1plus
    # repeatedly ("Out of memory: Killed process ... (cc1plus) ...
    # anon-rss:425060kB") until the box had to be rebooted -- which on THAT
    # machine also takes the attached board farm down with it.
    #
    # Unset keeps twister's default, so CI is unchanged.  Set ALP_TWISTER_JOBS
    # on a shared or memory-tight host; roughly one job per 2 GB of RAM is a
    # safe starting point, and lower still if anything else heavy is running.
    twister_jobs=()
    if [ -n "${ALP_TWISTER_JOBS:-}" ]; then
        twister_jobs=(-j "${ALP_TWISTER_JOBS}")

        # Capping twister ALONE IS NOT ENOUGH, and believing it was cost a
        # second near-OOM.  `-j` bounds concurrent TEST INSTANCES; each instance
        # then runs its own ninja, which defaults to the full core count.  So
        # the real concurrent-compiler count is ALP_TWISTER_JOBS x nproc.
        # Measured on this 20-core host with ALP_TWISTER_JOBS=4: 78 live cc1plus
        # processes, 27 of 31 GB consumed, load average 152 -- the run had to be
        # killed to avoid repeating the OOM reboot the cap was added to prevent.
        #
        # CMAKE_BUILD_PARALLEL_LEVEL is what bounds the inner build (it is also
        # what the bench runner uses for the same reason).  Default it to 2 so
        # the product stays modest, and let a caller override it deliberately.
        export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
        echo "stage_twister: capping twister at ${ALP_TWISTER_JOBS} concurrent test instance(s) (ALP_TWISTER_JOBS)," \
             "each building with CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL}" \
             "-- up to $((ALP_TWISTER_JOBS * CMAKE_BUILD_PARALLEL_LEVEL)) concurrent compilers"
    fi

    # --clobber-output: delete the previous twister-out/ instead of renaming
    # it to twister-out.N -- in a reused checkout the renamed copies (several
    # GB each) otherwise pile up run after run.
    # The full run scans all four roots; the local smoke run scans only the
    # suite directories select_checks.py --local chose.
    local -a roots=()
    local suite
    if [ "${#TWISTER_SUITES[@]}" -gt 0 ]; then
        echo "stage_twister: SMOKE subset (${#TWISTER_SUITES[@]} suite dir(s)); the full native_sim set runs in CI's pr-twister shards -- pass --full to run it here:"
        for suite in "${TWISTER_SUITES[@]}"; do
            echo "  ${suite}"
            roots+=(--testsuite-root "${REPO_ROOT}/${suite}")
        done
    else
        roots=(--testsuite-root "${REPO_ROOT}/tests/unit"
               --testsuite-root "${REPO_ROOT}/tests/zephyr"
               --testsuite-root "${REPO_ROOT}/tests/console"
               --testsuite-root "${REPO_ROOT}/examples")
    fi
    # Examples' testcase.yaml files point EXTRA_CONF_FILE at their
    # generated/alp.conf; twister configures from the source tree, so write
    # those fragments first (#866).
    python3 "${REPO_ROOT}/scripts/gen_example_alp_conf.py" || return 1
    python3 "${ZEPHYR_BASE}/scripts/twister" \
        "${twister_jobs[@]+"${twister_jobs[@]}"}" \
        "${roots[@]}" \
        -p native_sim/native/64 \
        --extra-args=CONFIG_COMPILER_WARNINGS_AS_ERRORS=y \
        --clobber-output \
        --inline-logs \
        --no-detailed-test-id
}

stage_shellcheck() {
    # Static-lint EVERY shipped shell script so a shell bug (an SC2164
    # cd-without-guard, a `set -u` empty-array trap, a POSIX-portability
    # slip that only bites macOS's bash 3.2) is caught on Linux BEFORE it
    # reddens macOS/Windows python-smoke.  Resolve shellcheck on PATH or
    # the no-root ~/.local/bin install.  NOTE: unlike
    # pr-static-analysis.yml's CI mirror (pinned to the v0.10.0 upstream
    # release tarball), this resolves whatever shellcheck version is
    # already installed locally -- a version skew here can disagree with
    # CI about what counts as clean; install v0.10.0 locally to match.
    local sc
    sc=$(command -v shellcheck 2>/dev/null || true)
    if [ -z "${sc}" ] && [ -x "${HOME}/.local/bin/shellcheck" ]; then
        sc="${HOME}/.local/bin/shellcheck"
    fi
    if [ -z "${sc}" ]; then
        return 99
    fi
    # #1550: widened from a flat `scripts/*.sh` glob (which only reached
    # scripts/ itself, one level deep) to a repo-wide `git ls-files '*.sh'`
    # sweep -- matching stage_bash32_parse's existing repo-wide `*.sh`
    # sweep below. Before this, two groups shipped unchecked: root
    # scripts/*.sh had no CI coverage at all (only this local stage), and 8
    # files entirely outside scripts/ (
    # keys/generate_dev_key.sh,
    # meta-alp-sdk/recipes-core/alp-system/files/alp-remoteproc-start.sh,
    # tests/yocto/*.sh, tools/native-sim-container/entrypoint.sh) were
    # linted nowhere at all, in CI or locally.
    #
    # test-all.sh is the load-bearing local-CI wrapper (it runs in a macOS
    # CI test, test_test_all_worktree.py), so lint it at warning level.
    # scripts/bench/** keeps the -S warning -x bar #1527 set (SC1012, the
    # #1478 stray-`\n` class, is warning-severity, so -S error alone would
    # not have caught it; -x follows `source` so bench-env.sh's
    # cross-file reads don't false-positive SC2034). Everything else --
    # every other file `git ls-files '*.sh'` returns -- runs at -S error,
    # matching this stage's own prior non-test-all.sh severity and
    # onramp-clean-container.yml's scripts/bootstrap.sh step.
    local -a all_files=()
    local f
    while IFS= read -r f; do
        all_files+=("${f}")
    done < <(git ls-files '*.sh')
    # An empty file list must never read as "0 broken files == PASS" --
    # that is a silent-empty-loop, the exact shape of gate this PR argues
    # against, and stage_bash32_parse below guards the identical case.
    if [ "${#all_files[@]}" -eq 0 ]; then
        echo "stage_shellcheck: 'git ls-files *.sh' returned NO files -- refusing to report a silent pass."
        return 1
    fi
    local rc=0
    for f in "${all_files[@]}"; do
        case "${f}" in
        scripts/test-all.sh)
            "${sc}" -S warning "${f}" || rc=1
            ;;
        scripts/bench/*)
            "${sc}" -x -S warning "${f}" || rc=1
            ;;
        *)
            "${sc}" -S error "${f}" || rc=1
            ;;
        esac
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
    # Default: the merge-base with origin/dev, the same base
    # pr-static-analysis.yml diffs (`git merge-base $BASE_SHA HEAD`), so
    # this grades every line the PR changes. HEAD~1 is NOT equivalent: on
    # a batch branch built with `git merge --no-edit` (or any branch with
    # more than one commit) it is only the LAST merge's delta and hides
    # everything merged before it. Falls back to HEAD~1 when origin/dev is
    # absent or HEAD sits on it (merge-base == HEAD would diff nothing),
    # matching CI's push/merge_group `HEAD~1`. $DIFF_BASE overrides.
    local base="${DIFF_BASE:-}"
    if [ -z "${base}" ]; then
        base=$(git merge-base origin/dev HEAD 2>/dev/null || true)
        if [ -z "${base}" ] || [ "${base}" = "$(git rev-parse HEAD)" ]; then
            base="HEAD~1"
        fi
    fi
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
    require_jsonschema_2020 stage_metadata_validate || return 99
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
    # Mirrors pr-metadata-validate.yml's `alp.lock --check` step (#1045).
    # alp.lock is no longer committed (#1576 -- its `digests.metadata` is a
    # single hash over the whole metadata/** tree, so any two PRs touching
    # different metadata files conflicted on this one line by construction).
    # `--check` therefore GENERATES a lock in memory and schema-validates it
    # rather than diffing against a tracked copy; it still catches a broken
    # generator (schema mismatch) and a local-path leak (`_reject_local`).
    # No "script missing" skip: scripts/west_commands/alp_lock.py is a
    # tracked repo file, so a missing/renamed script means the gate itself
    # vanished and that must redden, not SKIP silently (same reasoning as
    # stage_generated_files above).
    #
    # alp_lock.py imports jsonschema at module scope for its draft 2020-12
    # schema validation -- same missing/too-old-prerequisite trap as
    # stage_metadata_validate and stage_doc_yaml_fragments above (#1396/#1423).
    require_jsonschema_2020 stage_alp_lock || return 99
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
    require_jsonschema_2020 stage_doc_yaml_fragments || return 99
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
    # python3 existing on PATH says nothing about whether IT has pytest:
    # a system interpreter without pytest made `python3 -m pytest` below
    # exit 1 with "No module named pytest" -- a genuine module-not-found
    # on a clean tree, previously reported as this stage FAILING rather
    # than SKIPping for the missing prerequisite it actually is
    # (alp-sdk#1396). pytest_mock used to be checked here too, gating
    # entry the same way the ZEPHYR_BASE / natsort checks gate
    # stage_twister above -- it was the stage's only pytest-mock
    # consumer (test_alp_cli.py / test_alp_cli_emit.py), both retired by
    # the alp_cli CLI-wrapper retirement (#1367/#1368); no test under
    # tests/scripts/ uses the `mocker` fixture any more.
    if ! python3 -c 'import pytest' >/dev/null 2>&1; then
        echo "stage_pytest_scripts: python3 ($(command -v python3)) cannot import pytest. Activate the zephyrproject venv or: pip install pytest."
        return 99
    fi
    # pytest-xdist (a [dev] dependency since #2328) spreads the ~4,400 tests
    # over every core, as CI does: a parallel sweep, then the modules that
    # write into the real checkout on their own (tests/scripts/conftest.py
    # _REPO_WRITER_MODULES). Without xdist the stage still runs, serially.
    # phase: all (default) | parallel (read-only sweep + tests/parity) |
    # writes (only the repo_writes modules).  The overlapped run splits the
    # stage so the parallel half can run beside twister and the half that
    # writes the checkout runs after it.
    local phase="${1:-all}"
    # `gate_duplicate` tests re-run, from pytest, the live-repo check a gate
    # stage of THIS invocation already ran (public-private,
    # required-gate-scripts).  Skip them whenever the full-run branch
    # schedules those stages (SKIP_GATE_DUPLICATES; a gap in one of them shows
    # as [GAP], exit 2); CI's plain pytest sweep and --zephyr-only keep them.
    local dup="" alldup=""
    if [ "${SKIP_GATE_DUPLICATES:-0}" = "1" ]; then
        dup=" and not gate_duplicate"
        alldup="not gate_duplicate"
    fi
    if python3 -c 'import xdist' >/dev/null 2>&1; then
        if [ "${phase}" != "writes" ]; then
            # Beside other overlapped stages (ALP_GATE_OVERLAP, set by the pool) take
            # at most min(8, nproc/2) workers instead of every core.
            local nworkers=auto ncpu
            if [ "${ALP_GATE_OVERLAP:-0}" = "1" ]; then
                ncpu="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
                nworkers=$((ncpu / 2))
                [ "${nworkers}" -gt 8 ] && nworkers=8
                [ "${nworkers}" -lt 1 ] && nworkers=1
            fi
            python3 -m pytest tests/scripts/ -q -n "${nworkers}" -m "not repo_writes${dup}" || return 1
        fi
        if [ "${phase}" != "parallel" ]; then
            python3 -m pytest tests/scripts/ -q -m "repo_writes${dup}" || return 1
        fi
    else
        echo "stage_pytest_scripts: pytest-xdist not importable; running serially (pip install -e \".[dev]\" to parallelise)."
        case "${phase}" in
            all)      python3 -m pytest tests/scripts/ -q ${alldup:+-m "${alldup}"} || return 1 ;;
            parallel) python3 -m pytest tests/scripts/ -q -m "not repo_writes${dup}" || return 1 ;;
            writes)   python3 -m pytest tests/scripts/ -q -m "repo_writes${dup}" || return 1 ;;
        esac
    fi
    [ "${phase}" = "writes" ] && return 0

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

# The two halves the overlapped run schedules separately (see finish_overlap).
stage_pytest_parallel() { stage_pytest_scripts parallel; }
stage_pytest_repo_writes() { stage_pytest_scripts writes; }

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
    # Most of REQUIRED_GATE_SCRIPTS validate a schema, so an inadequate
    # jsonschema takes the stage down mid-loop (check_bootstrap_manifest.py
    # and check_build_plan.py were the first two to hit it) -- after the
    # earlier scripts have already printed OK, which makes the FAIL read
    # like a real gate break rather than a host gap.  Gate the whole
    # stage: a partial pass here is not a meaningful verdict either.
    require_jsonschema_2020 stage_required_gate_scripts || return 99
    local script path failed=0 ran=0 jobdir rc
    # Run the scripts in parallel (read-only checks; ALP_GATE_JOBS, default
    # 8), then print each one's output in list order so the log reads the
    # same as the old serial loop.  ~214 s -> ~55 s on alplab-gw.
    # check_public_private.py is skipped here: stage_public_private runs it.
    jobdir=$(mktemp -d)
    for script in "${REQUIRED_GATE_SCRIPTS[@]}"; do
        [ "${script}" = "check_public_private.py" ] && continue
        [ -f "scripts/${script}" ] && printf '%s
' "${script}"
    done | xargs -P "${ALP_GATE_JOBS:-8}" -I{}         sh -c 'python3 "scripts/$1" >"$2/$1.out" 2>&1; echo $? >"$2/$1.rc"' _ {} "${jobdir}"
    for script in "${REQUIRED_GATE_SCRIPTS[@]}"; do
        [ "${script}" = "check_public_private.py" ] && continue
        path="scripts/${script}"
        if [ ! -f "${path}" ]; then
            continue
        fi
        ran=1
        echo "--- ${path} ---"
        cat "${jobdir}/${script}.out" 2>/dev/null
        rc=$(cat "${jobdir}/${script}.rc" 2>/dev/null || echo 1)
        [ "${rc}" = "0" ] || failed=1
        # These two gates' pytest twins (gate_duplicate, deselected from this
        # run's pytest stage) also asserted the success line, which guards a
        # run that exits 0 without checking anything.  Keep that guard here.
        case "${script}" in
            check_emit_snapshots.py|check_zephyr_conf_parity.py)
                if [ "${rc}" = "0" ] && ! grep -q 'byte-identical' "${jobdir}/${script}.out" 2>/dev/null; then
                    echo "${script}: exited 0 but printed no 'byte-identical' success line -- a vacuous run"
                    failed=1
                fi
                ;;
        esac
    done
    rm -rf "${jobdir}"

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
    # The GD32 wire vectors moved with the firmware (ADR 0031).  Their
    # regeneration check now runs in alplabai/gd32-bridge-firmware's own CI,
    # against the same generator -- see that repo's .github/workflows/ci.yml.
    # Nothing to do here; the host-side consumers of those vectors are still
    # covered by tests/zephyr/chips/gd32g553/.

    if [ "${ran}" -eq 0 ]; then
        return 99
    fi
    return "${failed}"
}

# required-gate-scripts graded changelog citations against the WORKING
# TREE. CI grades the PR merge commit instead (actions/checkout on a
# pull_request event checks out refs/pull/N/merge), where a citation into a
# file dev has since moved might be wrong. This grades that merge, built in
# the object store from origin/dev and HEAD -- committed work only
# (alp-sdk#2186). origin/dev is passed explicitly rather than inherited:
# DIFF_BASE is shared with the clang-format stage, and a base that is
# already an ancestor of HEAD would grade HEAD, not a merge. The
# orchestration below probes for git >= 2.38 (merge-tree --write-tree) and
# for origin/dev first, and reports either as a [GAP] SKIP.
#
# NOT run with --strict-lines: that flag is the RELEASE-time hook
# (alp-sdk#2350 round 2, wired into scripts/bump_version.py), not a PR-time
# one -- it exists to turn a PRE-EXISTING citation's drift into an error
# right before it freezes into history, and running it here would redden
# every ordinary PR merged with a `dev` that moved underneath ANY
# pre-existing citation, which is precisely the treadmill alp-sdk#2350
# closed. A citation this PR itself ADDS still gets no drift tolerance
# either way (see `_check_one`'s `added` handling) -- --strict-lines adds
# nothing for that case that the plain run does not already enforce.
stage_changelog_citations_merge() {
    DIFF_BASE=origin/dev python3 scripts/check_changelog_citations.py --against-merge
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
    # Force the CWD doxygen actually inherits, in a subshell so it can't
    # leak: every relative path in the Doxyfile (INPUT, and thus the
    # relative markdown \ref links docs/**/*.md make to files like
    # vendors/*/README.md) resolves against doxygen's process CWD, not
    # against REPO_ROOT or the linking doc's own directory.  This
    # function's own `[ -f docs/doxygen/Doxyfile ]` check above passing
    # only proves the CWD was REPO_ROOT-relative at THAT point -- a
    # concurrent gw queue slot has been seen to leave the shell's CWD one
    # level off by the time this runs (alp-sdk#2473: 0 warnings on a
    # fresh clone of the identical commit, `unable to resolve reference`
    # in the shared slot).  Re-pinning here removes the dependency on
    # whatever left the CWD wherever it was, instead of chasing that
    # state leak.
    ( cd "${REPO_ROOT}" && {
        cat docs/doxygen/Doxyfile
        printf 'OUTPUT_DIRECTORY = %s\n' "${out_dir}"
        printf 'WARN_LOGFILE = %s\n' "${warn_log}"
        printf 'PROJECT_NUMBER = "%s"\n' "${project_number}"
    } | "${dox}" - >/dev/null 2>&1 ) || true
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
    # gen_pinmux_capability.py validates its own output against
    # metadata/schemas/pinmux-capability-v1.schema.json, so it needs the
    # 2020-12 validator too.  The per-generator rc==99 path below cannot
    # help here: the generator dies with an AttributeError (rc 1), not a
    # refusal, so the stage would report a generator DEFECT for a host
    # gap.  Gate at stage entry -- a drift check that regenerated only
    # some of its artifacts is not a drift check.
    require_jsonschema_2020 stage_generated_files || return 99
    local gens=(gen_soc_caps gen_status_strings gen_board_header
                gen_cc3501e_gpio_routes gen_power_tree
                gen_pinmux_capability gen_support_matrix
                gen_portability_matrix gen_catalog gen_error_catalog
                gen_verification_status gen_chip_driver_classification)
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
    # gen_npu_ops.py is deliberately NOT in the `gens` array above (see its
    # own module docstring): that array always REGENERATES, and doing that
    # here unconditionally would make the heavy, optional `vela` toolchain a
    # gate dependency for every contributor. `--check` sidesteps that: it
    # exits 2 -- not 1 -- the instant `vela` isn't found on PATH, before
    # anything toolchain-heavy runs, so this call is safe and cheap to make
    # unconditionally. rc 1 is a real defect (stale tables, or a report-format
    # /delta mismatch) and DOES fail the stage, same as any other generator.
    #
    # rc 2 is a SKIP -- but ONLY when the change under test cannot have
    # invalidated the tables. Unconditionally, it was the proxy-check shape
    # this repo keeps digging out: `vela` is on almost no contributor's PATH,
    # so the default local invocation printed "SKIPPED" and never checked the
    # NPU op tables AT ALL -- including on a diff that edited nothing but
    # metadata/npu_ops/ and scripts/gen_npu_ops.py, i.e. exactly the tooling
    # whose freshness this call is the only local proof of. A gate that
    # cannot run on the one change it exists for is decoration. So: if the
    # diff touches those paths and vela is missing, the stage FAILS and says
    # what to install; otherwise it still skips, loudly.
    if [ -f scripts/gen_npu_ops.py ]; then
        local gen_npu_ops_out npu_ops_touched npu_ops_base
        # DIFF_BASE overrides; same default as the clang-format stage above,
        # merge-base(origin/dev, HEAD) rather than HEAD~1: a
        # multi-commit branch (the normal case for a metadata change like
        # this) has npu_ops-touching commits older than HEAD~1, and HEAD~1
        # would miss them entirely.  If `origin/dev` is unreachable (no such
        # remote-tracking ref locally), `git merge-base` fails and this falls
        # back to HEAD~1.  Two sources, because either alone has a blind
        # spot: `git diff <base>` cannot see uncommitted work (which is what
        # a local run usually IS), and the working tree cannot see what an
        # earlier commit on the branch already changed.
        npu_ops_base="${DIFF_BASE:-$(git merge-base origin/dev HEAD 2>/dev/null || echo HEAD~1)}"
        npu_ops_touched=""
        if git rev-parse "${npu_ops_base}" >/dev/null 2>&1; then
            npu_ops_touched=$(git diff --name-only "${npu_ops_base}" -- \
                metadata/npu_ops scripts/gen_npu_ops.py 2>/dev/null)
        fi
        # `--porcelain` v1 is "XY <path>"; strip the status columns so both
        # probes yield bare paths and `sort -u` can merge them.
        npu_ops_touched="${npu_ops_touched}
$(git status --porcelain -- metadata/npu_ops scripts/gen_npu_ops.py 2>/dev/null | cut -c4-)"
        # Collapse to nothing when both probes came back empty.
        npu_ops_touched=$(printf '%s\n' "${npu_ops_touched}" \
            | sed '/^[[:space:]]*$/d' | sort -u)

        gen_npu_ops_out=$(python3 scripts/gen_npu_ops.py --check 2>&1)
        rc=$?
        if [ "${rc}" -eq 2 ] && [ -n "${npu_ops_touched}" ]; then
            echo "${gen_npu_ops_out}"
            echo "generated-files: gen_npu_ops.py --check could NOT run (vela not"
            echo "on PATH) -- but this change touches the very files it checks:"
            printf '%s\n' "${npu_ops_touched}" | sed 's/^/    /'
            echo "Skipping here would leave the NPU op tables unverified by the"
            echo "one gate that verifies them.  Install ethos-u-vela into a venv"
            echo "and re-run with it on PATH, e.g."
            echo "    PATH=<venv>/bin:\$PATH bash scripts/test-all.sh --target dev"
            return 1
        elif [ "${rc}" -eq 2 ]; then
            echo "generated-files: gen_npu_ops.py --check SKIPPED (vela not on PATH; this change touches no metadata/npu_ops/ or scripts/gen_npu_ops.py file)"
        elif [ "${rc}" -ne 0 ]; then
            echo "${gen_npu_ops_out}"
            echo "scripts/gen_npu_ops.py --check failed"
            return 1
        fi
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
        docs/chip-driver-classification.md \
        examples/aen \
        src/backends/gpio/cc3501e_rev_dependent_pins.c \
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
            docs/chip-driver-classification.md \
            examples/aen \
            src/backends/gpio/cc3501e_rev_dependent_pins.c \
            docs/diagnostics 2>/dev/null; then
        echo "generated files are OUT OF SYNC -- regenerated in place; git add + commit:"
        git --no-pager diff --stat -- \
            include/alp docs/abi src/cap.c src/status_strings.c \
            metadata/catalog.json metadata/error-catalog.json metadata/pinmux \
            metadata/socs/renesas/rzv2n/n44.json \
            docs/portability-matrix.md docs/peripheral-support-matrix.md \
            docs/verification-status.md \
            docs/chip-driver-classification.md \
            examples/aen \
            src/backends/gpio/cc3501e_rev_dependent_pins.c \
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
pin_alp_zephyr_module

if [ "${ZEPHYR_ONLY}" -eq 1 ]; then
    # Run the suite exactly once.  run_stage() already turns a 99
    # return (ZEPHYR_BASE unset) into SKIP, so no pre-check/case
    # dance -- and no second, output-hiding invocation -- is needed.
    run_stage "twister" stage_twister
else
    # This branch schedules the public-private and required-gate-scripts
    # stages, so the pytest stage deselects the tests that only re-run them.
    # If either of those stages cannot run (a 99), it surfaces as a [GAP]
    # (exit 2) -- the run is then flagged incomplete anyway, so the missing
    # duplicate is never a silent loss.
    SKIP_GATE_DUPLICATES=1

    # The full plain-CMake builds are release-grade -- the fast `dev`
    # profile skips them (dev PRs iterate on twister + the cheap gates).
    if [ "${TARGET}" = "dev" ]; then
        skip_stage "yocto-build-and-ctest" "--target dev (release-grade build)" scope
        skip_stage "baremetal-build"       "--target dev (release-grade build)" scope
    else
        run_stage "yocto-build-and-ctest" stage_yocto_build_and_ctest
        run_stage "baremetal-build"       stage_baremetal_build
    fi

    if [ "${YOCTO_ONLY}" -eq 0 ]; then
        if [ "${QUICK}" -eq 1 ]; then
            launch_skip "twister" "--quick" scope
        else
            # Local runs never build the full ~270-config native_sim set --
            # CI's sharded pr-twister does that in the merge queue.
            # select_checks.py --local answers `skip` (no native_sim input
            # changed), or `smoke` followed by the suite directories to build
            # (the changed suites + SMOKE_SUITES).  It printed its per-path
            # proof on stderr above.  --full and --target main bypass it and
            # run everything; an empty answer (the script crashed) also falls
            # through to the full run -- a bug must not shrink coverage.
            twister_plan=""
            if [ "${TARGET}" != "main" ] && [ "${FORCE_FULL}" -eq 0 ]; then
                twister_plan="$(python3 scripts/select_checks.py --base "${SELECT_BASE}" --worktree --local)"
            fi
            if [ "${twister_plan%%$'\n'*}" = "skip" ]; then
                launch_skip "twister" "no native_sim build input changed (select_checks.py; --full forces it)" scope
            elif [ -z "${ZEPHYR_BASE:-}" ]; then
                launch_skip "twister" "ZEPHYR_BASE not set (run scripts/bootstrap.sh first)" gap
            else
                if [ "${twister_plan%%$'\n'*}" = "smoke" ]; then
                    TWISTER_SMOKE=1
                    while IFS= read -r _suite; do
                        [ -n "${_suite}" ] && TWISTER_SUITES+=("${_suite}")
                    done <<< "${twister_plan#*$'\n'}"
                fi
                launch_bg "twister" stage_twister
            fi
        fi
    fi

    if command -v clang-format >/dev/null 2>&1; then
        launch "clang-format-diff" stage_clang_format
    else
        launch_skip "clang-format-diff" "clang-format not installed" gap
    fi

    # Shell-script static lint -- catches shell bugs (cd-without-guard,
    # set-u empty-array traps, POSIX slips) on Linux before they redden
    # macOS/Windows CI.  Skips cleanly if shellcheck isn't installed.
    if command -v shellcheck >/dev/null 2>&1 || [ -x "${HOME}/.local/bin/shellcheck" ]; then
        launch "shellcheck" stage_shellcheck
    else
        launch_skip "shellcheck" "shellcheck not installed (PATH or ~/.local/bin)" gap
    fi

    # bash 3.2 parse gate -- catches the class of defect PR #1050 hit
    # that shellcheck (above) and `bash -n` on a modern bash both miss
    # (see stage_bash32_parse for the apostrophe/heredoc mechanism).
    # Skips cleanly if neither podman nor docker is on PATH -- CI's
    # macOS leg (cross-platform-zephyr.yml python-smoke) still runs
    # this unconditionally with real bash 3.2.57, no container needed
    # there.
    if command -v podman >/dev/null 2>&1 || command -v docker >/dev/null 2>&1; then
        launch "bash32-parse" stage_bash32_parse
    else
        launch_skip "bash32-parse" "neither podman nor docker on PATH -- CI's macos-latest python-smoke leg runs this unconditionally with real bash 3.2.57" gap
    fi

    launch "metadata-validate" stage_metadata_validate

    # Documentation lint -- cheap, always runnable, no special tooling.
    if [ -f scripts/lint_doc_yaml_fragments.py ]; then
        launch "doc-yaml-fragments" stage_doc_yaml_fragments
    else
        launch_skip "doc-yaml-fragments" "scripts/lint_doc_yaml_fragments.py missing" gap
    fi

    if [ -f scripts/check_public_private.py ]; then
        launch "public-private" stage_public_private
    else
        launch_skip "public-private" "scripts/check_public_private.py missing" gap
    fi

    # Mirrors cross-platform-zephyr.yml's python-smoke --fail-on-warning
    # step (alp-sdk#1032 A5) so a repo drifting back to N findings is
    # caught locally, not only on three legs of a non-required workflow.
    if [ -f scripts/check_cross_platform.py ]; then
        launch "cross-platform-lint" stage_cross_platform_lint
    else
        launch_skip "cross-platform-lint" "scripts/check_cross_platform.py missing" gap
    fi

    # Required scripts/check_*.py gates -- see REQUIRED_GATE_SCRIPTS
    # above.  Keeps this wrapper's coverage aligned with the hard
    # gates pr-metadata-validate.yml / pr-doc-drift.yml run in CI.
    launch "required-gate-scripts" stage_required_gate_scripts

    # The same citation gate, graded against the merge CI will check out.
    # Both probes mirror what stage_changelog_citations_merge needs, so a
    # host that cannot build the merge gets a named [GAP], not a FAIL.
    if ! command -v python3 >/dev/null 2>&1 || [ ! -f scripts/check_changelog_citations.py ]; then
        launch_skip "changelog-citations-merge" "python3 or scripts/check_changelog_citations.py missing" gap
    elif ! git merge-tree --write-tree HEAD HEAD >/dev/null 2>&1; then
        launch_skip "changelog-citations-merge" "$(git --version) has no merge-tree --write-tree (needs git >= 2.38)" gap
    elif ! git rev-parse -q --verify "origin/dev^{commit}" >/dev/null; then
        launch_skip "changelog-citations-merge" "origin/dev not present here (git fetch origin dev)" gap
    else
        launch "changelog-citations-merge" stage_changelog_citations_merge
    fi

    # `check · generated files in sync` -- regenerate every single-sourced
    # artifact + fail on drift.  Catches the class of red that bit #623 /
    # #636 / #642 (new macro/symbol/gate without a committed regen).
    writer_stage "generated-files" stage_generated_files

    # alp.lock --check -- both the dev (fast) and main (release-grade)
    # profiles run this unconditionally, same as metadata-validate above.
    # Placed after generated-files for narrative order only: `--check` no
    # longer diffs against a committed lock (#1576), it schema-validates a
    # freshly generated one, so this stage's verdict does not depend on
    # running before or after generated-files regenerates its inputs.
    writer_stage "alp-lock" stage_alp_lock

    # Main-only: the strict ABI-snapshot diff gate that pr-abi-snapshot.yml
    # runs on `main` + `release/**` only.  The `--target main` release-grade
    # profile adds it; dev/full skip it (generated-files already regenerates
    # the snapshot, but the strict diff-vs-committed is a main-branch gate).
    if [ "${TARGET}" = "main" ]; then
        writer_stage "abi-strict" stage_abi_strict
    fi

    # Pytest -- subsumes metadata-validate's unittest coverage and adds
    # the linter + regression locks for a3cd4fd / e3a4c6b.
    if command -v python3 >/dev/null 2>&1 && [ -d tests/scripts ]; then
        if [ "${OVERLAP}" -eq 1 ]; then
            # Parallel sweep in the pool; the modules that write the
            # checkout (-m repo_writes) run after it, with the other writers.
            launch "pytest-scripts" stage_pytest_parallel
            writer_stage "pytest-repo-writes" stage_pytest_repo_writes
        else
            launch "pytest-scripts" stage_pytest_scripts
        fi
    else
        launch_skip "pytest-scripts" "tests/scripts missing or no python3" gap
    fi

    # HiL spec validation -- host-side parse + board-target check
    # for every smoke spec under tests/hil/.  No hardware required.
    if [ -f tests/hil/run_smoke.py ]; then
        launch "hil-spec-validate" stage_hil_spec_validate
    else
        launch_skip "hil-spec-validate" "tests/hil/run_smoke.py missing" gap
    fi

    if [ "${QUICK}" -eq 0 ] && [ "${YOCTO_ONLY}" -eq 0 ] && [ "${TARGET}" != "dev" ]; then
        # stage_doxygen generates the CI Doxyfile itself + finds doxygen on
        # PATH or in ~/doxybin, so no committed Doxyfile is needed.  The fast
        # dev profile skips it (Doxygen is one of the slow stages).
        if command -v doxygen >/dev/null 2>&1 || [ -x "${HOME}/doxybin/doxygen" ]; then
            launch "doxygen" stage_doxygen
        else
            launch_skip "doxygen" "doxygen not installed (PATH or ~/doxybin)" gap
        fi
    elif [ "${TARGET}" = "dev" ]; then
        launch_skip "doxygen" "--target dev (slow release-grade stage)" scope
    fi

    # Overlapped runs: join twister, print the captured stages in order, then
    # run the tree-writing stages.  No-op under ALP_GATE_SERIAL=1.
    finish_overlap
fi

END=$(date +%s)

# -------- Summary -------------------------------------------------------------

echo
echo "===== SUMMARY ($((END - START))s) ====="
fail_count=0
gap_count=0
for i in "${!STAGE_NAMES[@]}"; do
    tag=""
    if [ "${STAGE_STATUS[$i]}" = "SKIP" ] && [ "${STAGE_KIND[$i]}" = "gap" ]; then
        tag="[GAP] "
        gap_count=$((gap_count + 1))
    fi
    printf "  %-28s %s %s%s (%ss)\n" "${STAGE_NAMES[$i]}" "${STAGE_STATUS[$i]}" "${tag}" "${STAGE_NOTES[$i]}" "${STAGE_SECS[$i]}"
    [ "${STAGE_STATUS[$i]}" = "FAIL" ] && fail_count=$((fail_count + 1))
done

if [ "${TWISTER_SMOKE:-0}" -eq 1 ]; then
    echo
    echo "NOTE: twister ran the local SMOKE subset only (changed suites + a fixed smoke set)."
    echo "      The full native_sim set runs in CI's sharded pr-twister; --full runs it here."
fi

if [ "${fail_count}" -gt 0 ]; then
    echo
    echo "${fail_count} stage(s) failed.  See per-stage output above."
    exit 1
fi

# A [GAP] stage is one that TRIED to run and could not, for a missing
# tool / env var / importable module -- as opposed to a plain SKIP that
# --quick / --target deliberately scoped out (STAGE_KIND=scope, printed
# with no [GAP] tag). Zero failures does NOT mean this run measured
# what it claims to: alp-sdk#1396 was exactly this shape --
# `ZEPHYR_BASE` unset made twister SKIP, nothing failed, and the run
# still printed "All runnable stages passed." and exited 0, which reads
# as "the tree is fully verified" when the one stage that could have
# caught a build break never ran. A distinct exit code (2, not 0 or 1)
# means a caller that only checks $? for zero can't mistake this for a
# clean run either.
if [ "${gap_count}" -gt 0 ]; then
    echo
    echo "${gap_count} stage(s) SKIPPED for a missing prerequisite (marked [GAP] above) -- this run did NOT exercise everything --target ${TARGET} requires."
    echo "Install/configure what's missing (see docs/testing.md, docs/local-ci.md) and re-run before trusting this as a complete local gate."
    exit 2
fi
echo
echo "All runnable stages passed.  Real-hardware coverage is parked"
echo "behind HIL per docs/test-plan.md."
exit 0
