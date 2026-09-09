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

#include <zephyr/kernel.h>

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

int main(void)
{
	printk("\n=== AEN801 RV-3028-C7 tick probe (#2037) -- RTC vs read vs host timebase, "
	       "BRD_I2C @0x%02x ===\n",
	       RV3028C7_I2C_ADDR);

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
