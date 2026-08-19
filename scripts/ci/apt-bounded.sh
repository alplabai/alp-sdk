#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run apt-get under a WALL-CLOCK bound that is SHARED ACROSS EVERY INVOCATION IN
# THE STEP, with a dpkg-safe retry.
#
# Cross-platform scope: CI-only. Bash + apt-get + dpkg, so Debian/Ubuntu
# runners by nature -- it is NOT part of the Windows / WSL / macOS
# developer-host surface. (This note must stay within the first 30 lines:
# check_cross_platform.py's _bash_helper_has_note only reads that far.)
#
# WHY A BOUND AT ALL: `Acquire::http::Timeout` bounds an IDLE read, not a SLOW
# one. Every byte that arrives resets the timer, so a mirror that trickles
# defeats it forever, and apt has no minimum-transfer-rate option (no equivalent
# of curl's --speed-limit). Measured against two local servers (#1575):
#
#   server                               result
#   -----------------------------------  -----------------------------------
#   accepts, sends headers, then silent   rc=100 after 127s -- Timeout=30 FIRED,
#                                         3 retries, apt gave up on its own
#   accepts, then 1 byte every 20s        NEVER returns; only an external kill
#                                         ended it. Unbounded.
#
# WHY THE BUDGET IS SHARED, not per-invocation: a step calls this TWICE --
# `update` then `install`. A per-invocation budget of N therefore admits 2N per
# step, which overran the step's own `timeout-minutes` and let the STEP cap kill
# the wrapper before it could report its own attributed failure. Measured on
# alp-sdk#1592 (2026-08-19, job 96013431161): update burned 3x300s, install then
# burned 2x300s, and the 20-minute step cap fired at 09:34:08 -- the exact
# unattributed outcome this design exists to prevent.
#
# So the deadline is computed ONCE per step and persisted in RUNNER_TEMP, keyed
# by GITHUB_ACTION (the step's own identifier). Every later invocation in the
# same step inherits it, and each attempt is clamped to the time actually
# remaining. Total wall time for the step is APT_STEP_BUDGET regardless of how
# many times the wrapper is called.
#
# dpkg safety: `timeout` can kill apt-get mid-unpack, leaving the database
# half-configured or the lock held. Every retry runs `dpkg --configure -a`
# first -- the standard recovery, a no-op when nothing was interrupted.
#
# Usage:  scripts/ci/apt-bounded.sh update
#         scripts/ci/apt-bounded.sh install -y --no-install-recommends foo bar
set -euo pipefail

# Total wall clock for ALL invocations in this step. Must sit comfortably UNDER
# the step's own `timeout-minutes` so this wrapper loses the race and reports a
# named failure instead of the step cap firing anonymously.
: "${APT_STEP_BUDGET:=780}"      # 13 min, vs the 20-min step cap
: "${APT_ATTEMPT_TIMEOUT:=300}"  # ceiling per attempt; clamped to what remains
: "${APT_ATTEMPTS:=3}"

_now() { date +%s; }

# One deadline per step. GITHUB_ACTION identifies the step; fall back to the PID
# of our parent shell so a local run still gets a private, non-colliding file.
_state_dir="${RUNNER_TEMP:-/tmp}"
_key="${GITHUB_ACTION:-local-$PPID}"
_deadline_file="${_state_dir}/apt-bounded.${_key//[^A-Za-z0-9_.-]/_}.deadline"

if [ -s "$_deadline_file" ]; then
  DEADLINE="$(cat "$_deadline_file")"
else
  DEADLINE=$(( $(_now) + APT_STEP_BUDGET ))
  printf '%s' "$DEADLINE" > "$_deadline_file"
fi

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then SUDO="sudo"; fi

ACQ=(-o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::Retries=3)

rc=0
for attempt in $(seq 1 "$APT_ATTEMPTS"); do
  remaining=$(( DEADLINE - $(_now) ))
  if [ "$remaining" -le 10 ]; then
    echo "apt-bounded: step budget of ${APT_STEP_BUDGET}s exhausted before attempt ${attempt} -- giving up so the STEP cap does not fire anonymously (last rc=$rc)" >&2
    # NEVER exit 0 here.  rc is 0 when the budget was consumed by an EARLIER
    # invocation in this step, so `${rc:-124}` would report SUCCESS for an
    # apt-get that never ran -- a silent failure worse than the hang this
    # wrapper exists to bound.
    [ "$rc" -eq 0 ] && rc=124
    exit "$rc"
  fi
  # Never let one attempt eat the whole remaining budget when more are allowed.
  slice="$APT_ATTEMPT_TIMEOUT"
  [ "$slice" -gt "$remaining" ] && slice="$remaining"

  if [ "$attempt" -gt 1 ]; then
    echo "apt-bounded: attempt ${attempt}/${APT_ATTEMPTS} (previous rc=$rc, ${remaining}s of step budget left)" >&2
    $SUDO dpkg --configure -a >/dev/null 2>&1 || true
  fi

  set +e
  $SUDO timeout --signal=TERM --kill-after=30 "$slice" apt-get "${ACQ[@]}" "$@"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] && exit 0
  if [ "$rc" -ne 124 ] && [ "$rc" -ne 100 ]; then
    echo "apt-bounded: apt-get exited $rc (not a timeout/transient) -- not retrying" >&2
    exit "$rc"
  fi
done
echo "apt-bounded: all ${APT_ATTEMPTS} attempts failed (last rc=$rc)" >&2
exit "$rc"
