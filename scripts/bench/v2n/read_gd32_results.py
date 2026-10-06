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
    python3 read_gd32_results.py --fault    # the fatal-error block, if the CM33 died
    python3 read_gd32_results.py --no-live  # skip the 1.5 s heartbeat check
    ssh root@board python3 - < scripts/bench/v2n/read_gd32_results.py

By default the beacon heartbeat is sampled twice, 1.5 s apart: a record whose
heartbeat does not advance is a frozen CM33's last words, not a current result.

Exit status: 0 record valid (and, unless --no-live, heartbeat advancing), 2 no
valid record (wrong image / not running), 3 /dev/mem unreadable, 4 stale record
(not a result-publishing image), 5 STALLED (heartbeat not advancing), 6 --fault
and no fault block recorded.
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
LAYOUT = 2
BEACON_MAGIC = 0xA10D0683
BEACON_KIND = 0x200  # a result-publishing image; the idle shim is 0x100
WORDS = 20
FAULT_OFFSET = 0xF50
FAULT_MAGIC = 0x464C5431  # "FLT1"
FAULT_WORDS = 12
LIVE_GAP_S = 1.5  # the CM33 heartbeat ticks at 1 Hz
# Word order of alp_gd32_results_t; tests/scripts/test_gd32_results_reader.py
# pins this list to the C header.
FIELDS = (
    "magic", "layout", "seq", "kind", "state", "tests_pass", "tests_fail",
    "tests_skip", "fw_version", "features", "max_payload", "flags",
    "read2_first", "read2_dropped", "read2_gaps", "soak_cycles",
    "soak_errors", "soak_timeouts", "soak_elapsed_s", "fail_mask",
)
# Word order of alp_gd32_fault_t; the same test pins this to the C header.
FAULT_FIELDS = (
    "magic", "reason", "pc", "lr", "xpsr", "cfsr", "hfsr", "mmfar", "bfar",
    "thread_hash", "uptime_ms", "sp",
)
KINDS = {1: "functional", 2: "soak"}
LINK_PENDING = 0
STATES = {LINK_PENDING: "link-not-up", 1: "running", 2: "done", 0xDEAD: "no-link"}
# Row index -> name for fail_mask, in the order of each app's test table;
# tests/scripts/test_gd32_results_reader.py pins both lists to the C sources.
ROWS = {
    "functional": (
        "tmu_sqrt_4", "tmu_sqrt_2", "tmu_sin_0", "tmu_sin_pi_2", "tmu_sin_pi_6",
        "tmu_cos_0", "tmu_cos_pi", "tmu_cos_pi_3", "tmu_atan", "tmu_atan2",
        "tmu_hypot", "tmu_log", "tmu_sinh", "tmu_cosh", "tmu_tan_nosupport",
        "tmu_exp_nosupport", "tmu_tanh_nosupport", "tmu_q31_sqrt", "trng_lengths",
        "pwm_set_get", "pwm_configure", "adc_configure_error", "adc_all_channels",
        "link_features", "adc_stream2", "batch", "dsp_chain", "version_stable",
        "da9292_sentinel",
    ),
    "soak": (
        "ping", "get_version", "get_build_id", "reset_reason", "gpio", "pwm_set_get",
        "pwm_single_pulse", "pwm_capture", "adc_read", "adc_stream", "adc_stream_guard",
        "dac", "qenc", "counter", "trng", "tmu", "timer_sync", "power_mode",
        "da9292_sentinel", "ota_get_state", "adc_stream2", "batch",
    ),
}
FLAGS = {
    1 << 0: "attn_granted", 1 << 1: "attn_active", 1 << 2: "attn_fallback",
    1 << 3: "batch_ok", 1 << 4: "stream2_ok",
}


def decode(raw, beacon=None):
    """raw: 80 bytes of the record -> dict, or None when magic/layout is wrong."""
    rec = dict(zip(FIELDS, struct.unpack("<%dI" % WORDS, raw)))
    if rec["magic"] != MAGIC or rec["layout"] != LAYOUT:
        return None
    rec["kind_name"] = KINDS.get(rec["kind"], "unknown")
    rec["state_name"] = STATES.get(rec["state"], "unknown")
    v = rec["fw_version"]
    rec["fw"] = "%d.%d.%d" % (v >> 16, (v >> 8) & 0xFF, v & 0xFF)
    rec["fail_rows"] = [i for i in range(32) if rec["fail_mask"] >> i & 1]
    names = ROWS.get(rec["kind_name"], ())
    rec["fail_row_names"] = [names[i] if i < len(names) else "row%d" % i for i in rec["fail_rows"]]
    rec["flag_names"] = [n for b, n in FLAGS.items() if rec["flags"] & b]
    if beacon is not None:
        rec["beacon_magic_ok"] = beacon[0] == BEACON_MAGIC
        rec["beacon_kind"] = beacon[1]
        rec["beacon_heartbeat"] = beacon[2]
    return rec


def fnv1a(name):
    """The thread-name hash the CM33 fatal handler stores (FNV-1a, 32 bit)."""
    h = 2166136261
    for b in name.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


THREADS = {fnv1a(n): n for n in ("main", "idle", "sysworkq", "logging")}
THREADS[fnv1a("")] = "(unnamed/ISR)"


def decode_fault(raw):
    """raw: 48 bytes of the fault block -> dict, or None when no fault was recorded."""
    f = dict(zip(FAULT_FIELDS, struct.unpack("<%dI" % FAULT_WORDS, raw)))
    if f["magic"] != FAULT_MAGIC:
        return None
    f["thread"] = THREADS.get(f["thread_hash"], "hash 0x%08X" % f["thread_hash"])
    f["exception"] = f["xpsr"] & 0x1FF
    return f


def heartbeat_stalled(first, second):
    """True when two beacon heartbeat samples, LIVE_GAP_S apart, did not advance."""
    return first == second


def is_live(rec):
    """The record survives a warm reboot and the idle shim does not clear it, so
    only a running result-publishing image (beacon kind BEACON_KIND) makes it
    current; anything else is a stale record from an earlier image."""
    return rec.get("beacon_magic_ok") is True and rec.get("beacon_kind") == BEACON_KIND


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


def sample_heartbeat(mm):
    return struct.unpack("<3I", words(mm, BEACON_OFFSET, 3))[2]


def main(argv):
    try:
        fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
        mm = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=WINDOW)
    except OSError as e:
        print("cannot read /dev/mem at 0x%X: %s" % (WINDOW, e), file=sys.stderr)
        return 3
    if "--fault" in argv:
        f = decode_fault(words(mm, FAULT_OFFSET, FAULT_WORDS))
        if f is None:
            print("no fault block (magic 0x%08X) at 0x%X" % (FAULT_MAGIC, WINDOW + FAULT_OFFSET),
                  file=sys.stderr)
            return 6
        if "--json" in argv:
            print(json.dumps(f, sort_keys=True))
        else:
            for k in FAULT_FIELDS[1:]:
                print("%-12s 0x%08X" % (k, f[k]))
            print("%-12s %s (exception %d)" % ("thread", f["thread"], f["exception"]))
        return 0
    rec = read_record(mm)
    if rec is None:
        print("no valid GD32 result record (magic 0x%08X) at 0x%X" % (MAGIC, WINDOW + RESULTS_OFFSET),
              file=sys.stderr)
        return 2
    if not is_live(rec):
        print("stale GD32 result record: the CM33 is not running a result-publishing image "
              "(beacon magic %s, kind 0x%X)" % (rec.get("beacon_magic_ok"), rec.get("beacon_kind", 0)),
              file=sys.stderr)
        return 4
    if "--no-live" not in argv:
        hb = rec["beacon_heartbeat"]
        time.sleep(LIVE_GAP_S)
        hb2 = sample_heartbeat(mm)
        if heartbeat_stalled(hb, hb2):
            hint = ""
            if decode_fault(words(mm, FAULT_OFFSET, FAULT_WORDS)) is not None:
                hint = "; a fault block is recorded, run with --fault"
            print("STALLED: the CM33 heartbeat is frozen at %d (record is its last words)%s" % (hb, hint),
                  file=sys.stderr)
            return 5
    if "--json" in argv:
        print(json.dumps(rec, sort_keys=True))
    else:
        for k in FIELDS[3:-1]:
            print("%-16s %s" % (k, rec[k]))
        print("%-16s %s" % ("fail_rows", rec["fail_rows"]))
        print("%-16s %s" % ("fail_row_names", ",".join(rec["fail_row_names"]) or "-"))
        print("%-16s %s" % ("flag_names", ",".join(rec["flag_names"]) or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
