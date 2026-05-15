#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# alp-remoteproc-start.sh -- Phase 3 remoteproc lifecycle launcher.
#
# On a heterogeneous SoM (V2N / AEN / NX9101) the Linux A-class side
# is responsible for loading + starting the M-class firmware whose
# ELF the orchestrator installed at /lib/firmware/alp/<SKU>/*.elf.
# The kernel's remoteproc framework exposes each programmable peer
# core as /sys/class/remoteproc/remoteprocN; this script walks those
# entries, finds the one matching our SKU + slice firmware path,
# starts it, and waits for the resulting /dev/rpmsg_ctrl0 to appear
# before exiting (so userspace services that depend on RPMsg can
# wait on this unit via After=alp-remoteproc.service).
#
# Exit codes:
#   0 -- one or more remoteprocs started successfully + RPMsg ctrl
#        chardev visible within the 5 s window.
#   1 -- no matching remoteproc found (no ALP firmware installed).
#   2 -- start succeeded but /dev/rpmsg_ctrl0 never appeared (kernel
#        side hung).
#
# Logs go to the journal via systemd's stdout/stderr forwarding.

set -eu

TIMEOUT_SECONDS=5
ANY_STARTED=0

for rp in /sys/class/remoteproc/remoteproc*; do
    [ -d "$rp" ] || continue

    fw=$(cat "$rp/firmware" 2>/dev/null || true)
    case "$fw" in
        alp/*)
            ;;
        *)
            # Not an ALP firmware; skip (might be vendor-provided).
            continue
            ;;
    esac

    state=$(cat "$rp/state" 2>/dev/null || echo "unknown")
    if [ "$state" = "running" ]; then
        echo "alp-remoteproc: $rp already running ($fw)"
        ANY_STARTED=1
        continue
    fi

    echo "alp-remoteproc: starting $rp with $fw"
    if ! echo start > "$rp/state"; then
        echo "alp-remoteproc: failed to start $rp" >&2
        continue
    fi
    ANY_STARTED=1
done

if [ "$ANY_STARTED" -eq 0 ]; then
    echo "alp-remoteproc: no ALP-labelled remoteproc found" >&2
    exit 1
fi

# Wait for the kernel to publish /dev/rpmsg_ctrl0 (the chardev that
# alp_rpc_open ioctls against).  The fragment is part of the standard
# rpmsg-driver bring-up after `echo start`; usually visible within
# 100-500 ms, but ALP runs the kernel + remoteproc start serially so
# we give it 5 s.
elapsed=0
while [ "$elapsed" -lt "$TIMEOUT_SECONDS" ]; do
    if [ -c /dev/rpmsg_ctrl0 ]; then
        echo "alp-remoteproc: /dev/rpmsg_ctrl0 present after ${elapsed}s"
        exit 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

echo "alp-remoteproc: /dev/rpmsg_ctrl0 never appeared after ${TIMEOUT_SECONDS}s" >&2
exit 2
