/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-gd32-bridge-loopback -- single-pass JUMPERED Tier-B LOOPBACK
 * validation of the GD32 supervisor bridge.
 *
 * Where the functional tier drives each bridge surface against a
 * KNOWN-EXACT software answer (sqrt(4) == 2.0, an invalid ADC config
 * is REJECTED) and reads ADC pads that float on the bench, this tier
 * closes the loop in COPPER: three physical jumpers on the E1M-X V2
 * carrier route a bridge OUTPUT back into a bridge INPUT, so the
 * analog + timer signal paths -- which a floating-pad test cannot
 * prove -- get validated end to end on real silicon.
 *
 *   Jumper A: raw DAC0 (E1M-X A19) -> CK_ANA -> ADC channel 0 (1:1)
 *   Jumper B: PWM ch1  -> ENC1_X (encoder index 1, X input)
 *   Jumper C: PWM ch2  -> PWM ch3 (rebound as input-capture)
 *
 * See README.md for the exact header pins, the wiring photo note, and
 * the full rationale.  THIS FILE IS A MAINTAINER BENCH TOOL IN EXAMPLE
 * FORM: like the functional tier it exercises the gd32g553 chip driver
 * DIRECTLY (the documented exception to the portable-API rule for
 * dedicated bridge demos).  On an unwired board every value assertion
 * fails -- that is expected; the suite is only meaningful on a board
 * jumpered per the README.
 *
 * ====================================================================
 * SAFETY (read before plugging anything in)
 * ====================================================================
 *   * Jumper A is a DIRECT tap of the raw DAC0 net: the carrier's
 *     buffered DAC output path is INOPERABLE on this carrier revision
 *     and a bench rework (2026-06-04, documented in the internal
 *     carrier errata) is required before the raw tap reads true.
 *     DAC and ADC share the same 1.8 V analog rail, so a direct
 *     loopback physically cannot overdrive the ADC pad; the
 *     DAC_MAX_SAFE_MV bound below is a linearity choice (stay off the
 *     rail-clip region), not an electrical-safety cap.
 *   * NO physical rotary encoder may be plugged into J18 during the
 *     qenc test.  Jumper B drives ENC1_X from the PWM bridge; an
 *     external encoder would contend the line.
 *
 * ====================================================================
 * Verdict block  (find `loopback_results` in zephyr.elf via
 *   `arm-none-eabi-nm zephyr.elf | grep loopback_results`, then read
 *   the 32-word array over J-Link/SWD -- there is no console on this
 *   SoM).  Layout:
 *
 *   [0]  0xB10CBAC4  magic
 *   [1]  state: 0 = init, 1 = running, 2 = done (idle forever after)
 *   [2]  pass count
 *   [3]  fail count
 *   [4..11]   per-record status/result codes (record() cursor order):
 *             0          = PASS
 *             0x7E       = transport OK but the VALUE assertion failed
 *             other      = the failing alp_status_t, two's complement
 *   [12..15]  the four raw DAC->ADC readings (mV), one per setpoint
 *             in {150, 450, 900, 1350} order
 *   [16]  raw capture period_ns    (forensics; NOT asserted -- see below)
 *   [17]  raw capture pulse_width_ns
 *   [18]  raw qenc pos1  (cast to u32 from int32_t)
 *   [19]  raw qenc pos2  (cast to u32 from int32_t)
 *   [20..31]  reserved (0)
 * ====================================================================
 */

#include <string.h>
#include <zephyr/kernel.h>

#include "alp/chips/gd32g553.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* Verdict block                                                       */
/* ------------------------------------------------------------------ */

/* The bench reads this static array over SWD; the magic lets the
 * J-Link script confirm it has the right symbol + a coherent image
 * before it trusts the rest of the words. */
#define LOOPBACK_MAGIC 0xB10CBAC4u

/* record() writes per-test codes into [4..11]; we run three tests but
 * the DAC test emits one record per setpoint (4) plus the capture and
 * qenc records (2), so cap the cursor at the 8-slot window [4..11]. */
#define LOOPBACK_MAX_RECORDS 8u

volatile uint32_t loopback_results[32] = { LOOPBACK_MAGIC, 0u };

/* Named indices into the raw-forensics slots so the body reads cleanly
 * and the README's slot table has exactly one source of truth. */
#define SLOT_DAC_RAW_BASE 12u /* [12..15] : the four DAC->ADC readings  */
#define SLOT_CAP_PERIOD 16u   /* raw capture period_ns                 */
#define SLOT_CAP_PULSE 17u    /* raw capture pulse_width_ns            */
#define SLOT_QENC_POS1 18u    /* raw qenc pos1 (cast)                  */
#define SLOT_QENC_POS2 19u    /* raw qenc pos2 (cast)                  */

static gd32g553_t ctx;
static unsigned   record_idx; /* cursor into loopback_results[4..11] */

/* Mirror of the functional tier's record(): one call folds a transport
 * status AND a value verdict into a single result cell + bumps the
 * pass/fail tally.  A transport-OK-but-value-wrong outcome is the
 * interesting loopback failure (the link works, the copper path
 * doesn't), so it gets its own 0x7E code distinct from a wire error. */
static void record(alp_status_t s, bool value_ok)
{
	uint32_t cell;
	if (s == ALP_OK && value_ok) {
		cell = 0u;
		loopback_results[2]++;
	} else if (s == ALP_OK) {
		cell = 0x7Eu; /* status OK but the VALUE was wrong */
		loopback_results[3]++;
	} else {
		cell = (uint32_t)(int32_t)s; /* the failing status, sign-extended */
		loopback_results[3]++;
	}
	if (record_idx < LOOPBACK_MAX_RECORDS) {
		loopback_results[4u + record_idx] = cell;
	}
	record_idx++;
}

/* ------------------------------------------------------------------ */
/* Test 1: raw DAC0 -> CK_ANA -> ADC channel 0 loopback (Jumper A)     */
/* ------------------------------------------------------------------ */

/* DIRECT 1:1 LOOPBACK (bench rewire 2026-06-04).  The carrier's
 * buffered DAC output path is inoperable on this carrier revision
 * (carrier erratum, fixed next rev; rework details in the internal
 * carrier errata), so the maintainer taps the RAW DAC0 net (E1M-X pin
 * A19) straight to CK_ANA / P7.1.  DAC and ADC share the same 1.8 V
 * analog rail, so the loopback is gain-1, full-range, and physically
 * incapable of overdriving the ADC pad. */
#define LOOPBACK_GAIN 1u

/* Stay below ~1.5 V so the assertion never rides the rail-clip region
 * where DAC INL grows; this is a linearity choice, not a safety cap
 * (same-rail wiring cannot overdrive the pad). */
#define DAC_MAX_SAFE_MV 1500u

/* The four loopback setpoints -- spread across the usable range.  The
 * array order is also the [12..15] raw-slot order. */
static const uint16_t dac_setpoints_mv[4] = { 150u, 450u, 900u, 1350u };

static void           t_dac_adc_loopback(void)
{
	/* Park the DAC at 0 first so we start every sweep from a known
     * floor (defends against a stale setpoint left by a prior aborted
     * run feeding the ADC during the first settle). */
	(void)gd32g553_dac_set(&ctx, 0u, 0u);
	k_msleep(3);

	for (unsigned i = 0; i < ARRAY_SIZE(dac_setpoints_mv); ++i) {
		const uint16_t mv = dac_setpoints_mv[i];

		/* Belt-and-braces: the loop only ever holds in-range values,
         * but fence the actual hardware command so a future edit
         * cannot slip a rail-riding setpoint into the sweep. */
		const uint16_t cmd = (mv > DAC_MAX_SAFE_MV) ? DAC_MAX_SAFE_MV : mv;

		alp_status_t   s   = gd32g553_dac_set(&ctx, 0u /* DAC0 */, cmd);

		/* 3 ms settle: covers the 12-bit DAC's own settling plus the
         * jumper line's slew into the (lightly loaded) ADC pad. */
		k_msleep(3);

		uint16_t readings[4] = { 0 };
		if (s == ALP_OK) {
			/* 4 INDEPENDENT samples -- the firmware returns one mV
             * reading per requested sample, back-to-back conversions
             * with NO averaging.  The assertion takes readings[0]
             * (silicon-validated 2026-06-04: all four samples land
             * within a couple of mV of each other on the jumpered
             * loop, so any one is representative; the burst exists to
             * make a noisy/intermittent connection visible in the
             * forensic slot deltas, not to smooth it away). */
			s = gd32g553_adc_read(&ctx, 0u /* ADC channel 0 */, 4u, readings);
		}

		const uint16_t got                      = readings[0];
		loopback_results[SLOT_DAC_RAW_BASE + i] = got;

		/* Expected = the command itself (direct 1:1 wiring, both
         * converters on the same 1.8 V VREF).  Tolerance budget:
         *   +/-( 25 mV fixed + 2% of expected )
         * the 25 mV absorbs DAC + ADC offset and INL; the 2% covers
         * gain error across the converter pair.  Tighter than the old
         * buffered path because no external gain resistors remain in
         * the loop. */
		const uint32_t expected = (uint32_t)LOOPBACK_GAIN * (uint32_t)cmd;
		const uint32_t tol      = 25u + (expected * 2u) / 100u;
		const bool value_ok = (s == ALP_OK) && (got + tol >= expected) && (got <= expected + tol);

		record(s, value_ok);
	}

	/* ALWAYS park the DAC at 0 on the way out -- even if a read above
     * returned early via a failing status, we must not leave a live
     * voltage on the op-amp input. */
	(void)gd32g553_dac_set(&ctx, 0u, 0u);
}

/* ------------------------------------------------------------------ */
/* Test 2: PWM ch2 output -> PWM ch3 input-capture loopback (Jumper C) */
/* ------------------------------------------------------------------ */

/* Edge selector for gd32g553_pwm_capture_begin().  The host helper
 * takes a raw u8 (the header documents 0=rising, 1=falling, 2=both);
 * the portable mirror is ALP_PWM_CAPTURE_EDGE_BOTH == 2 in <alp/pwm.h>.
 * Both-edges is what lets us measure pulse width, not just period. */
#define PWM_CAPTURE_EDGE_BOTH 2u

static void t_pwm_capture_loopback(void)
{
	/* Stimulus: 200 Hz, 50 % duty on PWM ch2 (5 ms period, 2.5 ms
     * high).  Two deliberate choices make this robust on the SHARED-
     * timer loopback (ch2 stimulus + ch3 capture both ride TIMER0):
     *
     *   - 50 % duty: the both-edges state machine measures the delta
     *     between ADJACENT edges.  At 50 % the high time and the low
     *     time are equal (period/2 = 2.5 ms), so the measured pulse
     *     width is 2.5 ms regardless of WHICH edge (rising or falling)
     *     happened to arm the capture -- no phase ambiguity.
     *   - slow rate: at 200 Hz the edges are 2.5 ms apart, far wider
     *     than a single bridge transaction (~150 us), so the tight
     *     poll loop below reliably catches three ADJACENT edges.  At
     *     the old 1 kHz with a 5 ms retry ladder the three samples
     *     were NON-consecutive edges and the delta was meaningless. */
	alp_status_t s =
	    gd32g553_pwm_set(&ctx, 2u /* PWM ch2 */, 5000000u /* 5 ms */, 2500000u /* 2.5 ms */);

	/* Rebind PWM ch3's pin as a both-edges input-capture source.  The
     * jumper carries ch2's output into ch3's pin, so ch3 now measures
     * the stimulus we just programmed. */
	if (s == ALP_OK) {
		s = gd32g553_pwm_capture_begin(&ctx, 3u /* PWM ch3 */, PWM_CAPTURE_EDGE_BOTH);
	}

	/* 10 ms settle: a couple of stimulus periods so the capture unit
     * is latching before we start polling. */
	k_msleep(10);

	/* TIGHT poll loop -- no inter-read delay.  Each read advances the
     * firmware's edge state machine by at most one NEW edge, so to
     * walk seed -> pulse -> period the host must poll faster than the
     * 2.5 ms edge spacing (a ~150 us transaction easily does).
     * NOSUPPORT = "no fresh edge yet, poll again" (documented ring-
     * empty sentinel); up to 80 reads (~12 ms) covers several edges. */
	uint32_t     period_ns = 0, pulse_ns = 0;
	alp_status_t cap = ALP_ERR_NOSUPPORT;
	if (s == ALP_OK) {
		for (unsigned attempt = 0; attempt < 80u; ++attempt) {
			cap = gd32g553_pwm_capture_read(&ctx, 3u, &period_ns, &pulse_ns);
			if (cap != ALP_ERR_NOSUPPORT) {
				break; /* got a real status (OK or a hard error) */
			}
		}
		s = cap;
	}

	/* Record the raw measurements regardless of verdict -- the bench
     * reads these to tighten tolerances from silicon truth. */
	loopback_results[SLOT_CAP_PERIOD] = period_ns;
	loopback_results[SLOT_CAP_PULSE]  = pulse_ns;

	/* Assert ONLY the pulse width: 2.5 ms +/- 100 us -> [2.4, 2.6] ms.
     *
     * We deliberately do NOT assert the period.  Reason: the stimulus
     * (ch2) and the capture (ch3) live on the SAME advanced timer
     * (TIMER0).  The captured "period" is the same-edge delta, which on
     * a shared timer is exactly one counter wrap and reads ~0 -- a
     * documented degeneracy, not a fault.  The pulse width is the
     * ADJACENT-edge delta and IS a meaningful loopback check. */
	const bool value_ok = (s == ALP_OK) && (pulse_ns >= 2400000u) && (pulse_ns <= 2600000u);
	record(s, value_ok);

	/* Tear down capture mode, then park ch2's output (period kept,
     * duty 0 -> channel idle).  capture_end frees ch3's pin for a
     * future output re-open. */
	(void)gd32g553_pwm_capture_end(&ctx, 3u);
	(void)gd32g553_pwm_set(&ctx, 2u, 1000000u, 0u);
}

/* ------------------------------------------------------------------ */
/* Test 3: PWM ch1 -> ENC1_X stimulus into quadrature decoder (Jumper B)*/
/* ------------------------------------------------------------------ */

static void t_pwm_qenc_stimulus(void)
{
	/* Zero the encoder accumulator so pos1/pos2 are measured from a
     * known origin. */
	alp_status_t s = gd32g553_qenc_reset(&ctx, 1u /* encoder index 1 */);

	/* Drive ENC1_X with a 1 kHz, 50% square (1 ms period, 500 us duty)
     * from PWM ch1.  ENC1_Y (PC7) floats with a firmware pull-up ->
     * static HIGH, so only the X channel toggles. */
	if (s == ALP_OK) {
		s = gd32g553_pwm_set(&ctx, 1u /* PWM ch1 */, 1000000u, 500000u);
	}

	/* Let the stimulus run, then take two spaced reads to confirm the
     * count is BOUNDED (not free-running). */
	k_msleep(100);
	int32_t      pos1 = 0;
	alp_status_t s1   = gd32g553_qenc_read(&ctx, 1u, &pos1);

	k_msleep(10);
	int32_t      pos2 = 0;
	alp_status_t s2   = gd32g553_qenc_read(&ctx, 1u, &pos2);

	/* Park the stimulus (duty 0 -> ch1 idle) BEFORE we evaluate, so the
     * line is quiet no matter which way the verdict goes. */
	(void)gd32g553_pwm_set(&ctx, 1u, 1000000u, 0u);

	loopback_results[SLOT_QENC_POS1] = (uint32_t)pos1;
	loopback_results[SLOT_QENC_POS2] = (uint32_t)pos2;

	/* Physics, honestly: in X4 quadrature decode with Y held static
     * HIGH, a lone toggling X cannot accumulate net position -- each
     * X edge with an unchanging Y is an illegal/ambiguous transition
     * the decoder treats as +/-1 DITHER about the origin, with no
     * directional bias.  So |pos| should stay tiny.  The bound is set
     * LOOSE (<= 8) for this first silicon pass: it is here to catch the
     * failure mode this loopback guards against -- a genuinely floating
     * ENC input free-ran to THOUSANDS of counts.  The raw pos1/pos2 are
     * recorded so the bound can be tightened from silicon truth later. */
	const bool ok_status = (s == ALP_OK) && (s1 == ALP_OK) && (s2 == ALP_OK);
	const bool bounded   = (pos1 <= 8 && pos1 >= -8) && (pos2 <= 8 && pos2 >= -8);

	/* Fold the worst transport status into record() so a wire error is
     * reported as itself rather than masquerading as a value miss. */
	alp_status_t worst = (s != ALP_OK) ? s : (s1 != ALP_OK) ? s1 : s2;
	record(worst, ok_status && bounded);
}

/* ------------------------------------------------------------------ */
/* The single-pass suite                                               */
/* ------------------------------------------------------------------ */

static void run_suite(void)
{
	t_dac_adc_loopback();     /* Jumper A: raw DAC0 -> CK_ANA -> ADC0    */
	t_pwm_capture_loopback(); /* Jumper C: PWM ch2 -> PWM ch3 capture    */
	t_pwm_qenc_stimulus();    /* Jumper B: PWM ch1 -> ENC1_X decode      */
}

/* ------------------------------------------------------------------ */
/* Entry                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 1u,
	    .freq_hz       = 25000000u,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8u,
	    .cs_pin_id     = ALP_SPI_NO_CS, /* platform SPI driver owns CS */
	});
	if (spi == NULL) {
		loopback_results[1] = 0xDEADu;
		return 1;
	}

	/* Cold-boot autonomous: retry until the GD32 answers (shared PMIC
     * reset-out means the supervisor may still be coming up). */
	alp_status_t s;
	do {
		s = gd32g553_init(&ctx, spi, NULL, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
		if (s != ALP_OK) k_msleep(200);
	} while (s != ALP_OK);

	/* Settle past the host's boot window (same rationale as the soak +
     * functional tiers: A55 storage/pinmux bring-up can glitch shared
     * board state, and we want the analog rails quiet before the DAC
     * sweep). */
	k_msleep(20000);

	loopback_results[1] = 1u; /* running */
	run_suite();
	loopback_results[1] = 2u; /* done */

	/* Single pass, then idle forever -- the bench reads the verdict
     * block over SWD at its leisure.  All stimuli were parked at 0 by
     * each test's exit path, so nothing is driving the jumpered lines
     * while we sleep. */
	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
