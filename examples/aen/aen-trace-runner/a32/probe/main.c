/* main.c -- A32 memory/NEON throughput tests.
 *
 * Called by start.S once the MMU, caches and NEON are enabled, with the
 * results header already zeroed/stamped in SRAM. All state here lives on
 * the stack or in the results block in SRAM -- there is no global/static
 * mutable variable anywhere in this file, because the image is XIP from
 * read-only MRAM (see link.ld: it hard-fails the build if .data/.bss are
 * non-empty). Every "computed once" value below (e.g. CTR cache-line size)
 * is therefore recomputed locally where it's needed instead of cached in
 * a static.
 *
 * See docs/2026-09-22-a32-probe-spec.md, "main.c tests", for the exact
 * test matrix and results-block field semantics, and
 * IMPLEMENTATION-NOTES.md for why the fill pattern and checksum sampling
 * are shaped the way they are (review finding: a uniform fill pattern
 * makes the read-test's XOR checksum structurally 0 for our power-of-2
 * word counts, for real data AND for a silently-dropped write alike).
 */
#include <arm_neon.h>
#include <stdint.h>

#include "results.h"

#define NC_BUF_ADDR 0x02000000u
#define WB_BUF_ADDR 0x02100000u
#define NC_DST_ADDR 0x02200000u
#define SRAM1_ADDR  0x02400000u

#define ONE_MIB_WORDS     (1024u * 1024u / 4u)
#define SIXTEEN_KIB_WORDS (16u * 1024u / 4u)

/* Timed-pass counts. The spec asks for "one untimed warm pass, then N
 * timed passes" but leaves N to the implementation; these push enough
 * bytes through each buffer size to get many thousands of CNTVCT ticks
 * (100 MHz) of resolution without the whole probe run taking too long. */
#define PASSES_1MIB        16u
#define PASSES_16KIB       512u
#define PASSES_CLEAN_1MIB  8u
#define PASSES_CLEAN_16KIB 128u

/* ------------------------------------------------------------------------
 * Stage 2 -- SRAM1 (0x02400000-0x027FFFFF), mapped in start.S:
 *   0x024xxxxx  Normal NC, executable  -- mailbox + core1 park/heartbeat
 *   0x025xxxxx  Normal WB-WA, S=1      -- dual-core fill/clean/read buffer
 *   0x026xxxxx  Normal WB-WA, S=1      -- LDREX/STREX counter + coherency buf
 *   0x027xxxxx  UNMAPPED (Device/XN default) -- TF-A RW lives at
 *               0x027DE000-0x027ED000; nothing in this file ever
 *               references a 0x027xxxxx address.
 * ---------------------------------------------------------------------- */
#define SRAM1_MAILBOX_ADDR 0x02401000u
#define MAILBOX_SENTINEL   0x54524D42u /* "TRMB" -- addendum item E */

#define DUAL_WB_S1_ADDR \
	0x02500000u /* 1 MiB, Inner Shareable -- addendum item A: dual-core
                                         * fill/clean/read use THIS buffer, not the stage-1
                                         * WB_BUF_ADDR (S=0), so stage-1's single-core WB numbers
                                         * stay comparable to what was already measured on silicon */
#define LDREX_COUNTER_ADDR 0x02600000u
#define COH_BUF_ADDR       0x02601000u /* 64 KiB, addendum item C */
#define COH_BUF_WORDS      (64u * 1024u / 4u)
#define COH_FLAG_ADDR      0x02611000u

#define PASSES_DUAL          8u
#define PASSES_DUAL_CLEAN    4u
#define LDREX_ITERS_PER_CORE 1000000u /* addendum item B: expect final value == 2x this */

#define CORE0_DUAL_BASE_IDX 17u /* tests[] indices 17-20: core0's 4 fill/clean/read rounds */
#define CORE1_DUAL_BASE_IDX 21u /* tests[] indices 21-24: core1's */

/* How long core0 waits for core1 at each handshake before giving up and
 * reporting rather than hanging forever. If core1 dies mid-sequence (e.g.
 * a fatal fault during its own MMU/NEON bring-up, which never sets
 * secondary_ready), core0 must still finish the rest of the probe (CDC
 * reads, mailbox sentinel, stage=0xD0E, park) instead of spinning forever
 * on a flag that will never be set -- that would turn "core1 had a
 * problem" into "the whole probe looks wedged" for the bench. ~1 s @ 100
 * MHz: generous for what should be a microseconds-scale handshake. */
#define BARRIER_TIMEOUT_TICKS 100000000ull

extern uint64_t probe_read32(uint32_t addr); /* start.S: r0=value(0 if fault), r1=fault(0/1) */
extern uint64_t pmu_try_read(void);          /* start.S: r0=PMCCNTR(0 if unavail), r1=avail */
extern uint32_t psci_cpu_on_secondary(void); /* start.S: PSCI CPU_ON, returns SMC ret code */
extern void atomic_increment_n(volatile uint32_t *addr, uint32_t count); /* start.S: LDREX/STREX */
void        secondary_main(void); /* called from start.S's secondary_entry via `bl`; not static */

/* Fill pattern for scalar_fill: word[i] = SCALAR_BASE ^ i ^ pass, varies
 * across passes just so consecutive passes don't write bit-identical data
 * (not load-bearing for correctness -- see neon_fill below for the pattern
 * that actually matters). */
#define SCALAR_BASE 0xA5A5A5A5u

/* Fill pattern for neon_fill: word[i] = FILL_BASE ^ (i * FILL_MUL).
 *
 * This is NOT cosmetic. neon_read_xor() XOR-reduces the whole buffer, and
 * for our word counts (262144 and 4096 -- both powers of 2, >=4) XOR-
 * reducing ANY single repeated value, or ANY plain arithmetic index
 * 0,1,2,...,N-1, is a well-known 0: each output bit is toggled by exactly
 * half the N inputs, so they cancel in pairs regardless of what the value
 * or the base is. That is true for CORRECT data and for a silently-
 * dropped-write (reads back all 0) alike -- the old constant-fill checksum
 * could not tell them apart. Multiplying the index by an odd
 * "multiplicative hash" constant before XOR-ing in the base breaks that
 * cancellation (verified for both our exact word counts by direct
 * computation, not just argued -- see IMPLEMENTATION-NOTES.md): the result
 * is a fixed, known-nonzero value per buffer size that decode.py can
 * recompute independently and compare against the captured checksum. */
#define FILL_BASE 0xA5A5A5A5u
#define FILL_MUL  0x9E3779B1u /* Knuth/Fibonacci multiplicative-hash constant (odd) */

/* Sentinel: a single word written and immediately read back, per buffer
 * region, BEFORE any throughput test touches that region. This is the
 * primary, mathematically-unambiguous defence against a RAZ/WI firewall
 * (NS store silently dropped, reads back 0) or any other silently-dropped
 * write -- one word, one compare, no XOR-reduction math to get subtly
 * wrong. Recorded as its own test_id (op=6, beyond the base spec's op
 * 1-5) so decode.py can report PASS/FAIL directly instead of a checksum
 * inference. */
#define SENTINEL_PATTERN 0xC3A5C3A5u

static inline uint64_t read_cntvct(void)
{
	uint32_t lo, hi;
	/* isb: an uncommented CNTVCT read can be reordered by the core ahead
	 * of the instructions it's meant to be timing; isb forces everything
	 * before it to have retired first. "memory" clobber: stops the
	 * compiler itself reordering the preceding buffer stores/loads past
	 * this call (review finding F3). */
	__asm__ volatile("isb\n\t"
	                 "mrrc p15, 1, %0, %1, c14"
	                 : "=r"(lo), "=r"(hi)::"memory");
	return ((uint64_t)hi << 32) | lo;
}

static void scalar_fill(uint32_t *buf, uint32_t words, uint32_t base)
{
	for (uint32_t i = 0; i < words; i++)
		buf[i] = base ^ i;
}

/* word[i] = FILL_BASE ^ (i * FILL_MUL), computed incrementally: maintain
 * "i * FILL_MUL" as a running vector and ADD 4*FILL_MUL per store (4 words
 * per vst1q_u32) instead of multiplying fresh each time -- (i+1)*M =
 * i*M + M holds exactly, so this is not an approximation, just the cheap
 * way to compute the same sequence NEON's multiply-free ALU likes better. */
static void neon_fill(uint32_t *buf, uint32_t words)
{
	const uint32x4_t basev = vdupq_n_u32(FILL_BASE);
	const uint32x4_t step4 = vdupq_n_u32(4u * FILL_MUL);
	uint32x4_t im  = { 0u, FILL_MUL, 2u * FILL_MUL, 3u * FILL_MUL }; /* i*MUL for i={0,1,2,3} */
	uint32_t  *p   = buf;
	uint32_t  *end = buf + words; /* words is a multiple of 16 for every buffer we use */
	while (p < end) {
		vst1q_u32(p + 0, veorq_u32(basev, im)); /* 4 x vst1q_u32 = 4 x 16 B = 64 B per iteration */
		im = vaddq_u32(im, step4);
		vst1q_u32(p + 4, veorq_u32(basev, im));
		im = vaddq_u32(im, step4);
		vst1q_u32(p + 8, veorq_u32(basev, im));
		im = vaddq_u32(im, step4);
		vst1q_u32(p + 12, veorq_u32(basev, im));
		im = vaddq_u32(im, step4);
		p += 16;
	}
}

static uint32_t neon_read_xor(const uint32_t *buf, uint32_t words)
{
	uint32x4_t      acc = vdupq_n_u32(0);
	const uint32_t *p   = buf;
	const uint32_t *end = buf + words;
	while (p < end) {
		acc = veorq_u32(acc, vld1q_u32(p));
		p += 4;
	}
	uint32_t lanes[4];
	vst1q_u32(lanes, acc);
	return lanes[0] ^ lanes[1] ^ lanes[2] ^ lanes[3];
}

static void neon_copy(const uint32_t *src, uint32_t *dst, uint32_t words)
{
	for (uint32_t i = 0; i < words; i += 4)
		vst1q_u32(dst + i, vld1q_u32(src + i));
}

/* DCCMVAC clean-by-VA loop, one cache line at a time, +DSB. Cache line size
 * comes from CTR (bits[19:16], DminLine) read fresh each call rather than
 * cached in a static -- see file header. */
static void clean_dcache_range(const void *start, uint32_t size)
{
	uint32_t ctr;
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
	uint32_t line = 4u << ((ctr >> 16) & 0xFu); /* CTR.DminLine: line = 4 << DminLine bytes */

	uintptr_t addr = (uintptr_t)start & ~(uintptr_t)(line - 1);
	uintptr_t end  = (uintptr_t)start + size;
	for (; addr < end; addr += line) {
		uint32_t a = (uint32_t)addr;
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(a) : "memory"); /* DCCMVAC */
	}
	__asm__ volatile("dsb" ::: "memory");
}

static void
record(volatile results_t *r, uint32_t idx, uint32_t test_id, uint64_t bytes, uint64_t ticks)
{
	r->tests[idx].test_id  = test_id;
	r->tests[idx].bytes_lo = (uint32_t)bytes;
	r->tests[idx].bytes_hi = (uint32_t)(bytes >> 32);
	r->tests[idx].ticks_lo = (uint32_t)ticks;
	r->tests[idx].ticks_hi = (uint32_t)(ticks >> 32);
}

/* Write SENTINEL_PATTERN to *addr, DSB, read it back, record expected/
 * actual/pass-fail as one test entry (test_id op=6, "sentinel"). This is
 * deliberately independent of every other test's data/pattern choices --
 * it exists specifically to catch a silently-dropped NS write (RAZ/WI
 * firewall) that no amount of clever XOR-checksum math over a power-of-2
 * word count can be trusted to catch (see the FILL_MUL comment above). */
static uint32_t sentinel_check(volatile results_t *r, uint32_t idx, uint32_t buf_id, uint32_t *addr)
{
	*addr = SENTINEL_PATTERN;
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t readback = *(volatile uint32_t *)addr;

	r->tests[idx].test_id  = TEST_ID(buf_id, OP_SENTINEL);
	r->tests[idx].bytes_lo = SENTINEL_PATTERN;                         /* expected */
	r->tests[idx].bytes_hi = (readback == SENTINEL_PATTERN) ? 1u : 0u; /* pass flag */
	r->tests[idx].ticks_lo = readback;                                 /* actual */
	r->tests[idx].ticks_hi = 0u;
	return idx + 1;
}

/* ==========================================================================
 * Stage 2 helpers.
 * ======================================================================== */

static inline void barrier_signal(volatile uint32_t *flag)
{
	__asm__ volatile("dsb sy" ::: "memory"); /* publish everything written before the flag */
	*flag = 1;
	__asm__ volatile("dsb sy" ::: "memory"); /* make sure the flag write itself is out */
	__asm__ volatile("sev");                 /* wake the other core if it's WFE-waiting */
}

static inline void barrier_wait(volatile uint32_t *flag)
{
	while (*flag == 0)
		__asm__ volatile("wfe"); /* re-checks the condition on any wake, spurious or not */
	__asm__ volatile("dmb sy" ::
	                     : "memory"); /* order our reads of the guarded data after the flag */
}

/* Same as barrier_wait but gives up after max_ticks of CNTVCT (see
 * BARRIER_TIMEOUT_TICKS). Returns 1 if the flag was observed set in time,
 * 0 on timeout (flag left unset). */
static int barrier_wait_timeout(volatile uint32_t *flag, uint64_t max_ticks)
{
	uint64_t start = read_cntvct();
	while (*flag == 0) {
		if ((read_cntvct() - start) > max_ticks) return 0;
		__asm__ volatile("wfe");
	}
	__asm__ volatile("dmb sy" ::: "memory");
	return 1;
}

/* Task 1: enable + read PMCCNTR over a CNTVCT window of exactly 10,000,000
 * ticks (100 ms @ 100 MHz), computed via pmu_try_read() so an undef (NS
 * access to the PMU blocked by SDCR/MDCR) is recorded rather than guessed
 * at or treated as fatal. */
static void pmu_clock_test(volatile results_t *r)
{
	uint64_t s0     = pmu_try_read();
	uint32_t avail0 = (uint32_t)(s0 >> 32);
	uint32_t pmcc0  = (uint32_t)s0;

	if (!avail0) {
		r->pmu_available    = 0;
		r->pmu_cycles       = 0;
		r->pmu_cntvct_ticks = 0;
		return;
	}

	uint64_t v0 = read_cntvct();
	uint64_t v1;
	do {
		v1 = read_cntvct();
	} while ((v1 - v0) < 10000000ull); /* subtraction, not `v1 < v0+N` (review item 13): correct
					     * by construction even across a counter wrap, not just in
					     * the (here, practically impossible) common case */

	uint64_t s1     = pmu_try_read();
	uint32_t avail1 = (uint32_t)(s1 >> 32);
	uint32_t pmcc1  = (uint32_t)s1;

	r->pmu_available = avail1 ? 1u : 0u;
	r->pmu_cycles    = pmcc1 - pmcc0; /* PMCCNTR is 32-bit; wraps only after ~1 s even near 4 GHz,
	                                 * safe for a 100 ms window at any plausible clock here */
	r->pmu_cntvct_ticks = (uint32_t)(v1 - v0); /* actual measured window, target 10,000,000 */
}

/* Task 4 / addendum F: {L1CFB, SRCTRL, POS_STAT} from alp-sdk-lcd's
 * display_cdc200.h (CDC_L1_CFB_ADDR=0x134, CDC_SRCTRL=0x24, CDC_POS_STAT=
 * 0x44, all off CDC base 0x49031000), read twice ~1 ms apart so decode.py
 * can show whether POS_STAT (current scan position) is actually moving.
 * probe_read32() makes each SYNCHRONOUS fault non-fatal, but that's not
 * the whole risk here (review item 5, MAJOR): CPSR.A is masked for the
 * entire life of this image and NS can't unmask it (SCR.AW=0), so an
 * ASYNCHRONOUS/external abort from an unclocked or unpowered CDC200 --
 * plausible, this is explicitly the register set the T-A0 plan doc calls
 * "NS access ... unknown" -- would never reach data_abort_handler at all.
 * Worse, the load itself might simply never complete (bus hang), and
 * without a progress marker that shows up as an unexplained wedge at
 * stage 0x5A1 (the LAST marker before this ran). Mitigated three ways:
 * this is now the LAST thing main() does (after the mailbox sentinel, so
 * everything else is already on record even if this hangs); r->stage is
 * set to 0xCDC0+i before each individual read; and ISR.A (external abort
 * pending) is checked right after every read and folded into the fault
 * bitfield (bit0 = probe_read32 caught a synchronous fault, bit1 = ISR.A
 * was set -- see the results.h comment). */
static const uint32_t cdc_addrs[3] = { 0x49031134u, 0x49031024u, 0x49031044u };

static inline uint32_t read_isr_async_abort_pending(void)
{
	uint32_t isr;
	__asm__ volatile("mrc p15, 0, %0, c12, c1, 0" : "=r"(isr)); /* ISR: bit8 = A */
	return (isr >> 8) & 1u;
}

static void
cdc_read_set(volatile results_t *r, volatile uint32_t *out_reg, volatile uint32_t *out_fault)
{
	for (int i = 0; i < 3; i++) {
		r->stage               = 0xCDC0u + (uint32_t)i;
		uint64_t res           = probe_read32(cdc_addrs[i]);
		uint32_t sync_fault    = (uint32_t)(res >> 32);
		uint32_t async_pending = read_isr_async_abort_pending();
		out_reg[i]             = (uint32_t)res;
		out_fault[i]           = sync_fault | (async_pending << 1);
	}
}

static void cdc_probe(volatile results_t *r)
{
	cdc_read_set(r, r->cdc_reg0, r->cdc_fault0);

	uint64_t start = read_cntvct();
	while ((read_cntvct() - start) < 100000ull) {
	} /* ~1 ms @ 100 MHz; subtraction form, review
							   * item 13 */

	cdc_read_set(r, r->cdc_reg1, r->cdc_fault1);
}

/* Dual-core round 0/2: both cores NEON-fill their own half of *buf
 * concurrently. is_core0 selects which side of the go/done handshake this
 * call plays; both cores call the SAME function with the SAME round index
 * so there's exactly one place this logic is written. Returns 1 normally;
 * for core0 only, 0 if core1 never signalled done (see
 * BARRIER_TIMEOUT_TICKS) -- core1's own call always returns 1. */
static int dual_fill_round(volatile results_t *r,
                           uint32_t            round,
                           int                 is_core0,
                           uint32_t           *buf,
                           uint32_t            words,
                           uint32_t            buf_id,
                           uint32_t            passes,
                           uint32_t            out_idx)
{
	if (is_core0) {
		barrier_signal(&r->dual_go[round]);
	} else if (!barrier_wait_timeout(&r->dual_go[round], BARRIER_TIMEOUT_TICKS)) {
		return 0; /* core0 already bailed on an earlier round and will never signal this
			   * one -- give up and let secondary_main/secondary_entry fall through to
			   * park instead of waiting here forever (review item 3) */
	}

	neon_fill(buf, words); /* untimed warm pass */
	uint64_t t0 = read_cntvct();
	for (uint32_t p = 0; p < passes; p++)
		neon_fill(buf, words);
	__asm__ volatile("dsb sy" ::: "memory"); /* drain stores before the end-time snapshot (F3) */
	uint64_t t1 = read_cntvct();

	record(r,
	       out_idx,
	       TEST_ID_CORE(is_core0 ? 0u : 1u, buf_id, OP_DUAL_FILL),
	       (uint64_t)words * 4u * passes,
	       t1 - t0);

	if (is_core0) return barrier_wait_timeout(&r->dual_done[round], BARRIER_TIMEOUT_TICKS);
	barrier_signal(&r->dual_done[round]);
	return 1;
}

/* Dual-core round 1: each core cleans its OWN half after re-dirtying it.
 * DCCMVAC is by-VA, not by-set/way, so two cores doing this concurrently on
 * DISJOINT ranges is architecturally well-defined (unlike the set/way
 * invalidate in start.S, which is exactly why THAT one is core0-only --
 * see secondary_entry's comment). */
static int dual_clean_round(volatile results_t *r,
                            uint32_t            round,
                            int                 is_core0,
                            uint32_t           *buf,
                            uint32_t            words,
                            uint32_t            buf_id,
                            uint32_t            passes,
                            uint32_t            out_idx)
{
	if (is_core0) {
		barrier_signal(&r->dual_go[round]);
	} else if (!barrier_wait_timeout(&r->dual_go[round], BARRIER_TIMEOUT_TICKS)) {
		return 0;
	}

	neon_fill(buf, words);
	clean_dcache_range(buf, words * 4u); /* untimed warm pass */
	uint64_t ticks = 0;
	for (uint32_t p = 0; p < passes; p++) {
		neon_fill(buf, words);
		uint64_t c0 = read_cntvct();
		clean_dcache_range(buf, words * 4u);
		uint64_t c1 = read_cntvct();
		ticks += (c1 - c0);
	}

	record(r,
	       out_idx,
	       TEST_ID_CORE(is_core0 ? 0u : 1u, buf_id, OP_DUAL_CLEAN),
	       (uint64_t)words * 4u * passes,
	       ticks);

	if (is_core0) return barrier_wait_timeout(&r->dual_done[round], BARRIER_TIMEOUT_TICKS);
	barrier_signal(&r->dual_done[round]);
	return 1;
}

/* Dual-core round 3: each core NEON-reads its own half. */
static int dual_read_round(volatile results_t *r,
                           uint32_t            round,
                           int                 is_core0,
                           uint32_t           *buf,
                           uint32_t            words,
                           uint32_t            buf_id,
                           uint32_t            passes,
                           uint32_t            out_idx)
{
	if (is_core0) {
		barrier_signal(&r->dual_go[round]);
	} else if (!barrier_wait_timeout(&r->dual_go[round], BARRIER_TIMEOUT_TICKS)) {
		return 0;
	}

	(void)neon_read_xor(buf, words); /* untimed warm pass */
	uint64_t t0 = read_cntvct();
	for (uint32_t p = 0; p < passes; p++)
		(void)neon_read_xor(buf, words);
	uint64_t t1 = read_cntvct();

	record(r,
	       out_idx,
	       TEST_ID_CORE(is_core0 ? 0u : 1u, buf_id, OP_DUAL_READ),
	       (uint64_t)words * 4u * passes,
	       t1 - t0);

	if (is_core0) return barrier_wait_timeout(&r->dual_done[round], BARRIER_TIMEOUT_TICKS);
	barrier_signal(&r->dual_done[round]);
	return 1;
}

/* Addendum item B: both cores each do LDREX_ITERS_PER_CORE increments of
 * the SAME shared word concurrently; a correct global exclusive monitor
 * across the two cores gives exactly 2x that as the final value. Core0
 * initialises the counter to 0 and PUBLISHES that via barrier_signal()
 * (dsb before the go flag) BEFORE core1's barrier_wait() can return (dmb
 * after observing the flag) -- so core1 can never see a stale/garbage
 * counter value. Getting this ordering backwards would make a working
 * exclusive monitor look broken (final value off by whatever core1 added
 * to leftover SRAM garbage before core0's zero was visible) -- a bug in
 * the TEST, not the hardware, which is exactly the kind of false result
 * this ordering exists to avoid. */
static int ldrex_round(volatile results_t *r, int is_core0)
{
	volatile uint32_t *counter = (volatile uint32_t *)LDREX_COUNTER_ADDR;

	if (is_core0) {
		*counter = 0; /* init BEFORE publishing go[4] -- see comment above */
		barrier_signal(&r->dual_go[4]);
	} else if (!barrier_wait_timeout(&r->dual_go[4], BARRIER_TIMEOUT_TICKS)) {
		return 0;
	}

	uint64_t t0 = read_cntvct();
	atomic_increment_n(counter, LDREX_ITERS_PER_CORE);
	uint64_t t1 = read_cntvct();

	if (is_core0)
		r->ldrex_core0_ticks = (uint32_t)(t1 - t0);
	else
		r->ldrex_core1_ticks = (uint32_t)(t1 - t0);

	if (is_core0) {
		if (!barrier_wait_timeout(&r->dual_done[4], BARRIER_TIMEOUT_TICKS)) return 0;
		r->ldrex_final_value = *counter;
		return 1;
	}
	barrier_signal(&r->dual_done[4]);
	return 1;
}

/* Waits for *flag == value, WFE-polling, bounded by max_ticks of CNTVCT.
 * Same shape as barrier_wait_timeout but for a multi-state flag rather
 * than a plain 0/1 one -- used by coherency_round's 3-state handshake. */
static int wait_flag_eq_timeout(volatile uint32_t *flag, uint32_t value, uint64_t max_ticks)
{
	uint64_t start = read_cntvct();
	while (*flag != value) {
		if ((read_cntvct() - start) > max_ticks) return 0;
		__asm__ volatile("wfe");
	}
	__asm__ volatile("dmb sy" ::: "memory");
	return 1;
}

/* Addendum item C, reworked per review item 6 (MAJOR -- the original
 * version could pass falsely). Two problems with the first cut:
 *
 *   1. COH_FLAG_ADDR was never cleared, and SRAM1 is never zeroed and
 *      survives a warm reset (the image can be resident across runs). On a
 *      rerun, core1 could see a STALE flag=1 left over from a PREVIOUS
 *      run and read immediately -- and since the buffer already held that
 *      previous run's pattern, the checksum would still "match" even
 *      though THIS run's publish/read handshake never actually happened.
 *   2. core1's first-ever read of the buffer was also the ONLY read --
 *      nothing had put the old content in core1's L1 first, so a
 *      successful checksum only proved core1 could read memory, not that
 *      an already-cached line gets correctly invalidated/updated by the
 *      coherency fabric when the other core writes it.
 *
 * Fixed with a 3-state COH_FLAG_ADDR protocol instead of a single 0/1:
 *   0 = idle/cleared (core0 clears it BEFORE signalling dual_go[5], so a
 *       stale flag from a previous run can never survive into this one)
 *   1 = "core1 has primed its L1 (read the OLD content once) and is
 *       waiting for core0 to publish" -- core0 only writes after seeing
 *       this, so the priming read is guaranteed to happen before the
 *       write it's meant to race against
 *   2 = "core0 has published a freshly-seeded pattern" -- core1 only
 *       reads (for the real, checked checksum) after seeing this
 *
 * The pattern itself is also per-run-seeded (from CNTVCT, recorded in
 * coh_seed) rather than the fixed FILL_BASE -- belt and suspenders against
 * the same staleness concern: even if the flag protocol somehow let a
 * stale buffer through, a fresh seed each run makes the bytes actually
 * different, so a stale match becomes a checksum MISMATCH (caught) instead
 * of a silent false pass. decode.py reads coh_seed back and recomputes the
 * expected checksum with it (word[i] = seed + i*FILL_MUL, addition not
 * XOR specifically so the seed doesn't cancel out of the reduction the way
 * a top-level XOR would -- verified by direct computation, not assumed).
 *
 * Returns 1 normally; for core0, 0 if core1 never responds in time; for
 * core1, 0 if core0 never does. Last round, so run_dual_core_rounds
 * doesn't need to act on the return value, but it's still checked for
 * consistency with every other round. */
static int coherency_round(volatile results_t *r, int is_core0)
{
	volatile uint32_t *flag = (volatile uint32_t *)COH_FLAG_ADDR;

	if (is_core0) {
		*flag = 0; /* clear BEFORE go[5] -- see problem 1 above */
		__asm__ volatile("dsb sy" ::: "memory");
		barrier_signal(&r->dual_go[5]);
	} else if (!barrier_wait_timeout(&r->dual_go[5], BARRIER_TIMEOUT_TICKS)) {
		return 0;
	}

	if (is_core0) {
		if (!wait_flag_eq_timeout(flag, 1, BARRIER_TIMEOUT_TICKS)) return 0;

		uint32_t seed = (uint32_t)read_cntvct(); /* per-run seed, see comment above */
		r->coh_seed   = seed;
		uint32_t *buf = (uint32_t *)COH_BUF_ADDR;
		for (uint32_t i = 0; i < COH_BUF_WORDS; i++)
			buf[i] = seed + (i * FILL_MUL);
		__asm__ volatile("dmb ish" ::
		                     : "memory"); /* Inner Shareable, matching the S=1 attribute
							     * on this region (addendum item C says "dmb
							     * ish" specifically, not a plain dsb) */
		*flag = 2;
		__asm__ volatile("dsb sy" ::: "memory");
		__asm__ volatile("sev");

		return barrier_wait_timeout(&r->dual_done[5], BARRIER_TIMEOUT_TICKS);
	}

	/* core1: prime with the OLD content (problem 2 above), THEN tell
	 * core0 to go ahead and publish. */
	(void)neon_read_xor((const uint32_t *)COH_BUF_ADDR, COH_BUF_WORDS);
	__asm__ volatile("dsb sy" ::: "memory");
	*flag = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("sev");

	if (!wait_flag_eq_timeout(flag, 2, BARRIER_TIMEOUT_TICKS)) return 0;
	__asm__ volatile("dmb ish" ::: "memory"); /* pairs with core0's dmb ish above */

	r->coh_actual_checksum = neon_read_xor((const uint32_t *)COH_BUF_ADDR, COH_BUF_WORDS);

	barrier_signal(&r->dual_done[5]);
	return 1;
}

/* Runs all 6 dual-core rounds in the SAME fixed order on both cores (0=WB
 * fill, 1=WB clean, 2=NC fill, 3=WB read, 4=LDREX/STREX, 5=coherency).
 * EITHER side bails out early if the other stops responding within
 * BARRIER_TIMEOUT_TICKS (review item 3: this used to only be true for
 * core0 -- if core0 bailed, core1 would wait forever on a `dual_go[k]`
 * that would never come, stuck in secondary_main, never reaching its own
 * park). r->stage markers (core0 only, to avoid a pointless race on who
 * "wins" the write) record which round is in flight, so a wedge is
 * diagnosable by its last stage value rather than a bare guess. */
static void run_dual_core_rounds(volatile results_t *r, int is_core0)
{
	uint32_t  half     = ONE_MIB_WORDS / 2;
	uint32_t  base_idx = is_core0 ? CORE0_DUAL_BASE_IDX : CORE1_DUAL_BASE_IDX;
	uint32_t *wb_half  = (uint32_t *)(DUAL_WB_S1_ADDR + (is_core0 ? 0u : half * 4u));
	uint32_t *nc_half  = (uint32_t *)(NC_BUF_ADDR + (is_core0 ? 0u : half * 4u));

	if (is_core0) r->stage = 0xF010u;
	if (!dual_fill_round(r, 0, is_core0, wb_half, half, BUF_WB_S1, PASSES_DUAL, base_idx + 0))
		return;
	if (is_core0) r->stage = 0xF011u;
	if (!dual_clean_round(
	        r, 1, is_core0, wb_half, half, BUF_WB_S1, PASSES_DUAL_CLEAN, base_idx + 1))
		return;
	if (is_core0) r->stage = 0xF012u;
	if (!dual_fill_round(r, 2, is_core0, nc_half, half, BUF_NC, PASSES_DUAL, base_idx + 2)) return;
	if (is_core0) r->stage = 0xF013u;
	if (!dual_read_round(r, 3, is_core0, wb_half, half, BUF_WB_S1, PASSES_DUAL, base_idx + 3))
		return;
	if (is_core0) r->stage = 0xF014u;
	if (!ldrex_round(r, is_core0)) return;
	if (is_core0) r->stage = 0xF015u;
	(void)coherency_round(r, is_core0);
}

/* Runs on core 1 (called from secondary_entry in start.S once its own
 * MMU/NEON/MPIDR setup is done and secondary_ready is published). */
void secondary_main(void)
{
	volatile results_t *r = (volatile results_t *)RESULTS_BASE;
	run_dual_core_rounds(r, 0);
}

/* Runs the fill/read/copy quartet (and, for WB buffers, the clean test) on
 * one buffer/size combination. Returns the next free results index.
 * checksum is threaded through and re-published to the results block after
 * every single test so a fault mid-run still leaves the latest value
 * visible. */
static uint32_t run_buffer_tests(volatile results_t *r,
                                 uint32_t            idx,
                                 uint32_t           *checksum,
                                 uint32_t            buf_id,
                                 uint32_t           *buf,
                                 uint32_t            words,
                                 uint32_t            passes,
                                 int                 do_clean,
                                 uint32_t            passes_clean)
{
	uint64_t t0, t1, bytes;
	uint32_t p;

	/* scalar fill */
	r->stage = TEST_ID(buf_id, OP_SCALAR_FILL);
	scalar_fill(buf, words, SCALAR_BASE); /* untimed warm pass */
	t0 = read_cntvct();
	for (p = 0; p < passes; p++)
		scalar_fill(buf, words, SCALAR_BASE ^ p);
	__asm__ volatile("dsb sy" ::
	                     : "memory"); /* drain the stores before we snapshot end-time (F3):
						   * without this, a still-in-flight store wouldn't count
						   * against the measured interval, understating the time */
	t1    = read_cntvct();
	bytes = (uint64_t)words * 4u * passes;
	record(r, idx, TEST_ID(buf_id, OP_SCALAR_FILL), bytes, t1 - t0);
	*checksum ^= *(volatile uint32_t *)buf; /* prove the stores landed; can't be elided */
	r->checksum = *checksum;
	idx++;

	/* neon fill */
	r->stage = TEST_ID(buf_id, OP_NEON_FILL);
	neon_fill(buf, words);
	t0 = read_cntvct();
	for (p = 0; p < passes; p++)
		neon_fill(buf, words);
	__asm__ volatile("dsb sy" ::: "memory");
	t1    = read_cntvct();
	bytes = (uint64_t)words * 4u * passes;
	record(r, idx, TEST_ID(buf_id, OP_NEON_FILL), bytes, t1 - t0);
	*checksum ^= *(volatile uint32_t *)buf;
	r->checksum = *checksum;
	idx++;

	/* neon read, xor-accumulate. The buffer doesn't change between
	 * passes (nothing here writes it), so neon_read_xor() returns the
	 * SAME value every pass -- XOR-ing that into checksum once per pass
	 * would cancel to 0 for our even PASSES_* counts and add nothing for
	 * an odd count either way: pure noise, no information. Sample the
	 * (deterministic) result exactly once instead; the timed loop below
	 * exists purely to measure bandwidth. */
	r->stage = TEST_ID(buf_id, OP_NEON_READ);
	uint32_t read_sample =
	    neon_read_xor(buf, words); /* untimed warm pass; also the checksum sample */
	t0 = read_cntvct();
	for (p = 0; p < passes; p++)
		(void)neon_read_xor(buf, words);
	t1    = read_cntvct();
	bytes = (uint64_t)words * 4u * passes;
	record(r, idx, TEST_ID(buf_id, OP_NEON_READ), bytes, t1 - t0);
	*checksum ^= read_sample;
	r->checksum = *checksum;
	idx++;

	/* neon copy -> NC destination */
	uint32_t *dst = (uint32_t *)NC_DST_ADDR;
	r->stage      = TEST_ID(buf_id, OP_NEON_COPY);
	neon_copy(buf, dst, words); /* untimed warm pass */
	t0 = read_cntvct();
	for (p = 0; p < passes; p++)
		neon_copy(buf, dst, words);
	__asm__ volatile("dsb sy" ::: "memory");
	t1    = read_cntvct();
	bytes = (uint64_t)words * 4u * passes;
	record(r, idx, TEST_ID(buf_id, OP_NEON_COPY), bytes, t1 - t0);
	*checksum ^= *(volatile uint32_t *)dst;
	r->checksum = *checksum;
	idx++;

	if (do_clean) {
		/* Cost of DCCMVAC-cleaning the whole range, isolated from the
		 * fill that dirties it: each pass fills (untimed) then times
		 * only the clean. "bytes" records what was cleaned.
		 * clean_dcache_range() already ends with its own dsb, so the
		 * c1 = read_cntvct() capture below is already preceded by one
		 * (F3) without needing a second here. */
		r->stage = TEST_ID(buf_id, OP_CLEAN);
		neon_fill(buf, words);
		clean_dcache_range(buf, words * 4u); /* untimed warm pass */
		uint64_t clean_ticks = 0;
		for (p = 0; p < passes_clean; p++) {
			neon_fill(buf, words); /* re-dirty, not timed */
			uint64_t c0 = read_cntvct();
			clean_dcache_range(buf, words * 4u);
			uint64_t c1 = read_cntvct();
			clean_ticks += (c1 - c0);
		}
		bytes = (uint64_t)words * 4u * passes_clean;
		record(r, idx, TEST_ID(buf_id, OP_CLEAN), bytes, clean_ticks);
		*checksum ^= *(volatile uint32_t *)buf;
		r->checksum = *checksum;
		idx++;
	}

	return idx;
}

int main(void)
{
	volatile results_t *r        = (volatile results_t *)RESULTS_BASE;
	uint32_t            checksum = 0;
	uint32_t            idx      = 0;

	/* Sentinel write/readback per region, before anything else touches
	 * them -- see the SENTINEL_PATTERN comment above. */
	idx = sentinel_check(r, idx, BUF_NC, (uint32_t *)NC_BUF_ADDR);
	idx = sentinel_check(r, idx, BUF_WB_1MIB, (uint32_t *)WB_BUF_ADDR);
	idx = sentinel_check(r, idx, BUF_NC_DST, (uint32_t *)NC_DST_ADDR);

	idx = run_buffer_tests(
	    r, idx, &checksum, BUF_NC, (uint32_t *)NC_BUF_ADDR, ONE_MIB_WORDS, PASSES_1MIB, 0, 0);
	idx = run_buffer_tests(r,
	                       idx,
	                       &checksum,
	                       BUF_WB_1MIB,
	                       (uint32_t *)WB_BUF_ADDR,
	                       ONE_MIB_WORDS,
	                       PASSES_1MIB,
	                       1,
	                       PASSES_CLEAN_1MIB);
	idx = run_buffer_tests(r,
	                       idx,
	                       &checksum,
	                       BUF_WB_16KIB,
	                       (uint32_t *)WB_BUF_ADDR,
	                       SIXTEEN_KIB_WORDS,
	                       PASSES_16KIB,
	                       1,
	                       PASSES_CLEAN_16KIB);

	/* Single-word SRAM1 probe (stage 1's original test, unchanged code and
	 * meaning intact). If this data-aborts, data_abort_handler in start.S
	 * records fault_code/DFSR/DFAR/LR and parks -- main() never returns in
	 * that case. Note stage 2 now maps and actively uses SRAM1 (see
	 * results.h's sram1_word comment), so on a chain where stage 2's own
	 * SRAM1 mappings are live this read is expected to simply succeed
	 * rather than test "is SRAM1 mapped at all" the way it did in stage 1
	 * alone. */
	r->stage      = 0x5A1u;
	uint32_t v    = *(volatile uint32_t *)SRAM1_ADDR;
	r->sram1_word = v;

	(void)idx;

	/* ---- Stage 2 ---- */

	/* Task 1: core clock via the PMU cycle counter. */
	r->stage = 0xF000u;
	pmu_clock_test(r);

	/* Task 2: bring up core 1 via PSCI CPU_ON, then run the dual-core
	 * rounds (task 3 + addendum B/C) together with it. If PSCI itself
	 * fails, or core1 never signals ready within BARRIER_TIMEOUT_TICKS,
	 * skip straight to the rest of core0's own work -- see the
	 * BARRIER_TIMEOUT_TICKS comment for why this must not just hang. */
	r->stage           = 0xF001u;
	r->psci_cpu_on_ret = psci_cpu_on_secondary();
	if (r->psci_cpu_on_ret == 0 && barrier_wait_timeout(&r->secondary_ready, BARRIER_TIMEOUT_TICKS))
		run_dual_core_rounds(r, 1);

	/* Addendum item E: mailbox sentinel for the bench to check HE/debug-AP
	 * visibility of SRAM1 with the chain resident. Written BEFORE the CDC
	 * probe (review item 5) so it's on record even if the CDC reads hang. */
	*(volatile uint32_t *)SRAM1_MAILBOX_ADDR = MAILBOX_SENTINEL;
	__asm__ volatile("dsb sy" ::: "memory");
	r->mailbox_sentinel_written = 1;

	/* Task 4 / addendum F: non-fatal CDC200 register reads, twice ~1 ms
	 * apart. Reads only -- never a write, the display may be live. LAST
	 * (review item 5): this is the riskiest access in the whole probe (an
	 * unclocked/unpowered register can assert an asynchronous abort that
	 * CPSR.A can't unmask, or simply never complete the bus transaction),
	 * so everything else the probe can tell the bench is already recorded
	 * before this runs. */
	cdc_probe(r);

	return 0; /* start.S sets stage = 0xD0E and jumps to core0's SRAM park loop (goto_park's
		   * MPIDR dispatch); core1, if it came up, is already parking in its own SRAM1
		   * loop by now, both heartbeats advancing from here on -- task 5 / addendum D */
}
