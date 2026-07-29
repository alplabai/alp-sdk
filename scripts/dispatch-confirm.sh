#!/usr/bin/env bash
# scripts/dispatch-confirm.sh
#
# Cross-platform scope: runs as a CI step only on ubuntu-latest (this
# repo's runner), so bash-only is fine there. It is ALSO exec'd directly by
# tests/scripts/test_dispatch_confirm.py under pytest, which cross-platform-zephyr.yml
# runs on macos-latest too -- see the portable ISO-8601 date parsing below,
# which exists specifically because BSD/macOS `date` has no GNU `-d`.
#
# Confirms that the repository_dispatch dispatch-tan-parity.yml just fired at
# tan-cli actually started a run there, and tells the two failure shapes
# apart instead of collapsing them into one "warn forever" step (that
# collapse is exactly how #194 went unnoticed for months).
#
# Extracted out of the workflow so it's runnable and testable standalone --
# stub `gh` on PATH and see tests/scripts/test_dispatch_confirm.py for both a
# failing and a passing transcript.
#
# Contract with the caller (dispatch-tan-parity.yml):
#   - GH_TOKEN is already exported, scoped to tan-cli.
#   - DISPATCH_EPOCH is the unix time captured by the CALLER IMMEDIATELY
#     BEFORE the `gh api .../dispatches` call runs (not after, and not in
#     this step) -- capturing it later leaves a 1-3s gap in which a fast,
#     healthy dispatch's run can be created before DISPATCH_EPOCH and the
#     strict `>` comparison below then never sees it. Anchoring to that
#     fixed moment -- instead of re-deriving "now - 300s" on every poll
#     iteration, which is what the previous inline version did -- also
#     means a run that starts after the poll loop gives up doesn't
#     retroactively make the window wrong; the window is fixed at dispatch
#     time, not sliding with however long we've polled.
#   - PREV_DISPATCH_EPOCH (optional) is the created_at, as a unix epoch, of
#     THIS workflow's own previous run (any status) on alp-sdk -- i.e.
#     roughly when we last dispatched to tan-cli before this push. It is
#     the self-calibrated staleness baseline: the seam is judged broken
#     only if tan-cli has produced NO repository_dispatch run at all since
#     that previous dispatch, not if a run is merely older than some
#     wall-clock constant. That makes the check immune to ordinary quiet
#     periods by construction -- if alp-sdk itself went N hours without
#     touching the contract surface, no tan-cli run is expected in those N
#     hours either. If unset (this workflow's first-ever run, or the
#     caller's self-query failed), the script falls back to
#     DISPATCH_CONFIRM_STALE_THRESHOLD_S -- see that constant's own
#     comment for the measured evidence behind its default.
#
# Exit 0: a new run for this dispatch appeared, OR the failure to see one
#         is indistinguishable from ordinary Actions queueing (a query
#         error, or tan-cli's wiring fired since our own last dispatch,
#         so this is very likely a timing miss, not a break).
# Exit 1: the seam looks broken -- tan-cli has NEVER had a
#         repository_dispatch run, OR gh tooling/auth is broken on THIS
#         side, OR (self-calibrated) tan-cli produced no run at all since
#         the last time this workflow itself dispatched, OR (bootstrap
#         fallback only) its most recent run predates this push by more
#         than DISPATCH_CONFIRM_STALE_THRESHOLD_S. All are conditions that
#         resolve with real data, not a fallback value standing in for a
#         failed API call.
set -uo pipefail

: "${DISPATCH_EPOCH:?DISPATCH_EPOCH must be set to the unix time the dispatch call returned (date +%s)}"

# ~5 minutes of polling by default. The previous ~60s window was too tight
# (that's part of what made #194's era noisy) and the ~600s window this
# script shipped with initially collided badly with the workflow's
# `concurrency: cancel-in-progress: true`: 99 of 369 measured
# path-triggering push intervals over 60 days landed under 600s, so a
# meaningful share of confirm steps were getting cancelled mid-poll
# (showing as *cancelled*, not failed -- no verdict at all) rather than
# running to a real verdict. Halving the window trades a slightly higher
# chance of landing on the WARN branch (ordinary queueing, still exit 0)
# for roughly halving that cancellation exposure -- an easy trade now that
# the self-calibrated staleness check below no longer depends on a long
# poll to avoid false reds.
POLL_ATTEMPTS="${DISPATCH_CONFIRM_POLL_ATTEMPTS:-30}"
POLL_INTERVAL_S="${DISPATCH_CONFIRM_POLL_INTERVAL_S:-10}"
# Bootstrap-only fallback threshold, used ONLY when PREV_DISPATCH_EPOCH is
# unavailable (see the header). NOT the primary staleness check anymore --
# a flat wall-clock constant was tried first and falsified: the workflow's
# own paths-filtered push trigger measurably goes quiet for well over a
# day. Over the 60 days preceding 2026-07-27, it crossed 24h thirteen
# times, with a MAXIMUM observed gap of 110.8h (the gap ending 2026-06-24).
# This default (240h / 10 days) sits more than 2x above that measured
# maximum specifically so this fallback path can't reproduce the original
# false-red.
STALE_THRESHOLD_S="${DISPATCH_CONFIRM_STALE_THRESHOLD_S:-864000}"

if ! command -v gh >/dev/null 2>&1; then
    echo "::error::gh CLI is not on PATH -- this is a misconfiguration of THIS job/runner, not evidence about tan-cli. Fix the workflow's gh setup before trusting this gate's verdict again."
    exit 1
fi

# One cheap probe call before the poll loop starts: distinguishes "gh is
# broken or unauthenticated" (THIS repo's problem -- FAIL) from "tan-cli
# genuinely has no matching run yet" (keep polling). Without this, a dead
# GH_TOKEN (the App token mint silently failed, or the App's access to
# tan-cli was revoked) burns the entire poll window and then reads
# identically to a real no-runs result at the summary query further down --
# the same "guard wearing a guard's name" shape this whole change exists to
# remove, just moved one level down.
if ! gh api "repos/alplabai/tan-cli/actions/runs?per_page=1" >/dev/null 2>&1; then
    echo "::error::gh api probe call to tan-cli failed before polling even started -- GH_TOKEN is missing, expired, or unauthorized. This is THIS repo's tooling/auth problem (check the App token mint step), not a signal about tan-cli, so it fails outright instead of being reported as a warning about tan-cli."
    exit 1
fi

attempt=1
while [ "${attempt}" -le "${POLL_ATTEMPTS}" ]; do
    sleep "${POLL_INTERVAL_S}"
    count="$(gh api "repos/alplabai/tan-cli/actions/runs?event=repository_dispatch&per_page=5" \
        -q "[.workflow_runs[] | select((.created_at | fromdateiso8601) > ${DISPATCH_EPOCH})] | length" 2>/dev/null)"
    # No fallback-to-0 here on purpose: a failed gh api call leaves
    # `count` empty, which just means "keep polling" -- it must NOT be
    # read as "definitely zero runs", or a transient network blip during
    # the loop would count against tan-cli.
    if [ -n "${count}" ] && [ "${count}" -gt 0 ] 2>/dev/null; then
        echo "::notice::tan-cli started ${count} repository_dispatch run(s) for this dispatch"
        exit 0
    fi
    attempt=$((attempt + 1))
done

# No new run showed up within the poll window. Several DIFFERENT failures
# share this code path, and they must NOT share a verdict:
#
#   * tan-cli has NEVER had a repository_dispatch run (lifetime total_count
#     == 0) -> the wiring is dead, not slow. Unambiguous, FAIL.
#   * (self-calibrated) tan-cli's most recent repository_dispatch run
#     predates our OWN previous dispatch (PREV_DISPATCH_EPOCH) -> the seam
#     produced nothing across a whole inter-dispatch period. FAIL.
#   * (self-calibrated) tan-cli's most recent run is newer than
#     PREV_DISPATCH_EPOCH but this dispatch's poll still didn't see a new
#     one -> the wiring fired since last time, so this is almost certainly
#     Actions queueing for THIS push specifically. WARN.
#   * (bootstrap fallback, no PREV_DISPATCH_EPOCH) tan-cli's most recent
#     run is older than STALE_THRESHOLD_S -> FAIL. Newer -> WARN.
#
# A `gh api` failure at this point is a query problem, not evidence about
# tan-cli, so it warns rather than borrowing the fail path meant for a
# confirmed-broken seam.
summary="$(gh api "repos/alplabai/tan-cli/actions/runs?event=repository_dispatch&per_page=1" 2>/dev/null)"
if [ -z "${summary}" ]; then
    echo "::warning::could not query tan-cli's repository_dispatch history (gh api call failed) -- can't tell queueing from a broken seam from here, so not failing the push over it. Check https://github.com/alplabai/tan-cli/actions?query=event%3Arepository_dispatch manually."
    exit 0
fi

lifetime="$(echo "${summary}" | jq -r '.total_count // empty')"
if [ -z "${lifetime}" ] || [ "${lifetime}" = "0" ]; then
    echo "::error::tan-cli has NEVER had a repository_dispatch run (lifetime total_count=${lifetime:-0}). The dispatch is accepted with a 204 and goes nowhere, so the cross-repo parity gate is not running at all."
    echo "::error::Most likely cause, and the one that caused #194: parity.yml's 'repository_dispatch:' trigger is missing from tan-cli's DEFAULT branch. GitHub triggers repository_dispatch ONLY from the default branch's copy of a workflow -- having it on 'dev' alone does nothing. Check https://github.com/alplabai/tan-cli/blob/main/.github/workflows/parity.yml"
    echo "::error::Second possibility: the event_type no longer matches. This repo posts 'alp-sdk-planner-change'; tan's parity.yml must list that exact string under repository_dispatch.types."
    exit 1
fi

last_created="$(echo "${summary}" | jq -r '.workflow_runs[0].created_at // empty')"
if [ -z "${last_created}" ]; then
    echo "::warning::tan-cli has ${lifetime} lifetime repository_dispatch run(s), but the most recent one's timestamp couldn't be read -- treating as queueing, not failing."
    exit 0
fi

# Portable ISO-8601 -> epoch parse. GNU `date -d` accepts near-arbitrary,
# not just ISO-8601, input (verified: `date -d "01/02/2020 3pm"` parses
# fine) so validate the shape FIRST -- otherwise a garbage timestamp lands
# on the FAIL branch below instead of the intended "couldn't be read" warn.
# Then try GNU `date -d` (Linux runners), then the BSD/macOS `date -j -f`
# form (macos-latest, which cross-platform-zephyr.yml runs this test suite
# on, has no GNU coreutils and treats `-d` as an unrelated DST flag).
last_epoch=""
if printf '%s' "${last_created}" | grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$'; then
    if parsed="$(date -u -d "${last_created}" +%s 2>/dev/null)"; then
        last_epoch="${parsed}"
    elif parsed="$(date -u -j -f '%Y-%m-%dT%H:%M:%SZ' "${last_created}" +%s 2>/dev/null)"; then
        last_epoch="${parsed}"
    fi
fi
if [ -z "${last_epoch}" ]; then
    echo "::warning::tan-cli has ${lifetime} lifetime repository_dispatch run(s), but the most recent one's timestamp (\"${last_created}\") couldn't be parsed on this platform -- treating as queueing, not failing."
    exit 0
fi

if [ -n "${PREV_DISPATCH_EPOCH:-}" ]; then
    if [ "${last_epoch}" -gt "${PREV_DISPATCH_EPOCH}" ]; then
        echo "::warning::dispatched, but no NEW tan-cli repository_dispatch run appeared within ~$((POLL_ATTEMPTS * POLL_INTERVAL_S))s (lifetime runs: ${lifetime}, most recent: ${last_created}). That run IS newer than alp-sdk's own previous dispatch though, so the seam has fired since last time -- most likely Actions queueing for THIS push. Confirm a run for this SHA appeared at https://github.com/alplabai/tan-cli/actions?query=event%3Arepository_dispatch"
        exit 0
    fi
    echo "::error::tan-cli's most recent repository_dispatch run was at ${last_created}, which is NO NEWER than alp-sdk's own previous dispatch-tan-parity.yml run (baseline epoch ${PREV_DISPATCH_EPOCH}). tan-cli has produced no run at all across the whole period since the last time this workflow dispatched -- that is real evidence of a broken seam, not an ordinary timing miss (lifetime runs: ${lifetime})."
    echo "::error::Check https://github.com/alplabai/tan-cli/blob/main/.github/workflows/parity.yml still lists 'alp-sdk-planner-change' under repository_dispatch.types on tan-cli's DEFAULT branch, and that this repo's event_type ('alp-sdk-planner-change') hasn't drifted from it."
    exit 1
fi

# Bootstrap fallback only: no self-calibration baseline was available.
age=$((DISPATCH_EPOCH - last_epoch))
if [ "${age}" -gt "${STALE_THRESHOLD_S}" ]; then
    echo "::error::tan-cli's most recent repository_dispatch run was at ${last_created}, more than $((STALE_THRESHOLD_S / 3600))h before this push (lifetime runs: ${lifetime}), and no self-calibration baseline (PREV_DISPATCH_EPOCH) was available to compare against instead. That gap exceeds even a generous multiple of alp-sdk's own measured quiet-period maximum -- treat this as the wiring having broken again (the #194 failure mode), not a timing miss."
    echo "::error::Check https://github.com/alplabai/tan-cli/blob/main/.github/workflows/parity.yml still lists 'alp-sdk-planner-change' under repository_dispatch.types on tan-cli's DEFAULT branch, and that this repo's event_type ('alp-sdk-planner-change') hasn't drifted from it."
    exit 1
fi

echo "::warning::dispatched, but no NEW tan-cli repository_dispatch run appeared within ~$((POLL_ATTEMPTS * POLL_INTERVAL_S))s (lifetime runs: ${lifetime}, most recent: ${last_created}). No self-calibration baseline was available; falling back to the wall-clock threshold, which this run is within -- most likely Actions queueing. Confirm a run for this SHA appeared at https://github.com/alplabai/tan-cli/actions?query=event%3Arepository_dispatch"
exit 0
