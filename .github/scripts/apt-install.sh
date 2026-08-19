#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Install apt packages on a CI runner with WHOLE-OPERATION retry.
#
# WHY THIS EXISTS (#1595).  Every apt step in .github/workflows already carried
#
#     apt-get install -o Acquire::http::Timeout=30 -o Acquire::Retries=3 ...
#
# plus a 20-minute `timeout-minutes`.  That bounds each individual DOWNLOAD, not
# the operation.  Against a mirror that is slow rather than dead, every request
# creeps toward its 30 s ceiling and retries three times, so a multi-package
# install walks past 20 minutes inside ONE attempt and the job is killed with no
# second try -- and apt's own retries all target the same backend that is
# already struggling.
#
# Measured 2026-08-19: five distinct install steps blew the cap in a single
# afternoon -- `Install cppcheck` (in the MERGE QUEUE's own run, which evicted a
# green PR twice), `Install host build tools (dtc, ninja, ccache, gperf,
# libffi)` (twice, blocking a PR outright), `Install arm-none-eabi toolchain`,
# and `Install Yocto-side runtime deps`.  Re-running did not clear it.  Other
# runs the same afternoon succeeded, so the mirror was degraded, not down.
#
# The fix is to bound and retry the WHOLE operation: kill a slow attempt early
# and start over, which re-resolves the mirror DNS and usually lands on a
# healthier backend, instead of spending the entire budget on one bad one.
#
# Usage:  .github/scripts/apt-install.sh <pkg> [pkg...]
# Env:
#   APT_ATTEMPTS        attempts before giving up          (default 3)
#   APT_ATTEMPT_TIMEOUT seconds per attempt                (default 240)
#   APT_NO_RECOMMENDS   1 to pass --no-install-recommends  (default 0)
set -euo pipefail

if [ "$#" -eq 0 ]; then
	echo "apt-install: no packages given" >&2
	exit 2
fi

attempts="${APT_ATTEMPTS:-3}"
per="${APT_ATTEMPT_TIMEOUT:-240}"
recommends=""
[ "${APT_NO_RECOMMENDS:-0}" = "1" ] && recommends="--no-install-recommends"

# Keep apt's own per-request bounds: they still cap a single stuck download
# inside an attempt.  The outer `timeout` is what caps the attempt itself.
APT_OPTS="-o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::Retries=3"

# `timeout` returns 124 when it kills the child -- treated as a retryable slow
# mirror, same as any other non-zero status here.
#
# The cap wraps the WHOLE attempt (update + install), not each command
# separately. Capping them individually lets one attempt reach 2 x $per, so
# $attempts attempts could exceed the job's own `timeout-minutes` and the job
# would be killed mid-retry -- reintroducing the very failure this script exists
# to prevent, just later. Measured on the first CI run of this script: a "240s
# cap" attempt actually ran 5m10s.
attempt_once() {
	timeout "$per" sudo sh -c '
		set -e
		opts="$1"; shift
		recommends="$1"; shift
		# shellcheck disable=SC2086  # $opts is a deliberate word-split option list
		apt-get update $opts
		# shellcheck disable=SC2086
		apt-get install $opts -y $recommends "$@"
	' sh "$APT_OPTS" "$recommends" "$@"
}

i=1
while [ "$i" -le "$attempts" ]; do
	echo "== apt-install attempt $i/$attempts (${per}s cap): $* =="
	if attempt_once "$@"; then
		echo "== apt-install: ok on attempt $i =="
		exit 0
	fi
	if [ "$i" -lt "$attempts" ]; then
		# Brief, growing backoff: a degraded mirror often recovers within
		# seconds, and a fresh attempt re-resolves DNS to another backend.
		sleep $(( i * 10 ))
	fi
	i=$(( i + 1 ))
done

echo "apt-install: FAILED after $attempts attempts of ${per}s each." >&2
echo "             packages: $*" >&2
echo "             This is the degraded-package-mirror failure (#1595), not a repo defect --" >&2
echo "             re-running the job is reasonable, but if it persists the mirror is down." >&2
exit 1
