#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Cross-platform scope: every call site is an ubuntu-latest CI `run:` step
# (apt-get is Debian/Ubuntu-only), so there is no macOS/Windows/WSL
# equivalent this script needs to document or provide.
#
# Run apt-get under a WALL-CLOCK bound, with a dpkg-safe retry.
#
# WHY: `Acquire::http::Timeout` bounds an IDLE read, not a SLOW one. Every byte
# that arrives resets the timer, so a mirror that trickles defeats it forever,
# and apt has no minimum-transfer-rate option (no equivalent of curl's
# --speed-limit). Measured against two local servers (#1575):
#
#   server                              result
#   ----------------------------------  ------------------------------------
#   accepts, sends headers, then silent  rc=100 after 127s -- Timeout=30 FIRED,
#                                        3 retries, apt gave up on its own
#   accepts, then 1 byte every 20s       NEVER returns; only an external kill
#                                        ended it. Unbounded.
#
# So the transport options stay (they handle the silent class efficiently), and
# this wrapper adds the only thing that bounds the trickle class: a total-time
# limit, plus a retry against a different mirror draw.
#
# dpkg safety: `timeout` can kill apt-get mid-unpack, leaving the dpkg database
# half-configured or the lock held. Every retry therefore runs
# `dpkg --configure -a` first. That is the standard recovery and is a no-op when
# nothing was interrupted.
#
# Usage:  scripts/ci/apt-bounded.sh update
#         scripts/ci/apt-bounded.sh install -y --no-install-recommends foo bar
set -euo pipefail

# Per-attempt wall clock and attempt count. Generous: this bounds a HANG, it
# does not police a slow-but-progressing mirror.
: "${APT_ATTEMPT_TIMEOUT:=300}"
: "${APT_ATTEMPTS:=3}"

SUDO=""
[ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && SUDO="sudo"

# Transport options: bound the IDLE-read class cheaply so the wall-clock bound
# below is only ever reached by a genuine trickle.
ACQ=(-o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::Retries=3)

rc=0
for attempt in $(seq 1 "$APT_ATTEMPTS"); do
  if [ "$attempt" -gt 1 ]; then
    echo "apt-bounded: attempt $attempt/$APT_ATTEMPTS (previous rc=$rc)" >&2
    # A killed apt-get may have left dpkg mid-configure or holding the lock.
    $SUDO dpkg --configure -a >/dev/null 2>&1 || true
  fi
  set +e
  $SUDO timeout --signal=TERM --kill-after=30 "$APT_ATTEMPT_TIMEOUT" \
      apt-get "${ACQ[@]}" "$@"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] && exit 0
  # 124 = our timeout fired (the trickle case). 100 = apt's own transient
  # failure. Anything else is a real error -- surface it without burning
  # further attempts on it.
  if [ "$rc" -ne 124 ] && [ "$rc" -ne 100 ]; then
    echo "apt-bounded: apt-get exited $rc (not a timeout/transient) -- not retrying" >&2
    exit "$rc"
  fi
done
echo "apt-bounded: all $APT_ATTEMPTS attempts failed (last rc=$rc)" >&2
exit "$rc"
