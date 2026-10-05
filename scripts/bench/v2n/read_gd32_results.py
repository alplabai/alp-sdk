#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Read the GD32-bridge result record a CM33 test app left for Linux.

Runs ON the A55 (stdlib only, needs /dev/mem and root).  The CM33 apps
examples/v2n/v2n-gd32-bridge-functional and v2n-gd32-bridge-hil-soak publish a
20-word record at A55 0x4F700F00 (layout: include/alp/protocol/gd32_bridge_results.h),
just below the liveness beacon at 0x4F700FF0 that provisioning's cm33_running reads.

    python3 read_gd32_results.py            # human readable
    python3 read_gd32_results.py --json     # one JSON object
    ssh root@board python3 - < scripts/bench/v2n/read_gd32_results.py

Exit status: 0 record valid, 2 no valid record (wrong image / not running),
3 /dev/mem unreadable.
"""

import json
import mmap
import os
import struct
import sys
import time

WINDOW = 0x4F700000  # A55 view of the OpenAMP rsctbl window
RESULTS_OFFSET = 0xF00
BEACON_OFFSET = 0xFF0
MAGIC = 0x47443352
BEACON_MAGIC = 0xA10D0683
WORDS = 20
# Word order of alp_gd32_results_t; tests/scripts/test_gd32_results_reader.py
# pins this list to the C header.
FIELDS = (
    "magic", "layout", "seq", "kind", "state", "tests_pass", "tests_fail",
    "tests_skip", "fw_version", "features", "max_payload", "flags",
    "read2_first", "read2_dropped", "read2_gaps", "soak_cycles",
    "soak_errors", "soak_timeouts", "soak_elapsed_s", "reserved",
)
KINDS = {1: "functional", 2: "soak"}
LINK_PENDING = 0
STATES = {LINK_PENDING: "link-not-up", 1: "running", 2: "done", 0xDEAD: "no-link"}
FLAGS = {
    1 << 0: "attn_granted", 1 << 1: "attn_active", 1 << 2: "attn_fallback",
    1 << 3: "batch_ok", 1 << 4: "stream2_ok",
}


def decode(raw, beacon=None):
    """raw: 80 bytes of the record -> dict, or None when magic/layout is wrong."""
    rec = dict(zip(FIELDS, struct.unpack("<%dI" % WORDS, raw)))
    if rec["magic"] != MAGIC or rec["layout"] != 1:
        return None
    rec["kind_name"] = KINDS.get(rec["kind"], "unknown")
    rec["state_name"] = STATES.get(rec["state"], "unknown")
    v = rec["fw_version"]
    rec["fw"] = "%d.%d.%d" % (v >> 16, (v >> 8) & 0xFF, v & 0xFF)
    rec["flag_names"] = [n for b, n in FLAGS.items() if rec["flags"] & b]
    if beacon is not None:
        rec["beacon_magic_ok"] = beacon[0] == BEACON_MAGIC
        rec["beacon_heartbeat"] = beacon[2]
    return rec


def words(mm, off, n):
    """n u32 at off, ONE aligned 32-bit load each: /dev/mem maps the no-map range
    as Device memory, where a multi-word copy may fault (as cm33_running avoids)."""
    return b"".join(struct.pack("<I", struct.unpack_from("<I", mm, off + 4 * i)[0]) for i in range(n))


def read_record(mm, tries=20):
    """Seqlock read: retry while the writer is mid-update (odd seq) or seq moved."""
    for _ in range(tries):
        a = words(mm, RESULTS_OFFSET, WORDS)
        b = words(mm, RESULTS_OFFSET, WORDS)
        seq = struct.unpack_from("<I", a, 8)[0]
        if seq % 2 == 0 and a == b:
            beacon = struct.unpack("<3I", words(mm, BEACON_OFFSET, 3))
            return decode(a, beacon)
        time.sleep(0.01)
    return None


def main(argv):
    try:
        fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
        mm = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=WINDOW)
    except OSError as e:
        print("cannot read /dev/mem at 0x%X: %s" % (WINDOW, e), file=sys.stderr)
        return 3
    rec = read_record(mm)
    if rec is None:
        print("no valid GD32 result record (magic 0x%08X) at 0x%X" % (MAGIC, WINDOW + RESULTS_OFFSET),
              file=sys.stderr)
        return 2
    if "--json" in argv:
        print(json.dumps(rec, sort_keys=True))
    else:
        for k in FIELDS[3:-1]:
            print("%-16s %s" % (k, rec[k]))
        print("%-16s %s" % ("flag_names", ",".join(rec["flag_names"]) or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
