/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-rtc-tick-probe -- settles whether an intermittent RV-3028-C7 seconds
 * stall (#2037) is the RTC, the I2C read, or the HOST'S OWN TIMEBASE, for
 * the E1M-AEN801 (Alif Ensemble E8), bench RAM-run via J-Link.
 *
 * The failure this app exists to explain
 * ---------------------------------------
 * A demo phase reads the RTC, k_msleep(1100), reads again, and requires the
 * seconds field to have changed. Across five cold runs of a byte-identical
 * image it failed once: both reads returned ALP_OK, t0 == t1 == 00:00:10,
 * and the phase's own slept_ms counter -- measured with k_uptime_get(), the
 * SAME clock that ran the sleep -- read 1100. The RV-3028-C7 Application
 * Manual Rev. 1.4 documents no mechanism by which a successful burst read
 * of register 0x00 returns unchanged across a real gap of >= 1000 ms with
 * the oscillator already running. Every on-part explanation has been
 * checked against the manual and ruled out, which leaves the embarrassing
 * off-part one: a k_uptime_get()-measured 1100 ms sleep that is actually
 * under 1000 ms of real time would legitimately miss a 1 Hz tick, and
 * would do so while every value the demo logged read as a clean success.
 *
 * TEST A answers the load-bearing question directly: is the kernel
 * timebase itself trustworthy, measured against the RTC (+-5 ppm, the
 * better clock here) across a real 30 s window? If TEST A fails, every
 * other candidate this app -- or any other timing in this SDK build --
 * could measure is moot; the RTC and the kernel disagree about how long a
 * second is.
 *
 * TEST B answers a narrower question for the specific 1100 ms cadence the
 * demo actually uses: on an interval where the SECONDS byte does not
 * change, did the counter tick anyway? STATUS (0x0E) bit 4, UF, is an
 * independent witness -- the manual states UF is set within one second of
 * the Second update source being selected, which is the power-up default
 * (p.22) -- and the UNIX Time counter (0x1B..0x1E) is a second, wider
 * witness: the manual states it "does not know such register blocking"
 * (p.52), so it separates a memorised-but-not-yet-realised tick from no
 * tick at all. A stall where UF ends up set, or the UNIX counter advanced,
 * means the counter ticked and the seconds byte read stale -- the host or
 * bus is the suspect, not the part.
 *
 * Sampling strategy
 * ------------------
 * Five samples is not a rate. Loop TEST B inside one boot (>= 50
 * intervals, no cold power cycle needed) so a real miss rate can be
 * estimated in a single run instead of a coin-flip five-sample read.
 *
 * Two hard constraints from the datasheet
 * -----------------------------------------
 * NEVER write register 0x00 (Seconds), and NEVER write 1 to CONTROL_2
 * (0x10) bit 0 (RESET). Either one resets the prescaler from 8192 Hz back
 * to 1 Hz and restarts the current second, moving the next tick out by up
 * to a full second (Application Manual Rev. 1.4 p.14, p.24, p.85-86) --
 * the one documented way to stretch a second past 1100 ms, so writing
 * either would manufacture the very fault under investigation. This app
 * writes exactly one register on its own account -- STATUS (0x0E), to
 * disarm UF between samples -- and never touches 0x00 or CONTROL_2's bit
 * 0. rv3028c7_init() performs its own two writes internally (a STATUS
 * PORF-clear and a CONTROL_2 12_24-bit clear, see chips/rv3028c7/
 * rv3028c7.c) -- both plain, non-EEPROM, and already silicon-verified safe
 * by examples/aen/aen-rtc-control2-probe.
 *
 * Respect the 950 ms register-blocking window: during any access, the
 * counters at 0x00..0x06 are inhibited, and at most one tick is memorised
 * and realised after the I2C STOP (p.52, p.53). This app burst-reads
 * 0x00..0x1E -- time, STATUS, CONTROL_1/2, and UNIX Time -- in ONE
 * transaction per sample point (a few hundred microseconds at 100 kHz, far
 * under the 950 ms window) rather than several back-to-back reads, and
 * keeps sample points >= 1100 ms apart (Application Manual Rev. 1.4
 * register overview, p.41 -- 0x00..0x1E is one contiguous block).
 *
 * TEST 0 -- which core, and what clock, before any of the above (#2037
 * follow-up)
 * ------------------------------------------------------------------------
 * A bench run of this exact image measured the SysTick at 400.0 MHz
 * against a build that declares 160 MHz (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC),
 * derived from k_cycle_get_32() wrapping once across a 12 s RTC window --
 * 4.80e9 nominal cycles, minus one 2^32 wrap, lands within 0.79 ms of the
 * measured total. 160 MHz is the M55-HE's documented rate; 400 MHz is not
 * on the HE's clock menu at all (HWRM AHRM0012NDA v0.3 S8.3.2.3.4 --
 * ESCLK_SEL[ES1_PLL] only ever selects 80 or 160 MHz) -- it is the M55-HP's
 * ES0_PLL default. So either this image is executing on the HP while
 * believed to be on the HE, or this HE unit genuinely self-clocks at
 * 400 MHz -- and every observable this app used before now (UART, I2C,
 * ITCM at local address 0, DTCM at 0x20000000) is identical on both cores.
 * TEST 0 runs first and answers this directly instead of guessing:
 *
 *   TEST 0a -- core identity via the DTCM global-alias readback. A magic
 *   value is written to a local DTCM variable, then read back through both
 *   cores' "external access" global aliases (HE: 0x5880_0000+offset, HP:
 *   0x5080_0000+offset -- HWRM Table 10-2, p.326). The naive premise
 *   ("whichever alias shows the magic is your core") does NOT hold once
 *   the actual access matrix is checked: Table 10-2 lists a core's OWN
 *   external alias as access=N for that same core -- only the OTHER core
 *   and the A32 can reach it (HE: N at 0x5880_0000, Y at 0x5080_0000; HP:
 *   N at 0x5080_0000, Y at 0x5880_0000). So per the manual, the alias that
 *   FAULTS identifies the executing core, and the alias that succeeds
 *   reads a different, physically foreign SRAM that was never written --
 *   the inverse of what a first read of the two addresses suggests. This
 *   app checks BOTH signals (which read faults, which read shows the
 *   magic) and reports plainly if real silicon disagrees with the table,
 *   rather than asserting a verdict the data doesn't support. A guarded
 *   read that might fault runs in a disposable worker thread -- Zephyr's
 *   own fault handling (kernel/fatal.c: z_fatal_error() -> k_thread_abort()
 *   on the current thread once k_sys_fatal_error_handler() returns) kills
 *   only that thread, not main() -- so a fault here is reported, not fatal
 *   to the rest of this app's run.
 *
 *   TEST 0b -- the four CGU registers this decision turns on, READ ONLY:
 *   PLL_LOCK_CTRL (0x1A602004), PLL_CLK_SEL (0x1A602008, bit 20 ES1 =
 *   HE's oscillator-vs-PLL select), ESCLK_SEL (0x1A602010, ES1_PLL/
 *   ES1_OSC/ES0_PLL rate-select fields), and CLK_ENA (0x1A602014). Every
 *   address and bit position here was checked against HWRM AHRM0012NDA
 *   v0.3 S8.3.2.3.3/.4 directly, not taken on trust. This app NEVER writes
 *   any CGU register, and never writes CLK_ENA in particular -- see the
 *   read site's comment for why.
 *
 *   TEST 0c -- the SysTick/RTC ratio sampled at ~2 s, ~10 s, and ~60 s
 *   after boot (not just once, so a rate that changes mid-run is caught),
 *   printed alongside CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC. Reuses this
 *   app's own read_burst()/burst_unix_time() rather than a second RTC
 *   read path. Guards the exact wrap that cost real analysis time here:
 *   k_cycle_get_32() wraps every ~10.7 s at 400 MHz, so a raw
 *   before/after subtraction across a 60 s window silently loses several
 *   wraps. This app uses k_cycle_get_64() where the platform provides a
 *   real 64-bit counter (CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER), and where
 *   it doesn't, folds every 32-bit wrap in explicitly via polling more
 *   often than the fastest plausible wrap period -- see cyc64_tracker_t.
 *
 * Bus
 * ---
 * Same bus as aen-rtc-control2-probe: the on-module BRD bus -- SoC I2C0,
 * portable alias alp-i2c2 (portable bus index 2) -- at 7-bit address 0x52
 * (RV3028C7_I2C_ADDR). The board layer already enables it (metadata/
 * e1m_modules/aen/on-module-links.yaml `brd_i2c` entry); this app's own
 * overlay carries only the bench ITCM retarget.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf) when the
 * bench forces it. BENCH-VALIDATION app -- not a customer teaching
 * example.
 */

#include <stdbool.h>
#include <stdint.h>

#include <cmsis_core.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "alp/chips/rv3028c7.h"

/*
 * Raw register facts read OUTSIDE the rv3028c7 driver's public API -- same
 * rationale as aen-rtc-control2-probe: the driver deliberately exposes no
 * raw-register accessor, so a bench probe that needs STATUS/CONTROL_2/
 * UNIX-Time duplicates the addresses it needs here rather than growing the
 * driver's public surface for one diagnostic app. Addresses per the
 * RV-3028-C7 Application Manual Rev. 1.4 register overview, p.12/p.41 --
 * the same table chips/rv3028c7/rv3028c7.c cites for its own private
 * RV3028_REG_* constants; keep these tables in step if either grows.
 */
#define RV3028_REG_STATUS 0x0Eu
#define RV3028_REG_CTRL2  0x10u
#define RV3028_REG_UNIX0  0x1Bu /* UNIX Time counter, 4 bytes, 0x1B..0x1E */

/** STATUS bits this app reads. RV-3028-C7 Application Manual Rev. 1.4,
 *  Table 9. EEbusy (bit 7) is read-only "EEPROM write in flight", never an
 *  event -- printed for visibility only, since a 0xFF bus-timeout read
 *  here would otherwise be invisible (the driver never logs it). */
#define RV3028_STATUS_PORF 0x01u
#define RV3028_STATUS_UF   0x10u /**< Periodic update flag -- Test B's independent tick witness. */
#define RV3028_STATUS_EEBUSY 0x80u

/** One burst covering Seconds..UNIX-Time-MSB (0x00..0x1E) in a single I2C
 *  transaction -- see the file header's 950 ms register-blocking
 *  constraint for why this must stay one transaction, not several. */
#define RV3028_BURST_LEN 0x1Fu

/** Test B sampling cadence -- deliberately the SAME 1100 ms the failing
 *  demo phase uses (see file header), so a real miss rate for that exact
 *  cadence can be measured instead of guessed at. */
#define SAMPLE_GAP_MS      1100u
#define TEST_B_INTERVALS   55u /**< >= 50 required; five samples is not a rate. */
#define TEST_A_WINDOW_MS   30000u
#define TEST_A_TOLERANCE_S 1u /**< 30 s +/- 1 s, per the task's PASS band. */

/*
 * ---------------------------------------------------------------------
 * TEST 0a -- core identity via DTCM global-alias readback.
 * See the file header's "TEST 0" section for the full rationale and the
 * HWRM Table 10-2 access-matrix inversion this test's verdict logic is
 * built on.
 * ---------------------------------------------------------------------
 */

/** Local (core-internal) DTCM base -- SAME address on both cores, backed
 *  by physically different SRAM depending which core executes (HWRM
 *  Table 10-2, p.326: 0x2000_0000 is SRAM5/M55-HE 256 KB for HE, SRAM3/
 *  M55-HP 1 MB for HP). A variable placed here is ordinary .bss/.data. */
#define DTCM_LOCAL_BASE 0x20000000UL

/** "External access" global aliases -- HWRM Table 10-2, p.326. Per that
 *  table a core CANNOT reach its OWN DTCM through its own alias (HE: N at
 *  0x5880_0000; HP: N at 0x5080_0000) -- only the OTHER core and the A32
 *  can. See the file header for what this means for the verdict below. */
#define HE_DTCM_ALIAS_BASE 0x58800000UL /**< SRAM5 (M55-HE DTCM) external access. */
#define HP_DTCM_ALIAS_BASE 0x50800000UL /**< SRAM3 (M55-HP DTCM) external access. */

#define CORE_ID_MAGIC 0xC0DE1D01UL

/*
 * Guarded MMIO read -- runs the read in a disposable worker thread so a
 * bus fault kills only THAT thread. Zephyr's own fault path does this for
 * free: z_fatal_error() (kernel/fatal.c) calls k_thread_abort() on the
 * CURRENT thread once k_sys_fatal_error_handler() returns -- so main()
 * and every other thread are untouched by a fault in the worker. The
 * override below only lets that abort proceed (by returning) while
 * g_guarded_probe_active is set; any fault OUTSIDE that narrow window is
 * a real, unexpected crash and halts loudly, same as this app's other
 * RESULT FAIL paths.
 */
static volatile bool         g_guarded_probe_active;
static volatile bool         g_guarded_probe_faulted;
static volatile unsigned int g_guarded_probe_fault_reason;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	if (g_guarded_probe_active) {
		g_guarded_probe_fault_reason = reason;
		g_guarded_probe_faulted      = true;
		return;
	}
	printk("RESULT FAIL: fatal error (reason=%u) outside the guarded probe window -- halting\n",
	       reason);
	for (;;) {
		__WFE();
	}
}

struct alias_probe {
	uintptr_t addr;
	uint32_t  value;
};

K_SEM_DEFINE(probe_done_sem, 0, 1);
K_THREAD_STACK_DEFINE(probe_stack_a, 1024);
static struct k_thread probe_thread_a;
K_THREAD_STACK_DEFINE(probe_stack_b, 1024);
static struct k_thread probe_thread_b;

static void alias_probe_entry(void *p1, void *p2, void *p3)
{
	struct alias_probe *probe = p1;
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	probe->value = *(volatile uint32_t *)probe->addr;
	k_sem_give(&probe_done_sem);
}

/** One guarded 32-bit read. Returns true + *out_value on a clean read;
 *  returns false on a fault or a 50 ms timeout (generous for one load;
 *  a real fault resolves in microseconds -- the timeout exists only so a
 *  genuine bus wedge, which this app does not expect but does not trust
 *  either, can't hang the probe forever). Either way main() is never at
 *  risk -- only the disposable worker thread above dies. Uses a SEPARATE
 *  static thread object per caller (thread_obj/stack) rather than
 *  reusing one, so there is no "is the previous worker really terminated
 *  yet" question to answer. */
static bool guarded_read32(struct k_thread  *thread_obj,
                           k_thread_stack_t *stack,
                           size_t            stack_size,
                           uintptr_t         addr,
                           uint32_t         *out_value,
                           unsigned int     *out_fault_reason)
{
	struct alias_probe probe = { .addr = addr, .value = 0 };

	g_guarded_probe_faulted = false;
	g_guarded_probe_active  = true;

	k_tid_t tid = k_thread_create(thread_obj,
	                              stack,
	                              stack_size,
	                              alias_probe_entry,
	                              &probe,
	                              NULL,
	                              NULL,
	                              K_PRIO_COOP(5),
	                              0,
	                              K_NO_WAIT);

	int rc = k_sem_take(&probe_done_sem, K_MSEC(50));

	g_guarded_probe_active = false;
	k_thread_abort(tid); /* no-op if the thread already terminated (clean read or fault alike) */

	if (rc == 0) {
		*out_value = probe.value;
		return true;
	}
	*out_fault_reason = g_guarded_probe_faulted ? g_guarded_probe_fault_reason : 0xFFFFFFFFu;
	return false;
}

static void test0a_core_identity(void)
{
	static volatile uint32_t core_id_var;

	printk("\n=== TEST 0a: core identity -- DTCM global-alias readback ===\n");

	/* Same core writes and (via the worker threads below) reads this --
	 * program order + volatile is sufficient here, no cross-master
	 * coherency concern like a real DMA/other-core race would raise. */
	core_id_var = (uint32_t)CORE_ID_MAGIC;

	uintptr_t local_addr = (uintptr_t)&core_id_var;
	uintptr_t offset     = local_addr - DTCM_LOCAL_BASE;
	uintptr_t he_addr    = HE_DTCM_ALIAS_BASE + offset;
	uintptr_t hp_addr    = HP_DTCM_ALIAS_BASE + offset;

	uint32_t     he_value = 0, hp_value = 0;
	unsigned int he_fault_reason = 0, hp_fault_reason = 0;
	bool         he_ok = guarded_read32(&probe_thread_a,
	                                    probe_stack_a,
	                                    K_THREAD_STACK_SIZEOF(probe_stack_a),
	                                    he_addr,
	                                    &he_value,
	                                    &he_fault_reason);
	bool         hp_ok = guarded_read32(&probe_thread_b,
	                                    probe_stack_b,
	                                    K_THREAD_STACK_SIZEOF(probe_stack_b),
	                                    hp_addr,
	                                    &hp_value,
	                                    &hp_fault_reason);

	printk("local DTCM var   @ 0x%08x = 0x%08x (written)\n",
	       (unsigned)local_addr,
	       (unsigned)CORE_ID_MAGIC);
	if (he_ok) {
		printk("HE global alias  @ 0x%08x -> 0x%08x %s\n",
		       (unsigned)he_addr,
		       (unsigned)he_value,
		       (he_value == (uint32_t)CORE_ID_MAGIC) ? "(MAGIC MATCH)" : "(no magic)");
	} else {
		printk("HE global alias  @ 0x%08x -> FAULT/TIMEOUT (reason=%u) -- not readable from "
		       "the executing core\n",
		       (unsigned)he_addr,
		       he_fault_reason);
	}
	if (hp_ok) {
		printk("HP global alias  @ 0x%08x -> 0x%08x %s\n",
		       (unsigned)hp_addr,
		       (unsigned)hp_value,
		       (hp_value == (uint32_t)CORE_ID_MAGIC) ? "(MAGIC MATCH)" : "(no magic)");
	} else {
		printk("HP global alias  @ 0x%08x -> FAULT/TIMEOUT (reason=%u) -- not readable from "
		       "the executing core\n",
		       (unsigned)hp_addr,
		       hp_fault_reason);
	}

	bool he_magic = he_ok && (he_value == (uint32_t)CORE_ID_MAGIC);
	bool hp_magic = hp_ok && (hp_value == (uint32_t)CORE_ID_MAGIC);

	if (he_magic && !hp_magic) {
		printk("core identity verdict: M55-HE (magic value read back through the HE alias)\n");
	} else if (hp_magic && !he_magic) {
		printk("core identity verdict: M55-HP (magic value read back through the HP alias)\n");
	} else if (!he_ok && hp_ok) {
		printk("core identity verdict: M55-HE -- HE's own external DTCM alias faulted, HP's "
		       "didn't (HWRM Table 10-2 p.326: a core cannot reach its own external alias, "
		       "only the OTHER core/A32 can)\n");
	} else if (!hp_ok && he_ok) {
		printk("core identity verdict: M55-HP -- HP's own external DTCM alias faulted, HE's "
		       "didn't (HWRM Table 10-2 p.326: a core cannot reach its own external alias, "
		       "only the OTHER core/A32 can)\n");
	} else {
		printk("core identity verdict: INCONCLUSIVE (he_ok=%d hp_ok=%d he_magic=%d hp_magic=%d) "
		       "-- the observed pattern doesn't match HWRM Table 10-2's access matrix in "
		       "either direction; trust the raw values printed above over this line\n",
		       (int)he_ok,
		       (int)hp_ok,
		       (int)he_magic,
		       (int)hp_magic);
	}
}

/*
 * ---------------------------------------------------------------------
 * TEST 0b -- CGU registers, READ ONLY.
 * Addresses + bit positions verified against HWRM AHRM0012NDA v0.3
 * S8.3.2.3.3 (PLL_CLK_SEL) / S8.3.2.3.4 (ESCLK_SEL) directly -- see the
 * file header's "TEST 0" section.
 * ---------------------------------------------------------------------
 */
#define CGU_PLL_LOCK_CTRL 0x1A602004UL
#define CGU_PLL_CLK_SEL   0x1A602008UL
#define CGU_ESCLK_SEL     0x1A602010UL
#define CGU_CLK_ENA       0x1A602014UL

static void test0b_cgu_registers(void)
{
	printk("\n=== TEST 0b: CGU registers (read-only) -- HWRM AHRM0012NDA v0.3 "
	       "S8.3.2.3.3/.4 ===\n");

	/*
	 * Plain reads of the AON CGU block (base 0x1A60_2000, HWRM Table
	 * 10-2 p.325: A32/M55-HP/M55-HE all Y). THESE ARE READS ONLY -- this
	 * app never writes any CGU register, and NEVER writes CLK_ENA
	 * (0x1A602014) in particular: it gates clocks for both cores and
	 * other blocks on this bus, and a bad write here can silently kill a
	 * clock this app -- or the OTHER core -- depends on to keep running.
	 */
	uint32_t pll_lock_ctrl = *(volatile uint32_t *)CGU_PLL_LOCK_CTRL;
	uint32_t pll_clk_sel   = *(volatile uint32_t *)CGU_PLL_CLK_SEL;
	uint32_t esclk_sel     = *(volatile uint32_t *)CGU_ESCLK_SEL;
	uint32_t clk_ena       = *(volatile uint32_t *)CGU_CLK_ENA;

	printk("PLL_LOCK_CTRL (0x1A602004) = 0x%08x\n", pll_lock_ctrl);
	printk("PLL_CLK_SEL   (0x1A602008) = 0x%08x\n", pll_clk_sel);
	printk("ESCLK_SEL     (0x1A602010) = 0x%08x\n", esclk_sel);
	printk("CLK_ENA       (0x1A602014) = 0x%08x\n", clk_ena);

	/* Decode exactly the fields this investigation turns on (HWRM
	 * S8.3.2.3.3/.4 -- verified against the manual text, not taken on
	 * trust; see the file header). */
	unsigned int es1     = (pll_clk_sel >> 20) & 0x1u; /* PLL_CLK_SEL[20]  -- HE src: 0=OSC 1=PLL */
	unsigned int es0     = (pll_clk_sel >> 16) & 0x1u; /* PLL_CLK_SEL[16]  -- HP src: 0=OSC 1=PLL */
	unsigned int es1_pll = (esclk_sel >> 4) & 0x3u;    /* ESCLK_SEL[5:4]   -- HE PLL rate select */
	unsigned int es1_osc = (esclk_sel >> 12) & 0x3u;   /* ESCLK_SEL[13:12] -- HE OSC rate select */
	unsigned int es0_pll = (esclk_sel >> 0) & 0x3u;    /* ESCLK_SEL[1:0]   -- HP PLL rate select */

	static const uint32_t es1_pll_mhz[4]  = { 80u, 80u, 160u, 160u };
	static const uint32_t es0_pll_mhz[4]  = { 100u, 200u, 400u, 400u };
	static const char    *es1_osc_name[4] = {
		"76.8 MHz ring-osc", "38.4 MHz ring-osc", "76.8 MHz crystal-osc", "38.4 MHz crystal-osc"
	};

	printk("  PLL_CLK_SEL: ES1(HE src)=%u ES0(HP src)=%u\n", es1, es0);
	printk("  ESCLK_SEL:   ES1_PLL=%u (%u MHz)  ES1_OSC=%u (%s)  ES0_PLL=%u (%u MHz)\n",
	       es1_pll,
	       es1_pll_mhz[es1_pll],
	       es1_osc,
	       es1_osc_name[es1_osc],
	       es0_pll,
	       es0_pll_mhz[es0_pll]);

	if (es1) {
		printk("  -> HE (RTSS_HE_CLK) active source: PLL -> %u MHz\n", es1_pll_mhz[es1_pll]);
	} else {
		printk("  -> HE (RTSS_HE_CLK) active source: OSC -> %s\n", es1_osc_name[es1_osc]);
	}
	if (es0) {
		printk("  -> HP (RTSS_HP_CLK) active source: PLL -> %u MHz\n", es0_pll_mhz[es0_pll]);
	} else {
		printk("  -> HP (RTSS_HP_CLK) active source: OSC (ES0_OSC not decoded above -- see "
		       "raw ESCLK_SEL bits 9:8)\n");
	}
}

/*
 * ---------------------------------------------------------------------
 * TEST 0c -- SysTick/RTC ratio at ~2 s / ~10 s / ~60 s after boot.
 * ---------------------------------------------------------------------
 */

/** Wrap-safe cycle-count tracker, used only when the platform has no real
 *  64-bit hardware counter (see cyc64_now() below). k_cycle_get_32()
 *  wraps every ~10.7 s at the 400 MHz rate this app exists to explain --
 *  the exact wrap that turned one bench error into two (file header).
 *  cyc64_tracker_poll() must be called more often than the FASTEST
 *  plausible wrap period so every wrap is folded in as it happens,
 *  rather than trying to reconstruct a missed wrap count afterward. */
typedef struct {
	uint32_t last32;
	uint64_t wraps;
} cyc64_tracker_t;

static void cyc64_tracker_init(cyc64_tracker_t *t)
{
	t->last32 = k_cycle_get_32();
	t->wraps  = 0;
}

static uint64_t cyc64_tracker_poll(cyc64_tracker_t *t)
{
	uint32_t now = k_cycle_get_32();
	if (now < t->last32) {
		t->wraps++;
	}
	t->last32 = now;
	return (t->wraps << 32) | now;
}

/** Current wrap-safe cycle count. Prefers the real 64-bit hardware
 *  counter when the platform provides one (CONFIG_TIMER_HAS_64BIT_
 *  CYCLE_COUNTER -- never wraps in any run this app could see); falls
 *  back to the polled 32-bit tracker above otherwise. */
static uint64_t cyc64_now(cyc64_tracker_t *fallback)
{
	if (IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)) {
		return k_cycle_get_64();
	}
	return cyc64_tracker_poll(fallback);
}

/* test0c_ratio_checkpoints() itself is defined further down, after
 * read_burst()/burst_unix_time() (it reuses both -- see the file header's
 * "TEST 0" section for why -- and those need burst_t, defined below). */

static uint8_t bcd_to_bin(uint8_t bcd)
{
	return (uint8_t)((bcd & 0x0Fu) + ((bcd >> 4) & 0x0Fu) * 10u);
}

/** One combined register snapshot (0x00..0x1E) plus the transaction's
 *  own status code -- every field this app needs comes out of the same
 *  transaction, so "before" and "after" are always genuinely paired. */
typedef struct {
	uint8_t      raw[RV3028_BURST_LEN];
	alp_status_t rc;
} burst_t;

static burst_t read_burst(alp_i2c_t *bus)
{
	burst_t b;
	uint8_t reg = 0x00u;
	b.rc        = alp_i2c_write_read(bus, RV3028C7_I2C_ADDR, &reg, 1, b.raw, sizeof(b.raw));
	return b;
}

/** Print every byte of a burst, in hex, unconditionally -- decoded fields
 *  hide exactly the failure this app is chasing (a byte that reads back
 *  identical when it should not have). */
static void print_raw_hex(const char *label, const burst_t *b)
{
	printk("%-8s raw[0x00..0x1E] rc=%d:", label, (int)b->rc);
	for (size_t i = 0; i < sizeof(b->raw); i++) {
		printk(" %02x", b->raw[i]);
	}
	printk("\n");
}

static uint8_t burst_seconds_bin(const burst_t *b)
{
	return bcd_to_bin(b->raw[0x00] & 0x7Fu);
}
static uint8_t burst_status(const burst_t *b)
{
	return b->raw[RV3028_REG_STATUS];
}
static uint8_t burst_ctrl2(const burst_t *b)
{
	return b->raw[RV3028_REG_CTRL2];
}
/** UNIX Time counter, LSB-first per vendor convention -- unverified
 *  against the datasheet page image (no local copy in this tree), so the
 *  raw bytes are ALWAYS printed alongside every decoded value above so a
 *  bench reader can catch a byte-order mistake directly instead of
 *  trusting this decode blind. */
static uint32_t burst_unix_time(const burst_t *b)
{
	const uint8_t *u = &b->raw[RV3028_REG_UNIX0];
	return (uint32_t)u[0] | ((uint32_t)u[1] << 8) | ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24);
}

/** Disarm UF ahead of the next sample -- the ONLY write this app performs
 *  on its own account. Plain STATUS (0x0E) write, preserving every other
 *  latched flag; never register 0x00, never CONTROL_2 bit 0 (RESET) --
 *  see the file header hard constraint. */
static alp_status_t clear_uf(alp_i2c_t *bus, uint8_t status_now)
{
	uint8_t val        = (uint8_t)(status_now & ~RV3028_STATUS_UF);
	uint8_t scratch[2] = { RV3028_REG_STATUS, val };
	return alp_i2c_write(bus, RV3028C7_I2C_ADDR, scratch, sizeof(scratch));
}

/*
 * ---------------------------------------------------------------------
 * TEST 0c -- SysTick/RTC ratio at ~2 s / ~10 s / ~60 s after boot.
 * Reuses read_burst()/burst_unix_time() above rather than a second RTC
 * read path (file header). Performs no writes of its own -- clear_uf()
 * above stays the app's only write, and this test never calls it.
 * ---------------------------------------------------------------------
 */
static void test0c_ratio_checkpoints(alp_i2c_t *bus)
{
	printk("\n=== TEST 0c: SysTick/RTC ratio at 2 s / 10 s / 60 s -- "
	       "CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=%u (declared) ===\n",
	       (unsigned)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);

	cyc64_tracker_t tracker;
	cyc64_tracker_init(&tracker);

	burst_t  boot_rtc = read_burst(bus);
	uint64_t boot_cyc = cyc64_now(&tracker);
	if (boot_rtc.rc != ALP_OK) {
		printk("TEST 0c RESULT: FAIL -- boot-time burst read rc=%d\n", (int)boot_rtc.rc);
		return;
	}
	uint32_t boot_unix = burst_unix_time(&boot_rtc);

	static const uint32_t checkpoints_ms[] = { 2000u, 10000u, 60000u };
	uint32_t              elapsed_ms       = 0;

	for (size_t i = 0; i < ARRAY_SIZE(checkpoints_ms); i++) {
		uint32_t target = checkpoints_ms[i];

		while (elapsed_ms < target) {
			/* <=1 s hops -- comfortably under the fastest candidate
			 * wrap period (~10.7 s @ 400 MHz) so the tracker never
			 * misses a wrap even if this image really is at 400 MHz. */
			uint32_t step = MIN(1000u, target - elapsed_ms);
			k_msleep(step);
			elapsed_ms += step;
			(void)cyc64_now(&tracker); /* keep the wrap tracker current */
		}

		burst_t  now_rtc = read_burst(bus);
		uint64_t now_cyc = cyc64_now(&tracker);
		if (now_rtc.rc != ALP_OK) {
			printk("  [t=%u ms] RESULT: FAIL -- burst read rc=%d\n", elapsed_ms, (int)now_rtc.rc);
			continue;
		}

		uint32_t now_unix    = burst_unix_time(&now_rtc);
		uint32_t rtc_delta_s = now_unix - boot_unix;
		uint64_t cyc_delta   = now_cyc - boot_cyc;

		if (rtc_delta_s == 0u) {
			printk("  [t=%u ms] rtc_delta=0 s -- too early to compute a rate\n", elapsed_ms);
			continue;
		}

		uint32_t implied_hz = (uint32_t)(cyc_delta / rtc_delta_s);

		printk("  [t=%u ms] rtc_delta=%u s  cyc_delta=%llu cycles  implied=%u Hz "
		       "(declared %u Hz)\n",
		       elapsed_ms,
		       rtc_delta_s,
		       (unsigned long long)cyc_delta,
		       implied_hz,
		       (unsigned)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
	}
}

int main(void)
{
	printk("\n=== AEN801 RV-3028-C7 tick probe (#2037) -- RTC vs read vs host timebase, "
	       "BRD_I2C @0x%02x ===\n",
	       RV3028C7_I2C_ADDR);

	/* TEST 0a/0b run first and need neither alp_init() nor I2C -- pure
	 * SoC-local register/DTCM reads. See the file header's "TEST 0"
	 * section for why these come before every other test in this app. */
	test0a_core_identity();
	test0b_cgu_registers();

	(void)alp_init();

	/* BRD_I2C -- portable bus 2 (alp-i2c2), board-layer enabled; see the
	 * file header for why no bus wiring lives in this app's overlay. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = 2u,
	    .bitrate_hz = 100000u,
	});
	if (bus == NULL) {
		printk("RESULT FAIL: alp_i2c_open(BRD_I2C) -> NULL, alp_last_error=%d\n",
		       (int)alp_last_error());
		return 0;
	}

	/* What init sees. The driver never logs raw STATUS/CONTROL_2 itself
	 * (see aen-rtc-control2-probe's own note on this); read them here,
	 * immediately before calling rv3028c7_init(), so a 0xFF bus-timeout
	 * read at bring-up is visible instead of silently swallowed. */
	burst_t pre_init = read_burst(bus);
	print_raw_hex("pre-init", &pre_init);
	printk("raw STATUS (0x0E) as init sees it: 0x%02x (PORF=%u UF=%u EEbusy=%u)\n",
	       burst_status(&pre_init),
	       (burst_status(&pre_init) & RV3028_STATUS_PORF) ? 1 : 0,
	       (burst_status(&pre_init) & RV3028_STATUS_UF) ? 1 : 0,
	       (burst_status(&pre_init) & RV3028_STATUS_EEBUSY) ? 1 : 0);
	printk("raw CONTROL_2 (0x10) as init sees it: 0x%02x\n", burst_ctrl2(&pre_init));
	if (pre_init.rc != ALP_OK) {
		printk("RESULT FAIL: pre-init burst read rc=%d -- RTC not answering at 0x%02x\n",
		       (int)pre_init.rc,
		       RV3028C7_I2C_ADDR);
		alp_i2c_close(bus);
		return 0;
	}

	rv3028c7_t   ctx;
	alp_status_t rc_init    = rv3028c7_init(&ctx, bus);
	bool         cold_start = false;
	(void)rv3028c7_was_cold_start(&ctx, &cold_start);
	printk("rv3028c7_init() -> rc=%d cold_start=%s\n", (int)rc_init, cold_start ? "true" : "false");
	if (rc_init != ALP_OK) {
		printk("RESULT FAIL: rv3028c7_init rc=%d\n", (int)rc_init);
		alp_i2c_close(bus);
		return 0;
	}

	/* TEST 0c needs a live RTC, so it runs here -- still before TEST A/B,
	 * still no writes of its own (see its own doc comment above). */
	test0c_ratio_checkpoints(bus);

	/*
	 * ---------------------------------------------------------------
	 * TEST A -- host timebase vs the RTC, across a real 30 s window.
	 * The RTC is +-5 ppm; the kernel is the suspect here. This is the
	 * single most important number in this app: if it isn't 30, the
	 * "perfect part" premise behind every other candidate is wrong,
	 * and every k_msleep/k_uptime_get timing on this SDK build is
	 * suspect, not just this RTC phase.
	 * ---------------------------------------------------------------
	 */
	printk("\n=== TEST A: host timebase vs RV-3028-C7 over a real %u ms window ===\n",
	       TEST_A_WINDOW_MS);

	burst_t  a0   = read_burst(bus);
	int64_t  up0  = k_uptime_get();
	uint32_t cyc0 = k_cycle_get_32();
	print_raw_hex("A0", &a0);

	if (a0.rc != ALP_OK) {
		printk("TEST A RESULT: FAIL -- pre-sleep burst read rc=%d\n", (int)a0.rc);
	} else {
		k_msleep(TEST_A_WINDOW_MS);

		burst_t  a1   = read_burst(bus);
		int64_t  up1  = k_uptime_get();
		uint32_t cyc1 = k_cycle_get_32();
		print_raw_hex("A1", &a1);

		if (a1.rc != ALP_OK) {
			printk("TEST A RESULT: FAIL -- post-sleep burst read rc=%d\n", (int)a1.rc);
		} else {
			uint32_t rtc_delta_s     = burst_unix_time(&a1) - burst_unix_time(&a0);
			int64_t  kernel_delta_ms = up1 - up0;
			uint32_t cyc_delta       = cyc1 - cyc0;
			uint64_t cyc_delta_ms    = k_cyc_to_ns_floor64(cyc_delta) / 1000000ull;

			printk("RTC unix-time delta:    %u s  (before=%u after=%u)\n",
			       rtc_delta_s,
			       burst_unix_time(&a0),
			       burst_unix_time(&a1));
			printk("k_uptime_get() delta:   %lld ms\n", kernel_delta_ms);
			printk("k_cycle_get_32() delta: %u cycles = %llu ms "
			       "(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=%u)\n",
			       cyc_delta,
			       cyc_delta_ms,
			       (unsigned)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);

			bool test_a_pass = (rtc_delta_s >= (30u - TEST_A_TOLERANCE_S)) &&
			                   (rtc_delta_s <= (30u + TEST_A_TOLERANCE_S));
			if (test_a_pass) {
				printk("TEST A RESULT: PASS -- RTC measured %u s across the 30 s "
				       "window; the kernel timebase is trustworthy here.\n",
				       rtc_delta_s);
			} else {
				printk("TEST A RESULT: FAIL -- RTC measured %u s, not 30 +/-%u. "
				       "The RTC is the better clock (+-5 ppm) -- this points at "
				       "the KERNEL TIMEBASE, not the RTC, and it invalidates the "
				       "premise every other candidate here assumed (a perfect "
				       "part). Every k_msleep/k_uptime_get timing on this SDK "
				       "build is suspect until this is understood.\n",
				       rtc_delta_s,
				       TEST_A_TOLERANCE_S);
			}
		}
	}

	/*
	 * ---------------------------------------------------------------
	 * TEST B -- at the demo's own 1100 ms cadence, did the counter
	 * tick even on an interval where the seconds byte read stale?
	 * UF and the UNIX Time counter are independent witnesses; see the
	 * file header for what each one distinguishes.
	 * ---------------------------------------------------------------
	 */
	printk("\n=== TEST B: %u x %u ms intervals -- seconds vs UF vs UNIX-time ===\n",
	       TEST_B_INTERVALS,
	       SAMPLE_GAP_MS);

	burst_t prev = read_burst(bus);
	print_raw_hex("B init", &prev);
	if (prev.rc == ALP_OK) (void)clear_uf(bus, burst_status(&prev));

	uint32_t intervals_run = 0, advanced = 0, not_advanced = 0, read_failures = 0;
	uint32_t nonadv_uf_set = 0, nonadv_uf_clear = 0;
	uint32_t nonadv_unix_ticked = 0, nonadv_unix_flat = 0;

	for (uint32_t i = 0; i < TEST_B_INTERVALS; i++) {
		k_msleep(SAMPLE_GAP_MS);

		burst_t cur = read_burst(bus);
		char    lbl[16];
		snprintk(lbl, sizeof(lbl), "B[%u]", i);
		print_raw_hex(lbl, &cur);

		if (prev.rc != ALP_OK || cur.rc != ALP_OK) {
			read_failures++;
			if (cur.rc == ALP_OK) (void)clear_uf(bus, burst_status(&cur));
			prev = cur;
			continue;
		}

		intervals_run++;
		uint8_t  sec_before  = burst_seconds_bin(&prev);
		uint8_t  sec_after   = burst_seconds_bin(&cur);
		bool     ticked      = (sec_before != sec_after);
		bool     uf_set      = (burst_status(&cur) & RV3028_STATUS_UF) != 0;
		uint32_t unix_delta  = burst_unix_time(&cur) - burst_unix_time(&prev);
		bool     unix_ticked = (unix_delta != 0u);

		if (ticked) {
			advanced++;
			printk("  [%u] seconds %02u -> %02u : ADVANCED (UF=%u unix_delta=%u)\n",
			       i,
			       sec_before,
			       sec_after,
			       uf_set,
			       unix_delta);
		} else {
			not_advanced++;
			if (uf_set)
				nonadv_uf_set++;
			else
				nonadv_uf_clear++;
			if (unix_ticked)
				nonadv_unix_ticked++;
			else
				nonadv_unix_flat++;
			const char *verdict =
			    (uf_set || unix_ticked)
			        ? "counter DID tick -- stale read (host/bus), not a real stall"
			        : "counter did NOT tick -- genuine stall";
			printk("  [%u] seconds %02u == %02u : STALL (UF=%u unix_delta=%u) -- %s\n",
			       i,
			       sec_before,
			       sec_after,
			       uf_set,
			       unix_delta,
			       verdict);
		}

		/* Re-arm UF for the next interval -- see clear_uf()'s doc comment
		 * for why this is the only write this loop performs. */
		(void)clear_uf(bus, burst_status(&cur));
		prev = cur;
	}

	printk("\nTEST B SUMMARY: intervals=%u advanced=%u not_advanced=%u read_failures=%u\n",
	       intervals_run,
	       advanced,
	       not_advanced,
	       read_failures);
	printk("  of the %u non-advancing intervals: UF_set=%u UF_clear=%u | "
	       "unix_ticked=%u unix_flat=%u\n",
	       not_advanced,
	       nonadv_uf_set,
	       nonadv_uf_clear,
	       nonadv_unix_ticked,
	       nonadv_unix_flat);

	alp_i2c_close(bus);
	return 0;
}
