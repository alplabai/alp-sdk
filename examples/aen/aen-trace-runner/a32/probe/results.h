/* results.h -- layout of the A32 probe results block.
 *
 * This is the ONLY place the C-side view of the results block is defined.
 * start.S uses raw numeric offsets (it can't #include a C header) -- keep
 * the .equ block at the top of start.S in sync with this file by hand; the
 * _Static_assert()s below catch drift on the C side, and decode.py mirrors
 * the same offsets independently (see docs/2026-09-22-a32-probe-spec.md,
 * "Results block" table, for the base stage-1 byte layout; everything from
 * +0x2C0 on is a stage-2 addition, appended after the stage-1 layout so
 * stage-1 tooling/offsets are untouched).
 *
 * No instance of this struct is ever a global/static MUTABLE variable: this
 * image is XIP from MRAM and must carry no .data/.bss (see link.ld). The
 * struct is only ever reached through a pointer literal at RESULTS_BASE,
 * which is plain SRAM the linker never owns. (A `static const` array IS
 * fine -- that's .rodata, not .data/.bss; main.c uses one for the CDC200
 * register list.)
 */
#ifndef A32_PROBE_RESULTS_H
#define A32_PROBE_RESULTS_H

#include <stddef.h>
#include <stdint.h>

#define RESULTS_BASE  0x023FF000u
#define RESULTS_MAGIC 0xA32A0001u
#define MAX_TESTS     32u

/* fault_code values (spec: "0 none, 1 undef, 2 pabort, 3 dabort") -- this
 * is the FATAL fault record only; see PROBE_RECOVERY_PC below for the
 * separate non-fatal "expected, might fault" mechanism stage 2 adds. */
#define FAULT_NONE   0u
#define FAULT_UNDEF  1u
#define FAULT_PABORT 2u
#define FAULT_DABORT 3u

/* test_id = (core << 16) | (buffer << 8) | op. `core` is 0 for every
 * stage-1 test (test_id was always < 0x10000 then, so this is a backward-
 * compatible extension) and 0/1 for the stage-2 dual-core rounds. */
#define BUF_NC       1u
#define BUF_WB_1MIB  2u
#define BUF_WB_16KIB 3u
#define BUF_NC_DST   4u    /* copy-destination region, op=6 sentinel only */
#define BUF_WB_S1    5u    /* stage-2: the Inner-Shareable WB region dual-core tests use --
                             * deliberately NOT the same buffer as BUF_WB_1MIB (stage-1's WB
                             * numbers stay on the S=0 attribute so they remain comparable) */
#define OP_SCALAR_FILL 1u
#define OP_NEON_FILL   2u
#define OP_NEON_READ   3u
#define OP_NEON_COPY   4u
#define OP_CLEAN       5u
#define OP_SENTINEL    6u  /* write/readback pass-fail: bytes_lo=expected, bytes_hi=pass(1)/fail(0),
                             * ticks_lo=actual readback, ticks_hi unused -- see sentinel_check() */
#define OP_DUAL_FILL   7u  /* stage-2 dual-core rounds, one tests[] entry per core per round */
#define OP_DUAL_CLEAN  8u
#define OP_DUAL_READ   9u
#define TEST_ID(buf, op) (((uint32_t)(buf) << 8) | (uint32_t)(op))
#define TEST_ID_CORE(core, buf, op) (((uint32_t)(core) << 16) | ((uint32_t)(buf) << 8) | (uint32_t)(op))

typedef struct {
	uint32_t test_id;
	uint32_t bytes_lo;
	uint32_t bytes_hi;
	uint32_t ticks_lo;
	uint32_t ticks_hi;
} test_entry_t;

typedef struct {
	/* ---- stage 1, byte-for-byte unchanged (0x00..0x2C0) ---- */
	uint32_t magic;      /* +0x00 */
	uint32_t stage;      /* +0x04 */
	uint32_t fault_code; /* +0x08 */
	uint32_t dfsr;       /* +0x0C */
	uint32_t dfar;       /* +0x10 */
	uint32_t ifsr;       /* +0x14 */
	uint32_t ifar;       /* +0x18 */
	uint32_t fault_lr;   /* +0x1C */
	uint32_t cntfrq;     /* +0x20 */
	uint32_t sctlr;      /* +0x24 */
	uint32_t cpacr;      /* +0x28 */
	uint32_t fpexc;      /* +0x2C */
	uint32_t checksum;   /* +0x30 */
	uint32_t sram1_word; /* +0x34 -- legacy stage-1 field name; this was the original
	                       * "does 0x02400000 data-abort" probe. Stage 2 now maps and uses
	                       * SRAM1 for real, so this field's abort-or-not result is superseded
	                       * by the fact stage 2 gets there at all -- kept for continuity with
	                       * the stage-1 report, not removed. */
	uint32_t reserved0;  /* +0x38 pad, spec table jumps straight from +0x34 to +0x40 */
	uint32_t reserved1;  /* +0x3C pad */
	test_entry_t tests[MAX_TESTS]; /* +0x40..0x2C0. Stage-1 used indices 0-16 (3 sentinels +
	                                 * 14 throughput tests); stage 2 uses 17-24 for the 4
	                                 * dual-core fill/clean/read rounds x 2 cores. */

	/* ---- stage 2 additions, appended after the stage-1 layout (0x2C0..) ---- */
	uint32_t pmu_available;      /* +0x2C0: 1 if the PMCR/PMCNTENSET/PMCCNTR sequence didn't
	                               * fault/undef (NS access to the PMU can be blocked by
	                               * SDCR/MDCR), else 0 -- see pmu_try_read() in start.S */
	uint32_t pmu_cycles;         /* +0x2C4: PMCCNTR delta over the measurement window */
	uint32_t pmu_cntvct_ticks;   /* +0x2C8: actual CNTVCT delta measured (target: 10,000,000) */

	uint32_t psci_cpu_on_ret;    /* +0x2CC: PSCI CPU_ON SMC return code (r0) -- 0=SUCCESS,
	                               * see decode.py PSCI_NAMES for the negative PSCI_E_* values */
	uint32_t secondary_mpidr;    /* +0x2D0: MPIDR read back BY core1 in secondary_entry, proof
	                               * it's actually running our code; 0 if it never got here */

	uint32_t secondary_ready;    /* +0x2D4: barrier flag, core1->core0: MMU/NEON done, MPIDR
	                               * recorded, about to wait on dual_go[0] */
	uint32_t dual_go[6];         /* +0x2D8..: core0->core1, one per dual-core round:
	                               * 0=WB-S1 fill, 1=WB-S1 clean, 2=NC fill, 3=WB-S1 read,
	                               * 4=LDREX/STREX stress, 5=cross-L1 coherency */
	uint32_t dual_done[6];       /* +0x2F0..: core1->core0, same round order */
	uint32_t secondary_parked;   /* +0x308: core1->core0: all rounds done, about to jump to its
	                               * SRAM1 park loop (0x02402000) */

	uint32_t cdc_reg0[3];        /* +0x30C: first read of {0x49031134, 0x49031024, POS_STAT
	                               * 0x49031044} (CDC_L1_CFB_ADDR, CDC_SRCTRL, CDC_POS_STAT --
	                               * offsets from alp-sdk-lcd's display_cdc200.h) */
	uint32_t cdc_fault0[3];      /* +0x318: bit0 = synchronous fault (probe_read32 caught a data
	                               * abort, non-fatal); bit1 = ISR.A was set right after the read
	                               * (an asynchronous/external abort was pending -- CPSR.A is
	                               * masked for this whole image and NS can't unmask it per
	                               * SCR.AW=0, so an async abort from an unclocked/unpowered
	                               * CDC200 would otherwise show up as a garbage value with
	                               * bit0==0, indistinguishable from success) */
	uint32_t cdc_reg1[3];        /* +0x324: second read, ~1 ms later (CNTVCT-timed) -- lets
	                               * decode.py show whether POS_STAT is actually moving */
	uint32_t cdc_fault1[3];      /* +0x330 */

	uint32_t ldrex_final_value;  /* +0x33C: shared counter after both cores each do 1,000,000
	                               * LDREX/STREX increments concurrently -- expect exactly
	                               * 2,000,000; anything else means the exclusive monitor isn't
	                               * arbitrating correctly across the two cores */
	uint32_t ldrex_core0_ticks;  /* +0x340 */
	uint32_t ldrex_core1_ticks;  /* +0x344 */

	uint32_t coh_actual_checksum; /* +0x348: core1's XOR-checksum of the 64 KiB pattern core0
	                                * wrote and published with dmb ish -- decode.py recomputes
	                                * the expected value using coh_seed below (same
	                                * i*FILL_MUL formula, but seed + ... not FILL_BASE ^ ...,
	                                * see main.c's coherency_round) */
	uint32_t coh_seed;            /* +0x34C: per-run seed (from CNTVCT) folded into the pattern
	                                * core0 writes -- SRAM1 is never zeroed and survives a warm
	                                * reset, so a fixed pattern could make a stale buffer from a
	                                * PREVIOUS run's coherency test coincidentally checksum-match
	                                * even if THIS run's publish/read handshake is broken. A
	                                * fresh seed each run defeats that; decode.py reads this back
	                                * and recomputes with it rather than trusting a constant. */

	uint32_t mailbox_sentinel_written; /* +0x350: 1 once main() has written the 0x54524D42
	                                     * sentinel to SRAM1 0x02401000 */

	/* ---- internal fault-recovery handshake (start.S <-> the probe_read32 /
	 * pmu_try_read helpers); not a "result" a reader should interpret on its
	 * own, documented here only because it lives in the same block. Guarded
	 * to core0 only in record_fault (an MPIDR check), not just by which
	 * function happens to arm it -- see IMPLEMENTATION-NOTES. */
	uint32_t probe_recovery_pc;  /* +0x354 */
	uint32_t probe_faulted;      /* +0x358 */
} results_t;

/* Live heartbeat words are NOT in this block -- they're in the two park
 * loops' own SRAM, so they keep advancing after the results snapshot is
 * done being written (that's the whole point, addendum item D). Poll
 * these directly, not a results_t field (offset +0x18 within each copy of
 * start.S's heartbeat_park_template -- 6 instruction words before the
 * data word: wfi/adr/ldr/add/str/b, then .word heartbeat):
 *   core0: PARK_LOOP_SRAM + 0x18  == 0x023FE018
 *   core1: CORE1_PARK_ADDR + 0x18 == 0x02402018
 */

_Static_assert(offsetof(results_t, magic) == 0x00, "magic offset");
_Static_assert(offsetof(results_t, stage) == 0x04, "stage offset");
_Static_assert(offsetof(results_t, fault_code) == 0x08, "fault_code offset");
_Static_assert(offsetof(results_t, dfsr) == 0x0C, "dfsr offset");
_Static_assert(offsetof(results_t, dfar) == 0x10, "dfar offset");
_Static_assert(offsetof(results_t, ifsr) == 0x14, "ifsr offset");
_Static_assert(offsetof(results_t, ifar) == 0x18, "ifar offset");
_Static_assert(offsetof(results_t, fault_lr) == 0x1C, "fault_lr offset");
_Static_assert(offsetof(results_t, cntfrq) == 0x20, "cntfrq offset");
_Static_assert(offsetof(results_t, sctlr) == 0x24, "sctlr offset");
_Static_assert(offsetof(results_t, cpacr) == 0x28, "cpacr offset");
_Static_assert(offsetof(results_t, fpexc) == 0x2C, "fpexc offset");
_Static_assert(offsetof(results_t, checksum) == 0x30, "checksum offset");
_Static_assert(offsetof(results_t, sram1_word) == 0x34, "sram1_word offset");
_Static_assert(offsetof(results_t, tests) == 0x40, "tests offset");
_Static_assert(sizeof(test_entry_t) == 20, "test_entry_t size");
_Static_assert(offsetof(results_t, pmu_available) == 0x2C0, "pmu_available offset");
_Static_assert(offsetof(results_t, pmu_cycles) == 0x2C4, "pmu_cycles offset");
_Static_assert(offsetof(results_t, pmu_cntvct_ticks) == 0x2C8, "pmu_cntvct_ticks offset");
_Static_assert(offsetof(results_t, psci_cpu_on_ret) == 0x2CC, "psci_cpu_on_ret offset");
_Static_assert(offsetof(results_t, secondary_mpidr) == 0x2D0, "secondary_mpidr offset");
_Static_assert(offsetof(results_t, secondary_ready) == 0x2D4, "secondary_ready offset");
_Static_assert(offsetof(results_t, dual_go) == 0x2D8, "dual_go offset");
_Static_assert(offsetof(results_t, dual_done) == 0x2F0, "dual_done offset");
_Static_assert(offsetof(results_t, secondary_parked) == 0x308, "secondary_parked offset");
_Static_assert(offsetof(results_t, cdc_reg0) == 0x30C, "cdc_reg0 offset");
_Static_assert(offsetof(results_t, cdc_fault0) == 0x318, "cdc_fault0 offset");
_Static_assert(offsetof(results_t, cdc_reg1) == 0x324, "cdc_reg1 offset");
_Static_assert(offsetof(results_t, cdc_fault1) == 0x330, "cdc_fault1 offset");
_Static_assert(offsetof(results_t, ldrex_final_value) == 0x33C, "ldrex_final_value offset");
_Static_assert(offsetof(results_t, ldrex_core0_ticks) == 0x340, "ldrex_core0_ticks offset");
_Static_assert(offsetof(results_t, ldrex_core1_ticks) == 0x344, "ldrex_core1_ticks offset");
_Static_assert(offsetof(results_t, coh_actual_checksum) == 0x348, "coh_actual_checksum offset");
_Static_assert(offsetof(results_t, coh_seed) == 0x34C, "coh_seed offset");
_Static_assert(offsetof(results_t, mailbox_sentinel_written) == 0x350, "mailbox_sentinel_written offset");
_Static_assert(offsetof(results_t, probe_recovery_pc) == 0x354, "probe_recovery_pc offset");
_Static_assert(offsetof(results_t, probe_faulted) == 0x358, "probe_faulted offset");
_Static_assert(sizeof(results_t) == 0x35C, "results_t total size");

#endif /* A32_PROBE_RESULTS_H */
