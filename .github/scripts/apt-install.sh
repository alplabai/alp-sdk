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

# Clear the wreckage a killed attempt leaves behind.
#
# MEASURED, not defensive hand-waving: when `timeout` fired, apt-get SURVIVED as
# an orphan still holding the dpkg lock, so both retries died in seconds with
#
#   E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 2599 (apt-get)
#
# i.e. the retry could never have worked. Safe here because this only ever runs
# on an ephemeral single-purpose CI runner, where no other apt is legitimately in
# flight; do NOT lift this into anything that runs on a developer machine.
apt_cleanup() {
	sudo pkill -9 -x apt-get 2>/dev/null || true
	sudo pkill -9 -x dpkg 2>/dev/null || true
	sudo rm -f /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock \
		/var/cache/apt/archives/lock /var/lib/apt/lists/lock 2>/dev/null || true
	# A kill mid-unpack leaves packages half-configured; without this the next
	# install refuses to proceed.
	sudo dpkg --configure -a >/dev/null 2>&1 || true
}

# One attempt, bounded as a WHOLE by $per -- capping `update` and `install`
# separately lets a single attempt reach 2 x $per, so $attempts of them could
# exceed the job's own `timeout-minutes` and be killed mid-retry, reintroducing
# the failure this script exists to prevent. (Measured on this script's first CI
# run: a "240s cap" attempt ran 5m10s.)
#
# `timeout` is applied to each command but the REMAINING budget is recomputed, so
# the attempt total still cannot exceed $per. Deliberately no intermediate shell:
# `timeout ... sudo apt-get ...` lets sudo relay the signal to apt-get, whereas
# wrapping in `sudo sh -c` orphaned apt-get and produced the stuck lock above.
attempt_once() {
	local started remaining
	started=$(date +%s)
	remaining="$per"

	# shellcheck disable=SC2086  # APT_OPTS is a deliberate word-split option list
	timeout "$remaining" sudo apt-get update $APT_OPTS || return 1

	remaining=$(( per - ( $(date +%s) - started ) ))
	[ "$remaining" -gt 0 ] || return 1

	# shellcheck disable=SC2086
	timeout "$remaining" sudo apt-get install $APT_OPTS -y $recommends "$@" || return 1
	return 0
}

i=1
while [ "$i" -le "$attempts" ]; do
	echo "== apt-install attempt $i/$attempts (${per}s cap): $* =="
	if attempt_once "$@"; then
		echo "== apt-install: ok on attempt $i =="
		exit 0
	fi
	# A killed or failed attempt can leave apt-get orphaned on the dpkg lock;
	# clear it, or every remaining attempt fails in seconds for the wrong reason.
	apt_cleanup
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
