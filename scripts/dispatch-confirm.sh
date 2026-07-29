#!/usr/bin/env bash
# scripts/dispatch-confirm.sh
#
# Confirms that the repository_dispatch dispatch-tan-parity.yml just fired at
# tan-cli actually started a run there, and tells the two failure shapes
# apart instead of collapsing them into one "warn forever" step (that
# collapse is exactly how #194 went unnoticed for months).
#
# Extracted out of the workflow so it's runnable and testable standalone --
# stub `gh` on PATH and see tests/ci/test_dispatch_confirm.sh for both a
# failing and a passing transcript.
#
# Contract with the caller (dispatch-tan-parity.yml):
#   - GH_TOKEN is already exported, scoped to tan-cli.
#   - DISPATCH_EPOCH is the unix time the dispatch call returned, captured
#     by the CALLER before this script starts polling. Anchoring the "did a
#     new run appear" check to that fixed moment -- instead of re-deriving
#     "now - 300s" on every poll iteration, which is what the previous
#     inline version did -- means a run that starts after the poll loop
#     gives up doesn't retroactively make the window wrong; the window is
#     fixed at dispatch time, not sliding with however long we've polled.
#
# Exit 0: a new run for this dispatch appeared, OR the failure to see one
#         is indistinguishable from ordinary Actions queueing (a query
#         error, or tan-cli's wiring fired recently enough that this is
#         very likely a timing miss, not a break).
# Exit 1: the seam looks broken -- either tan-cli has NEVER had a
#         repository_dispatch run, or its most recent one predates this
#         push by more than DISPATCH_CONFIRM_STALE_THRESHOLD_S. Both are
#         conditions that resolve with real data, not a fallback value
#         standing in for a failed API call.
set -uo pipefail

: "${DISPATCH_EPOCH:?DISPATCH_EPOCH must be set to the unix time the dispatch call returned (date +%s)}"

# ~10 minutes of polling by default: GitHub Actions queue delays under
# normal load clear in well under a minute; sustained platform-wide
# degradation can push that to a few minutes. Ten minutes is a defensible
# ceiling for "still just queueing" before falling back to the staleness
# check below -- long enough to absorb a bad queueing day, short enough
# that this step doesn't itself become the slow part of the push.
POLL_ATTEMPTS="${DISPATCH_CONFIRM_POLL_ATTEMPTS:-60}"
POLL_INTERVAL_S="${DISPATCH_CONFIRM_POLL_INTERVAL_S:-10}"
# If tan-cli's most recent repository_dispatch run (of ANY kind) is older
# than this, "the wiring worked before" stops being a good excuse -- a
# seam that's genuinely alive gets exercised on every alp-sdk push to
# dev/main, which land far more often than once a day.
STALE_THRESHOLD_S="${DISPATCH_CONFIRM_STALE_THRESHOLD_S:-86400}"

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

# No new run showed up within the poll window. Two DIFFERENT failures share
# this code path, and they must NOT share a verdict:
#
#   * tan-cli has NEVER had a repository_dispatch run (lifetime total_count
#     == 0) -> the wiring is dead, not slow. Unambiguous, FAIL.
#   * tan-cli's most recent repository_dispatch run (any age) is RECENT
#     (within STALE_THRESHOLD_S) -> the wiring fired recently, so this is
#     almost certainly Actions queueing. WARN.
#   * tan-cli's most recent repository_dispatch run is STALE (older than
#     STALE_THRESHOLD_S) -> the wiring used to work but hasn't produced a
#     run in longer than a normal push cadence allows for. That is
#     reachable, real evidence of a broken seam -- unlike lifetime==0,
#     which stops being reachable forever the first time the seam ever
#     fires once. FAIL.
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
last_epoch="$([ -n "${last_created}" ] && date -d "${last_created}" +%s 2>/dev/null || echo "")"
if [ -z "${last_epoch}" ]; then
    echo "::warning::tan-cli has ${lifetime} lifetime repository_dispatch run(s), but the most recent one's timestamp couldn't be read -- treating as queueing, not failing."
    exit 0
fi

age=$((DISPATCH_EPOCH - last_epoch))
if [ "${age}" -gt "${STALE_THRESHOLD_S}" ]; then
    echo "::error::tan-cli's most recent repository_dispatch run was at ${last_created}, more than $((STALE_THRESHOLD_S / 3600))h before this push (lifetime runs: ${lifetime}). The seam has fired before, but not recently enough for an unstarted run this time to be ordinary queueing -- treat this as the wiring having broken again (the #194 failure mode), not a timing miss."
    echo "::error::Check https://github.com/alplabai/tan-cli/blob/main/.github/workflows/parity.yml still lists 'alp-sdk-planner-change' under repository_dispatch.types on tan-cli's DEFAULT branch, and that this repo's event_type ('alp-sdk-planner-change') hasn't drifted from it."
    exit 1
fi

echo "::warning::dispatched, but no NEW tan-cli repository_dispatch run appeared within ~$((POLL_ATTEMPTS * POLL_INTERVAL_S))s (lifetime runs: ${lifetime}, most recent: ${last_created}). The wiring fired recently, so this is most likely Actions queueing -- confirm a run for this SHA appeared at https://github.com/alplabai/tan-cli/actions?query=event%3Arepository_dispatch"
exit 0
