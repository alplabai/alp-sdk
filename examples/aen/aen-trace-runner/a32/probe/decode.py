#!/usr/bin/env python3
"""Decode the A32 probe results block.

Layout (little-endian uint32 throughout) is defined in
docs/2026-09-22-a32-probe-spec.md ("Results block") and mirrored in
results.h -- keep all three in sync by hand. The fill-pattern constants and
test sequence below mirror main.c the same way (see expected_checksum()) --
if main.c's run_buffer_tests() call order or pass counts change, update
CHECKSUM_PLAN here too, or this check will start flagging good runs.

Usage:
    decode.py <dump.bin>     parse a captured 0x023FF000.. dump
    decode.py --selftest     build synthetic blocks in-process and verify
                              the MB/s math and the checksum cross-check,
                              without any hardware
"""
import struct
import sys

MAGIC = 0xA32A0001
MASK32 = 0xFFFFFFFF

HEADER_FMT = "<8I"  # magic,stage,fault_code,dfsr,dfar,ifsr,ifar,fault_lr
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 0x20

DIAG_OFFSET = 0x20
DIAG_FMT = "<6I"  # cntfrq,sctlr,cpacr,fpexc,checksum,sram1_word
DIAG_SIZE = struct.calcsize(DIAG_FMT)  # 0x18, ends at 0x38

TESTS_OFFSET = 0x40  # 8 bytes of padding after the diag block, per the spec table
TEST_FMT = "<5I"  # test_id,bytes_lo,bytes_hi,ticks_lo,ticks_hi
TEST_SIZE = struct.calcsize(TEST_FMT)  # 20
MAX_TESTS = 32

EXPECTED_CNTFRQ = 100_000_000  # spec: "CNTFRQ = 100 MHz"

BUFFER_NAMES = {1: "NC", 2: "WB-1MiB", 3: "WB-16KiB", 4: "NC-dst", 5: "WB-S1"}
OP_NAMES = {
    1: "scalar-fill",
    2: "neon-fill",
    3: "neon-read",
    4: "neon-copy",
    5: "clean",
    6: "sentinel",
    7: "dual-fill",
    8: "dual-clean",
    9: "dual-read",
}
OP_SENTINEL = 6
FAULT_NAMES = {0: "none", 1: "undef", 2: "pabort", 3: "dabort"}

# Stage 2 (appended after the stage-1 0x2C0-byte layout -- see results.h).
STAGE2_OFFSET = 0x2C0
STAGE2_SIZE = 0x354 - 0x2C0  # through mailbox_sentinel_written (0x350); probe_recovery_pc/
# probe_faulted (0x354/0x358) are start.S-internal handshake state, not reported here.
CDC_REG_NAMES = ("L1CFB(0x134)", "SRCTRL(0x24)", "POS_STAT(0x44)")
# cdc_fault bit encoding (see results.h): bit0 = synchronous fault caught by probe_read32,
# bit1 = ISR.A (external/async abort pending) was set right after the read -- CPSR.A is
# masked for this whole image and NS can't unmask it, so an async abort from an
# unclocked/unpowered CDC200 would otherwise show up as a garbage value with bit0==0.
CDC_FAULT_NAMES = {0: "ok", 1: "sync-fault", 2: "async-pending", 3: "sync+async"}
# r->stage markers set outside the per-test TEST_ID encoding -- useful for reading "where did
# it get stuck" off a wedged target's stage field.
STAGE_NAMES = {
    0x5A1: "SRAM1 probe (stage 1)",
    0xD0E: "done",
    0xF000: "PMU test",
    0xF001: "PSCI CPU_ON",
    0xF010: "dual round 0 (WB-S1 fill)",
    0xF011: "dual round 1 (WB-S1 clean)",
    0xF012: "dual round 2 (NC fill)",
    0xF013: "dual round 3 (WB-S1 read)",
    0xF014: "dual round 4 (LDREX/STREX)",
    0xF015: "dual round 5 (coherency)",
    0xCDC0: "CDC200 read L1CFB",
    0xCDC1: "CDC200 read SRCTRL",
    0xCDC2: "CDC200 read POS_STAT",
}
PSCI_NAMES = {
    0: "SUCCESS",
    -1 & MASK32: "NOT_SUPPORTED",
    -2 & MASK32: "INVALID_PARAMETERS",
    -3 & MASK32: "DENIED",
    -4 & MASK32: "ALREADY_ON",
    -5 & MASK32: "ON_PENDING",
    -6 & MASK32: "INTERNAL_FAILURE",
    -7 & MASK32: "NOT_PRESENT",
    -8 & MASK32: "DISABLED",
    -9 & MASK32: "INVALID_ADDRESS",
}
LDREX_ITERS_PER_CORE = 1_000_000
COH_BUF_WORDS = 64 * 1024 // 4
ROUND_NAMES = ("WB-S1 fill", "WB-S1 clean", "NC fill", "WB-S1 read", "LDREX/STREX", "coherency")

# --- checksum cross-check -------------------------------------------------
# Mirrors main.c's fill patterns and run_buffer_tests() sequence exactly
# (same constants, same order of XOR contributions into `checksum`) so an
# independently-computed expected value can be compared against the
# captured one. See main.c's FILL_MUL comment for why a plain/uniform
# pattern can't be checked this way: XOR-reducing it over a power-of-2 word
# count is structurally 0 for real data AND for a silently-dropped write
# alike, so a "recomputed expected checksum" built on that pattern would be
# 0 == 0 either way -- useless. FILL_MUL breaks that.
SCALAR_BASE = 0xA5A5A5A5
FILL_BASE = 0xA5A5A5A5
FILL_MUL = 0x9E3779B1
ONE_MIB_WORDS = 1024 * 1024 // 4
SIXTEEN_KIB_WORDS = 16 * 1024 // 4
PASSES_1MIB = 16
PASSES_16KIB = 512

NEON_FILL_SAMPLE = FILL_BASE  # word[0] = FILL_BASE ^ (0*FILL_MUL) = FILL_BASE, always


def scalar_fill_sample(passes):
    """word[0] after the last timed pass: base=(SCALAR_BASE^(passes-1)), word[0]=base^0=base."""
    return (SCALAR_BASE ^ (passes - 1)) & MASK32


def neon_read_reduced(words):
    """XOR-reduce of {FILL_BASE ^ (i*FILL_MUL) : i=0..words-1} -- replayed literally
    (not a hand-derived closed form) so this can never silently drift from what
    main.c's neon_fill()+neon_read_xor() actually computes on real hardware."""
    acc = 0
    im = 0
    for _ in range(words):
        acc ^= (FILL_BASE ^ im) & MASK32
        im = (im + FILL_MUL) & MASK32
    return acc


def coh_reduced(seed, words):
    """XOR-reduce of {seed + i*FILL_MUL : i=0..words-1} -- the cross-L1 coherency test's
    pattern (main.c's coherency_round). Deliberately ADDS the seed rather than XOR-ing it
    in (like neon_read_reduced above does with FILL_BASE): XOR-ing a seed in at the top
    cancels out of the reduction entirely for our even word counts (same reasoning as the
    FILL_MUL fix itself -- XOR-reducing N copies of any single fixed value is 0 for even
    N), which would make the checksum unable to tell two different seeds apart. Addition
    doesn't have that cancellation (verified by direct computation for several seeds, not
    just assumed), so the seed recorded in the dump (coh_seed) is load-bearing here."""
    acc = 0
    im = 0
    for _ in range(words):
        acc ^= (seed + im) & MASK32
        im = (im + FILL_MUL) & MASK32
    return acc


# (buffer_id, words, passes, has_clean) for the three run_buffer_tests() calls in main(),
# in call order.
CHECKSUM_PLAN = [
    (1, ONE_MIB_WORDS, PASSES_1MIB, False),
    (2, ONE_MIB_WORDS, PASSES_1MIB, True),
    (3, SIXTEEN_KIB_WORDS, PASSES_16KIB, True),
]


def expected_checksum():
    cs = 0
    for _buf_id, words, passes, has_clean in CHECKSUM_PLAN:
        cs ^= scalar_fill_sample(passes)
        cs ^= NEON_FILL_SAMPLE  # neon fill sample
        cs ^= neon_read_reduced(words)  # neon read sample (taken once, not per pass -- see main.c)
        cs ^= NEON_FILL_SAMPLE  # neon copy dst sample: dst[0] == src[0] == FILL_BASE
        if has_clean:
            cs ^= NEON_FILL_SAMPLE  # clean test re-dirties with neon_fill, samples buf[0] after
    return cs & MASK32


def fault_pc(fault_code, fault_lr):
    """Faulting instruction address from the banked LR, per exception type
    (ARM ARM return-address-offset table): Data Abort's LR is the aborting
    instruction + 8; Undefined Instruction's and Prefetch Abort's LR is the
    faulting instruction + 4."""
    if fault_code == 3:  # dabort
        return (fault_lr - 8) & MASK32
    if fault_code in (1, 2):  # undef, pabort
        return (fault_lr - 4) & MASK32
    return None


def decode(blob):
    magic, stage, fault_code, dfsr, dfar, ifsr, ifar, fault_lr = struct.unpack_from(HEADER_FMT, blob, 0)
    cntfrq, sctlr, cpacr, fpexc, checksum, sram1_word = struct.unpack_from(DIAG_FMT, blob, DIAG_OFFSET)

    result = {
        "magic": magic,
        "magic_ok": magic == MAGIC,
        "stage": stage,
        "fault_code": fault_code,
        "fault_name": FAULT_NAMES.get(fault_code, "unknown(%d)" % fault_code),
        "dfsr": dfsr,
        "dfar": dfar,
        "ifsr": ifsr,
        "ifar": ifar,
        "fault_lr": fault_lr,
        "fault_pc": fault_pc(fault_code, fault_lr),
        "cntfrq": cntfrq,
        "cntfrq_ok": cntfrq == EXPECTED_CNTFRQ,
        "sctlr": sctlr,
        "cpacr": cpacr,
        "fpexc": fpexc,
        "checksum": checksum,
        "sram1_word": sram1_word,
        "tests": [],
    }

    off = TESTS_OFFSET
    for _ in range(MAX_TESTS):
        if off + TEST_SIZE > len(blob):
            break
        test_id, bytes_lo, bytes_hi, ticks_lo, ticks_hi = struct.unpack_from(TEST_FMT, blob, off)
        off += TEST_SIZE
        if test_id == 0 and bytes_lo == 0 and ticks_lo == 0:
            break  # first unused (zeroed) trailing entry
        buf_id = (test_id >> 8) & 0xFF
        op_id = test_id & 0xFF
        entry = {
            "test_id": test_id,
            "buffer": BUFFER_NAMES.get(buf_id, "buf%d" % buf_id),
            "op": OP_NAMES.get(op_id, "op%d" % op_id),
        }
        if op_id == OP_SENTINEL:
            entry["expected"] = bytes_lo
            entry["actual"] = ticks_lo
            entry["pass"] = bytes_hi == 1
        else:
            n_bytes = bytes_lo | (bytes_hi << 32)
            ticks = ticks_lo | (ticks_hi << 32)
            entry["bytes"] = n_bytes
            entry["ticks"] = ticks
            if ticks == 0:
                entry["mb_s"] = None  # error: can't compute a rate from zero ticks
            else:
                entry["mb_s"] = n_bytes * cntfrq / ticks / 1e6
        result["tests"].append(entry)

    result["expected_checksum"] = expected_checksum()
    result["checksum_ok"] = checksum == result["expected_checksum"]

    result["stage2"] = None
    if len(blob) >= STAGE2_OFFSET + STAGE2_SIZE:
        (
            pmu_available,
            pmu_cycles,
            pmu_cntvct_ticks,
            psci_cpu_on_ret,
            secondary_mpidr,
            secondary_ready,
            *dual_go_done,
        ) = struct.unpack_from("<6I12I", blob, STAGE2_OFFSET)
        dual_go = dual_go_done[0:6]
        dual_done = dual_go_done[6:12]
        (secondary_parked,) = struct.unpack_from("<I", blob, STAGE2_OFFSET + 0x308 - 0x2C0)
        cdc_reg0 = struct.unpack_from("<3I", blob, STAGE2_OFFSET + 0x30C - 0x2C0)
        cdc_fault0 = struct.unpack_from("<3I", blob, STAGE2_OFFSET + 0x318 - 0x2C0)
        cdc_reg1 = struct.unpack_from("<3I", blob, STAGE2_OFFSET + 0x324 - 0x2C0)
        cdc_fault1 = struct.unpack_from("<3I", blob, STAGE2_OFFSET + 0x330 - 0x2C0)
        ldrex_final, ldrex_c0_ticks, ldrex_c1_ticks, coh_actual, coh_seed, mailbox_written = struct.unpack_from(
            "<6I", blob, STAGE2_OFFSET + 0x33C - 0x2C0
        )

        core1_up = secondary_mpidr != 0
        expected_ldrex = 2 * LDREX_ITERS_PER_CORE
        coh_expected = coh_reduced(coh_seed, COH_BUF_WORDS)

        result["stage2"] = {
            "pmu_available": bool(pmu_available),
            "pmu_cycles": pmu_cycles,
            "pmu_cntvct_ticks": pmu_cntvct_ticks,
            "pmu_mhz": (pmu_cycles * cntfrq / pmu_cntvct_ticks / 1e6) if pmu_available and pmu_cntvct_ticks else None,
            "psci_cpu_on_ret": psci_cpu_on_ret,
            "psci_name": PSCI_NAMES.get(psci_cpu_on_ret, "unknown(%d)" % psci_cpu_on_ret),
            "secondary_mpidr": secondary_mpidr,
            "core1_up": core1_up,
            "secondary_ready": bool(secondary_ready),
            "secondary_parked": bool(secondary_parked),
            "dual_go": dual_go,
            "dual_done": dual_done,
            "rounds_completed": sum(1 for d in dual_done if d),
            "cdc_reg0": cdc_reg0,
            "cdc_fault0": cdc_fault0,
            "cdc_reg1": cdc_reg1,
            "cdc_fault1": cdc_fault1,
            "ldrex_final_value": ldrex_final,
            "ldrex_expected": expected_ldrex,
            "ldrex_ok": ldrex_final == expected_ldrex,
            "ldrex_core0_ticks": ldrex_c0_ticks,
            "ldrex_core1_ticks": ldrex_c1_ticks,
            "coh_actual_checksum": coh_actual,
            "coh_expected_checksum": coh_expected,
            "coh_seed": coh_seed,
            "coh_ok": coh_actual == coh_expected,
            "mailbox_sentinel_written": bool(mailbox_written),
        }

        # Dual-core aggregate bandwidth per round: sum of both cores' bytes over the
        # WALL-CLOCK time the pair actually took, i.e. the slower core's ticks --
        # that's what "aggregate MB/s" has to mean for two things running concurrently.
        by_round = {}
        for t in result["tests"]:
            core = (t["test_id"] >> 16) & 0xFF
            round_op = t["test_id"] & 0xFF
            if round_op in (7, 8, 9) and "ticks" in t:  # dual-core ops only
                key = (round_op, t["buffer"])
                by_round.setdefault(key, {})[core] = t
        aggregates = []
        for (_op, buf), cores in by_round.items():
            if 0 in cores and 1 in cores:
                b = cores[0]["bytes"] + cores[1]["bytes"]
                ticks = max(cores[0]["ticks"], cores[1]["ticks"])
                aggregates.append(
                    {
                        "buffer": buf,
                        "op": cores[0]["op"],
                        "bytes": b,
                        "ticks": ticks,
                        "mb_s": (b * cntfrq / ticks / 1e6) if ticks else None,
                    }
                )
        result["stage2"]["dual_aggregates"] = aggregates

    return result


def format_report(r):
    lines = []
    stage_name = STAGE_NAMES.get(r["stage"])
    lines.append(
        "magic=0x%08X (%s)  stage=0x%X%s  fault=%s"
        % (
            r["magic"],
            "OK" if r["magic_ok"] else "BAD",
            r["stage"],
            (" (%s)" % stage_name) if stage_name else "",
            r["fault_name"],
        )
    )
    if r["fault_code"]:
        lines.append(
            "  DFSR=0x%08X DFAR=0x%08X IFSR=0x%08X IFAR=0x%08X LR=0x%08X PC=%s"
            % (
                r["dfsr"],
                r["dfar"],
                r["ifsr"],
                r["ifar"],
                r["fault_lr"],
                "0x%08X" % r["fault_pc"] if r["fault_pc"] is not None else "?",
            )
        )
    cntfrq_flag = "" if r["cntfrq_ok"] else "  *** ERROR: expected %d ***" % EXPECTED_CNTFRQ
    lines.append(
        "CNTFRQ=%d%s SCTLR=0x%08X CPACR=0x%08X FPEXC=0x%08X sram1=0x%08X"
        % (r["cntfrq"], cntfrq_flag, r["sctlr"], r["cpacr"], r["fpexc"], r["sram1_word"])
    )
    checksum_flag = (
        "OK" if r["checksum_ok"] else "*** MISMATCH: expected 0x%08X ***" % r["expected_checksum"]
    )
    lines.append("checksum=0x%08X  %s" % (r["checksum"], checksum_flag))
    for t in r["tests"]:
        if t["op"] == "sentinel":
            status = "PASS" if t["pass"] else "*** FAIL (silently-dropped write?) ***"
            lines.append(
                "  [%-8s %-12s] expected=0x%08X actual=0x%08X  %s"
                % (t["buffer"], t["op"], t["expected"], t["actual"], status)
            )
        elif t["mb_s"] is None:
            lines.append(
                "  [%-8s %-12s] %10d bytes  %10d ticks  *** ERROR (ticks=0) ***"
                % (t["buffer"], t["op"], t["bytes"], t["ticks"])
            )
        else:
            lines.append(
                "  [%-8s %-12s] %10d bytes  %10d ticks  %10.2f MB/s"
                % (t["buffer"], t["op"], t["bytes"], t["ticks"], t["mb_s"])
            )

    s2 = r.get("stage2")
    if s2 is None:
        lines.append("\n(no stage-2 data in this dump -- capture through 0x%X, not just the stage-1 tail)" % (STAGE2_OFFSET + STAGE2_SIZE))
        return "\n".join(lines)

    lines.append("\n-- stage 2 --")
    if s2["pmu_available"]:
        mhz = s2["pmu_mhz"]
        lines.append(
            "PMU: %d cycles / %d ticks (target 10,000,000) = %s"
            % (s2["pmu_cycles"], s2["pmu_cntvct_ticks"], ("%.1f MHz" % mhz) if mhz else "?")
        )
    else:
        lines.append("PMU: *** unavailable (PMCR/PMCNTENSET/PMCCNTR faulted -- NS access blocked by SDCR/MDCR?) ***")

    lines.append(
        "PSCI CPU_ON: ret=%d (%s)  secondary_mpidr=0x%08X (core1 %s)  secondary_ready=%s  secondary_parked=%s"
        % (
            s2["psci_cpu_on_ret"],
            s2["psci_name"],
            s2["secondary_mpidr"],
            "UP" if s2["core1_up"] else "never reported in",
            s2["secondary_ready"],
            s2["secondary_parked"],
        )
    )

    if not s2["core1_up"]:
        lines.append(
            "  (core1 never came up -- dual-core rounds below did not run; "
            "treat LDREX/coherency/dual-aggregate numbers as N/A, not failures)"
        )
    else:
        lines.append("dual-core rounds completed: %d/6 (%s)" % (s2["rounds_completed"], ", ".join(ROUND_NAMES)))
        for agg in s2["dual_aggregates"]:
            mb = agg["mb_s"]
            lines.append(
                "  [%-8s %-10s aggregate] %10d bytes  %10d ticks  %s"
                % (agg["buffer"], agg["op"], agg["bytes"], agg["ticks"], ("%.2f MB/s" % mb) if mb else "ERROR")
            )

        ldrex_flag = "OK" if s2["ldrex_ok"] else "*** MISMATCH: expected %d ***" % s2["ldrex_expected"]
        lines.append(
            "LDREX/STREX: final=%d (%s)  core0_ticks=%d core1_ticks=%d"
            % (s2["ldrex_final_value"], ldrex_flag, s2["ldrex_core0_ticks"], s2["ldrex_core1_ticks"])
        )

        coh_flag = "OK" if s2["coh_ok"] else "*** MISMATCH: expected 0x%08X ***" % s2["coh_expected_checksum"]
        lines.append(
            "cross-L1 coherency: seed=0x%08X checksum=0x%08X (%s)"
            % (s2["coh_seed"], s2["coh_actual_checksum"], coh_flag)
        )

    for label, regs, faults in (("read#1", s2["cdc_reg0"], s2["cdc_fault0"]), ("read#2 (~1ms later)", s2["cdc_reg1"], s2["cdc_fault1"])):
        parts = []
        for name, val, flt in zip(CDC_REG_NAMES, regs, faults):
            if flt:
                parts.append("%s=FAULT(%s)" % (name, CDC_FAULT_NAMES.get(flt, "bits=0x%x" % flt)))
            else:
                parts.append("%s=0x%08X" % (name, val))
        lines.append("CDC200 %s: %s" % (label, "  ".join(parts)))
    if s2["cdc_reg0"][2] != s2["cdc_reg1"][2] and not (s2["cdc_fault0"][2] or s2["cdc_fault1"][2]):
        lines.append("  POS_STAT changed between reads -- CDC200 is actively scanning")
    elif not (s2["cdc_fault0"][2] or s2["cdc_fault1"][2]):
        lines.append("  POS_STAT unchanged between reads")

    lines.append("mailbox sentinel written: %s (0x54524D42 @ 0x02401000)" % s2["mailbox_sentinel_written"])

    return "\n".join(lines)


def build_synthetic_block(
    checksum=None,
    sentinel_pass=True,
    ticks_zero=False,
    cntfrq=EXPECTED_CNTFRQ,
    core1_up=True,
    ldrex_ok=True,
    coh_ok=True,
    coh_seed=0x12345678,
    cdc_async_fault=False,
):
    """A hand-checkable results block for --selftest: no hardware involved."""
    if checksum is None:
        checksum = expected_checksum()
    header = struct.pack(HEADER_FMT, MAGIC, 0xD0E, 0, 0, 0, 0, 0, 0)
    diag = struct.pack(DIAG_FMT, cntfrq, 0xC0000000 | 0x1005, 0x00F00000, 0x40000000, checksum, 0x11223344)
    pad = b"\x00" * (TESTS_OFFSET - HEADER_SIZE - DIAG_SIZE)
    sentinel = struct.pack(
        TEST_FMT, (1 << 8) | OP_SENTINEL, 0xC3A5C3A5, 1 if sentinel_pass else 0, 0xC3A5C3A5 if sentinel_pass else 0, 0
    )
    # 100 MB moved in exactly 1 second of 100 MHz ticks -> 100 MB/s.
    t1 = struct.pack(TEST_FMT, (1 << 8) | 1, 100_000_000, 0, 0 if ticks_zero else 100_000_000, 0)
    # 1 MiB moved in 1000 ticks @ 100 MHz -> 1048576 * 1e8 / 1000 / 1e6 = 104857.6 MB/s.
    t2 = struct.pack(TEST_FMT, (2 << 8) | 2, 1024 * 1024, 0, 1000, 0)
    # A pair of dual-core fill entries (core0/core1), same round, disjoint halves, so the
    # aggregate MB/s path gets exercised too.
    half_bytes = 512 * 1024
    t_dual0 = struct.pack(TEST_FMT, (0 << 16) | (5 << 8) | 7, half_bytes, 0, 1000, 0)
    t_dual1 = struct.pack(TEST_FMT, (1 << 16) | (5 << 8) | 7, half_bytes, 0, 1200, 0)
    tests_tail = sentinel + t1 + t2 + t_dual0 + t_dual1
    stage1_blob = header + diag + pad + tests_tail

    stage2_pad = b"\x00" * (STAGE2_OFFSET - len(stage1_blob))

    pmu_cycles = 80_000_000  # -> 800 MHz over a 10,000,000-tick (100 ms) window
    pmu_block = struct.pack("<3I", 1, pmu_cycles, 10_000_000)
    psci_block = struct.pack("<I", 0 if core1_up else (-3 & MASK32))  # SUCCESS or DENIED
    secondary_mpidr = 0x80000101 if core1_up else 0
    mpidr_ready_block = struct.pack("<2I", secondary_mpidr, 1 if core1_up else 0)
    dual_go = struct.pack("<6I", *([1] * 6 if core1_up else [0] * 6))
    dual_done = struct.pack("<6I", *([1] * 6 if core1_up else [0] * 6))
    cdc_reg0 = struct.pack("<3I", 0x11111111, 0x22222222, 0x1000)
    cdc_fault0 = struct.pack("<3I", 0, 0, 2 if cdc_async_fault else 0)  # POS_STAT read: async-pending
    cdc_reg1 = struct.pack("<3I", 0x11111111, 0x22222222, 0x1010)  # POS_STAT moved
    cdc_fault1 = struct.pack("<3I", 0, 0, 0)
    ldrex_final = (2 * LDREX_ITERS_PER_CORE) if ldrex_ok else 1234
    coh_actual = coh_reduced(coh_seed, COH_BUF_WORDS) if coh_ok else 0
    tail_block = struct.pack("<6I", ldrex_final, 500_000, 480_000, coh_actual, coh_seed, 1)

    stage2_blob = (
        pmu_block
        + psci_block
        + mpidr_ready_block
        + dual_go
        + dual_done
        + struct.pack("<I", 1 if core1_up else 0)  # secondary_parked
        + cdc_reg0
        + cdc_fault0
        + cdc_reg1
        + cdc_fault1
        + tail_block
    )
    assert len(stage2_blob) == STAGE2_SIZE, len(stage2_blob)

    return stage1_blob + stage2_pad + stage2_blob


def selftest():
    ok = True

    # Case 1: everything matches (checksum, sentinel, cntfrq all good).
    r = decode(build_synthetic_block())
    print(format_report(r))
    assert r["magic_ok"], "magic mismatch"
    assert r["stage"] == 0xD0E, r["stage"]
    assert r["checksum_ok"], "checksum should have matched"
    assert r["cntfrq_ok"]
    sentinel_entry = r["tests"][0]
    assert sentinel_entry["op"] == "sentinel" and sentinel_entry["pass"]
    mb1 = r["tests"][1]["mb_s"]
    mb2 = r["tests"][2]["mb_s"]
    assert abs(mb1 - 100.0) < 1e-6, mb1
    assert abs(mb2 - 104857.6) < 1e-3, mb2
    print("case 1 (all good) OK: %.1f MB/s and %.1f MB/s computed correctly\n" % (mb1, mb2))

    # Case 2: mismatch case -- checksum wrong (e.g. a firewalled/all-zero region
    # would XOR in 0 instead of the real per-test samples), sentinel FAIL, and a
    # ticks==0 entry. decode.py must flag all three, not silently show 0.00.
    bad = build_synthetic_block(checksum=0, sentinel_pass=False, ticks_zero=True)
    rb = decode(bad)
    print(format_report(rb))
    assert not rb["checksum_ok"], "checksum mismatch should have been flagged"
    assert rb["tests"][0]["pass"] is False
    assert rb["tests"][1]["mb_s"] is None, "ticks=0 must not report a rate"
    print("case 2 (mismatch) OK: checksum mismatch, sentinel FAIL and ticks=0 all flagged\n")

    # Case 3: CNTFRQ off the expected 100 MHz.
    rc = decode(build_synthetic_block(cntfrq=24_000_000))
    assert not rc["cntfrq_ok"]
    print("case 3 (bad CNTFRQ) OK: flagged as unexpected\n")

    # Case 4: core1 never came up (PSCI failed) -- stage2 section must say so
    # plainly and not report LDREX/coherency as pass or fail.
    r4 = decode(build_synthetic_block(core1_up=False))
    assert not r4["stage2"]["core1_up"]
    assert "core1 never came up" in format_report(r4)
    print("case 4 (core1 never up) OK: reported distinctly from a round failure\n")

    # Case 5: LDREX/STREX and cross-L1 coherency each independently wrong --
    # both must be flagged (this is the "hardware answer was actually broken"
    # case, as opposed to case 4's "core1 didn't even start").
    r5 = decode(build_synthetic_block(ldrex_ok=False, coh_ok=False))
    assert not r5["stage2"]["ldrex_ok"]
    assert not r5["stage2"]["coh_ok"]
    print("case 5 (LDREX + coherency mismatch) OK: both flagged\n")

    # Case 6: a different coh_seed must change the expected (and, if the silicon really
    # published that seed, the actual) checksum -- proves the seed is load-bearing, not
    # just recorded for show.
    r6a = decode(build_synthetic_block(coh_seed=0x1))
    r6b = decode(build_synthetic_block(coh_seed=0xDEADBEEF))
    assert r6a["stage2"]["coh_ok"] and r6b["stage2"]["coh_ok"]
    assert r6a["stage2"]["coh_expected_checksum"] != r6b["stage2"]["coh_expected_checksum"]
    print("case 6 (coh_seed changes the expected checksum) OK\n")

    # Case 7: CDC async-pending (ISR.A) fault kind -- a masked external abort that a bare
    # sync-fault check would miss entirely (garbage value, fault flag 0). Must show up as
    # its own distinct fault kind, not silently read as a valid register value.
    r7 = decode(build_synthetic_block(cdc_async_fault=True))
    assert r7["stage2"]["cdc_fault0"][2] == 2
    report7 = format_report(r7)
    assert "async-pending" in report7
    print("case 7 (CDC async-pending fault kind) OK: distinct from a sync fault\n")

    # Case 8: stage marker names resolve for a mid-run wedge (e.g. stuck in dual round 4).
    stuck = bytearray(build_synthetic_block())
    struct.pack_into("<I", stuck, 4, 0xF014)  # stage field, offset 0x04
    r8 = decode(bytes(stuck))
    assert r8["stage"] == 0xF014
    assert "LDREX/STREX" in format_report(r8)
    print("case 8 (stage marker name resolves) OK\n")

    print("selftest OK" if ok else "selftest FAILED")


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        selftest()
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 1
    with open(argv[1], "rb") as f:
        blob = f.read()
    print(format_report(decode(blob)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
