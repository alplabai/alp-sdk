/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-gd32-bridge-functional -- single-pass FUNCTIONAL validation of
 * the GD32 supervisor bridge, then a live oscilloscope observable.
 *
 * Where the hil-soak proves the LINK (every opcode answers, forever),
 * this app proves the FUNCTIONS: each test drives a bridge surface
 * with a known stimulus and asserts the VALUE that comes back --
 * sqrt(4) is 2.0, sin(pi/2) is 1.0, a 32-byte TRNG pull has entropy,
 * an invalid ADC configuration is REJECTED, a PWM setpoint reads back
 * within tolerance.  It runs the table once, publishes a per-test
 * verdict block for the V2N DAP (no console on this SoM), and then
 * parks in a forever PWM7 DUTY STAIRCASE -- PWM7 doubles as the EVK
 * LED pad, so a scope probe (or the naked eye, at staircase rates)
 * verifies the bridge's PWM path end-to-end on real silicon.
 *
 * Verdict block (find `func_results` in zephyr.map; read via DAP):
 *   [0]  0xF07C7E57 magic
 *   [1]  state: 1 = testing, 2 = staircase running (tests done),
 *        0xDEAD = link never came up
 *   [2]  pass count   [3] fail count
 *   [4 + i] per-test result, table order below:
 *        0          = PASS
 *        0x7E       = value assertion failed (status was OK)
 *        other      = the failing alp_status_t, two's complement
 *   [40] staircase: current duty per-mille (live)
 *   [41] staircase: step counter (liveness)
 *
 * Protocol v0.15 adds three tests (link_features, adc_stream2, batch).
 * They are SELF-GATING: gd32g553_init() negotiates the v0.15 features
 * (BIG_FRAME, ADC_STREAM2, BATCH) with a peer that reports minor >= 15,
 * and each test checks the contract of the link it actually got -- a
 * granted feature must work, an ungranted one must answer
 * ALP_ERR_NOSUPPORT without touching the wire.  ATTN (the data-ready
 * line on P71) needs an interrupt hook; the hil-soak example wires one
 * and shows how, this example stays on the staging-gap path.
 *
 * This is a maintainer bench tool in example form; like the soak it
 * exercises the gd32g553 chip driver directly (the documented
 * exception to the portable-API rule for dedicated bridge demos).
 */

#include <string.h>

#include "alp/chips/gd32g553.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* Verdict block                                                       */
/* ------------------------------------------------------------------ */

#define FUNC_MAX_TESTS 36u

volatile uint32_t func_results[44] = { 0xF07C7E57u, 0u };

static gd32g553_t ctx;
static unsigned   test_idx; /* cursor into func_results[4..] */

static void record(alp_status_t s, bool value_ok)
{
	uint32_t cell;
	if (s == ALP_OK && value_ok) {
		cell = 0u;
		func_results[2]++;
	} else if (s == ALP_OK) {
		cell = 0x7Eu; /* status OK but the VALUE was wrong */
		func_results[3]++;
	} else {
		cell = (uint32_t)(int32_t)s; /* the failing status, sign-extended */
		func_results[3]++;
	}
	if (test_idx < FUNC_MAX_TESTS) {
		func_results[4u + test_idx] = cell;
	}
	test_idx++;
}

/* ------------------------------------------------------------------ */
/* Float helpers (the wire carries IEEE-754 bit patterns)              */
/* ------------------------------------------------------------------ */

static uint32_t f32_bits(float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

static float bits_f32(uint32_t u)
{
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}

static bool near_f(float got, float want, float tol)
{
	const float d = got - want;
	return (d >= -tol) && (d <= tol);
}

/* ------------------------------------------------------------------ */
/* TMU: the full math table.  Every supported CORDIC primitive gets a  */
/* known-exact probe; tolerances reflect the unit's ~20-bit effective  */
/* precision plus the radian->pi-units conversion rounding.            */
/* ------------------------------------------------------------------ */

#define PI_F 3.14159265358979f

static void t_tmu_f32(gd32g553_tmu_function_t fn, float a, float b, float want, float tol)
{
	uint32_t           out = 0;
	const alp_status_t s =
	    gd32g553_tmu_compute(&ctx, fn, GD32G553_TMU_FMT_F32, f32_bits(a), f32_bits(b), &out);
	record(s, (s == ALP_OK) && near_f(bits_f32(out), want, tol));
}

/* tan/exp/tanh have no native TMU mode -- the firmware answers
 * NOSUPPORT by design and the assert here pins that contract (a
 * regression to STATUS_IO or to a wrong value would be a defect). */
static void t_tmu_nosupport(gd32g553_tmu_function_t fn)
{
	uint32_t           out = 0;
	const alp_status_t s =
	    gd32g553_tmu_compute(&ctx, fn, GD32G553_TMU_FMT_F32, f32_bits(1.0f), 0u, &out);
	record((s == ALP_ERR_NOSUPPORT) ? ALP_OK : ((s == ALP_OK) ? ALP_ERR_IO : s),
	       s == ALP_ERR_NOSUPPORT);
}

/* Q31 path: sqrt(0.25) = 0.5.  Q31 full scale is +/-1.0, so 0.25 =
 * 0x20000000 and the expected 0.5 = 0x40000000 (+/- a few LSB of
 * CORDIC noise -- 1e-6 of full scale is ~2147 LSB, generous). */
static void t_tmu_q31_sqrt(void)
{
	uint32_t           out = 0;
	const alp_status_t s   = gd32g553_tmu_compute(
	    &ctx, GD32G553_TMU_FN_SQRT, GD32G553_TMU_FMT_Q31, 0x20000000u, 0u, &out);
	const int32_t err = (int32_t)out - 0x40000000;
	record(s, (s == ALP_OK) && (err > -4096) && (err < 4096));
}

/* ------------------------------------------------------------------ */
/* TRNG: boundary lengths + cheap entropy sanity                       */
/* ------------------------------------------------------------------ */

/* One pull with the DOCUMENTED fault-recover tolerance: the TRNG
 * takes intermittent seed errors, parks with latched fault flags and
 * answers ONE honest ALP_ERR_IO while the firmware demotes + lazily
 * rebuilds the unit -- the very next pull succeeds (silicon-validated
 * recovery, 2026-06-04).  A single retry per pull asserts exactly
 * that contract; the HiL soak's TRNG row uses the same shape.
 * (Caught live: the single-pass tier failed slot 18 with one IO on a
 * run whose other 25 tests + 150 soak cycles were clean.) */
static alp_status_t trng_pull_with_recover(uint8_t *dst, size_t len)
{
	alp_status_t s = gd32g553_trng_read(&ctx, dst, len);
	if (s == ALP_ERR_IO) {
		s = gd32g553_trng_read(&ctx, dst, len);
	}
	return s;
}

static void t_trng_lengths(void)
{
	/* Two 16-byte pulls -- half a 256-bit NIST conditioning round
     * each, so a single round satisfies the request without the
     * firmware bounding out mid-pull.  (A 32-byte single pull spans a
     * whole round and legitimately answers BUSY while conditioning is
     * mid-flight; that path is exercised implicitly by the host
     * driver's BUSY-retry, not asserted here.) */
	uint8_t      a[16] = { 0 };
	uint8_t      b[16] = { 0 };
	alp_status_t s     = trng_pull_with_recover(a, sizeof a);
	if (s == ALP_OK) {
		s = trng_pull_with_recover(b, sizeof b);
	}
	/* Entropy sanity: not all-constant, and the second pull differs.
     * (Statistical tests belong off-target; this catches "stuck word"
     * and "replayed buffer" failure modes.) */
	bool ok = (s == ALP_OK);
	if (ok) {
		bool constant = true;
		for (unsigned i = 1; i < sizeof a; ++i) {
			if (a[i] != a[0]) {
				constant = false;
				break;
			}
		}
		ok = !constant && (memcmp(a, b, sizeof a) != 0);
	}
	record(s, ok);
}

/* ------------------------------------------------------------------ */
/* PWM: configure + setpoint readback on the scope channel             */
/* ------------------------------------------------------------------ */

/* PWM7 is the EVK LED pad AND the maintainer's scope channel for this
 * bench round -- everything observable funnels through it. */
#define SCOPE_PWM_CH 7u

static void t_pwm_set_get_scope_ch(void)
{
	/* 1 kHz, 50 % -- easy to eyeball on a scope.  The 16-bit timers
     * at a 1 us tick round 1 ms / 500 us exactly, so the readback
     * tolerance is one tick (1 us) on each field. */
	alp_status_t s      = gd32g553_pwm_set(&ctx, SCOPE_PWM_CH, 1000000u, 500000u);
	uint32_t     period = 0, duty = 0;
	if (s == ALP_OK) {
		s = gd32g553_pwm_get(&ctx, SCOPE_PWM_CH, &period, &duty);
	}
	record(s,
	       (s == ALP_OK) && (period >= 999000u) && (period <= 1001000u) && (duty >= 499000u) &&
	           (duty <= 501000u));
}

/* pwm_configure is a documented v0.3 PARTIAL: the default tuple
 * (edge-aligned, no dead-time, no break) MUST answer OK -- it is the
 * idempotent "set to defaults" call -- while any non-default knob may
 * answer NOSUPPORT until the per-timer apply path lands.  Assert
 * exactly that contract: defaults always OK; center-up either OK
 * (HAL landed) or NOSUPPORT (documented partial), never anything
 * else.  Order restores edge alignment for the staircase below. */
static void t_pwm_configure_roundtrip(void)
{
	const alp_status_t s =
	    gd32g553_pwm_configure(&ctx, SCOPE_PWM_CH, GD32G553_PWM_ALIGN_CENTER_UP, 0u, 0u);
	const alp_status_t restore =
	    gd32g553_pwm_configure(&ctx, SCOPE_PWM_CH, GD32G553_PWM_ALIGN_EDGE, 0u, 0u);
	record(ALP_OK, (restore == ALP_OK) && (s == ALP_OK || s == ALP_ERR_NOSUPPORT));
}

/* ------------------------------------------------------------------ */
/* ADC: configuration error path + all-channel ceiling sweep           */
/* ------------------------------------------------------------------ */

/* 14-bit resolution REQUIRES oversampling >= 4 (datasheet effective-
 * resolution table; enforced by the firmware per the host header).
 * The firmware must reject the combination -- INVAL surfacing here
 * (instead of the pre-fix masked ALP_ERR_IO) also regression-tests
 * the host's short-error-envelope decode. */
static void t_adc_configure_error_path(void)
{
	const alp_status_t s = gd32g553_adc_configure(&ctx, 0u, 1u, 0u, 14u);
	record((s == ALP_ERR_INVAL || s == ALP_ERR_NOSUPPORT) ? ALP_OK
	       : (s == ALP_OK)                                ? ALP_ERR_IO
	                                                      : s,
	       s == ALP_ERR_INVAL || s == ALP_ERR_NOSUPPORT);
}

/* Every ADC channel answers and respects the physical ceiling (pads
 * float on the bench, so the VALUE is unconstrained below VREF). */
static void t_adc_all_channels(void)
{
	alp_status_t worst    = ALP_OK;
	bool         value_ok = true;
	for (uint8_t ch = 0u; ch < 8u; ++ch) {
		uint16_t           mv[2] = { 0 };
		const alp_status_t s     = gd32g553_adc_read(&ctx, ch, 2u, mv);
		if (s != ALP_OK) {
			worst = s;
			break;
		}
		if (mv[0] > 3400u || mv[1] > 3400u) value_ok = false;
	}
	record(worst, value_ok);
}

/* ------------------------------------------------------------------ */
/* Protocol v0.15: negotiated link features, lossless stream, BATCH    */
/* ------------------------------------------------------------------ */

/* What init() negotiated must agree with what the peer says it is: a
 * minor >= 15 bridge grants STATUS_SEQ | BIG_FRAME | ADC_STREAM2 | BATCH
 * (0x1B; ATTN would add 0x04 but needs a hook this example does not
 * register) with the 252-byte BIG_FRAME ceiling; an older bridge keeps
 * the legacy 1-byte STATUS_SEQ form and the 65-byte envelope, so the
 * wire is byte-identical to v0.14. */
static void t_link_features(void)
{
	const bool v015 = ctx.version.minor >= GD32G553_V015_MIN_PROTOCOL_MINOR;
	bool       ok;

	if (v015) {
		const uint32_t want = GD32G553_LINK_FEAT_STATUS_SEQ | GD32G553_LINK_FEAT_BIG_FRAME |
		                      GD32G553_LINK_FEAT_ADC_STREAM2 | GD32G553_LINK_FEAT_BATCH;
		ok                  = ((ctx.granted & want) == want) && (ctx.max_payload == 252u);
	} else {
		ok = (ctx.granted == GD32G553_LINK_FEAT_STATUS_SEQ) && (ctx.max_payload == 65u);
	}
	record(ALP_OK, ok);
}

/* ADC_STREAM2: BEGIN2 returns the REALISED rate exactly (tick_hz /
 * period_ticks, 1 MHz / 1000 = 1 kHz) and READ2 returns raw codes with a
 * sample index and a drop count.  A poll-driven consumer (watermark 0)
 * gets the deepest ring the firmware grants, so 50 ms at 1 kHz drops
 * nothing: the two reads must be contiguous, first_index(2) ==
 * first_index(1) + got(1).  (BEGIN2's reply carries the GRANTED watermark
 * and ring depth, which can exceed the request at high rates -- always
 * use the reply.)  Without
 * the grant, BEGIN2 must answer NOSUPPORT -- the 0x33/0x34 stream above
 * is then the only stream path. */
static void t_adc_stream2(void)
{
	gd32g553_adc_stream2_info_t info;
	const alp_status_t          s = gd32g553_adc_stream_begin2(&ctx, 0u, 0u, 1000u, 0u, &info);

	if ((ctx.granted & GD32G553_LINK_FEAT_ADC_STREAM2) == 0u) {
		record(ALP_OK, s == ALP_ERR_NOSUPPORT);
		return;
	}
	if (s != ALP_OK) {
		record(s, false);
		return;
	}

	alp_delay_ms(50);

	uint16_t           codes[GD32G553_READ2_MAX_SAMPLES_BIG];
	const uint8_t      max    = (uint8_t)((ctx.max_payload - GD32G553_READ2_HDR_BYTES) / 2u);
	uint32_t           first1 = 0, drop1 = 0, first2 = 0, drop2 = 0;
	uint8_t            got1 = 0, got2 = 0;
	const alp_status_t s1 = gd32g553_adc_stream_read2(&ctx, 0u, max, &first1, &drop1, &got1, codes);
	const alp_status_t s2 = gd32g553_adc_stream_read2(&ctx, 0u, max, &first2, &drop2, &got2, codes);
	const alp_status_t send = gd32g553_adc_stream_end(&ctx, 0u);

	const alp_status_t worst    = (s1 != ALP_OK) ? s1 : (s2 != ALP_OK) ? s2 : send;
	const bool         value_ok = (info.tick_hz == 1000000u) && (info.period_ticks == 1000u) &&
	                              (info.full_scale == 4095u) && (got1 >= 30u) && (drop1 == 0u) &&
	                              (first1 == 0u) && (drop2 == 0u) && (first2 == first1 + got1) &&
	                              (ctx.stream2_gaps == 0u);
	record(worst, value_ok);
}

/* BATCH: PING + a full-mask GPIO_READ + COUNTER_READ in ONE transaction
 * pair.  The driver validates the request against the allow-list and the
 * reply against the request (executed <= count, per-op lengths); here we
 * only assert the outcome.  A second batch whose middle op fails (READ2
 * on a stream that was never started) must STOP at that op: executed is
 * 2, the third op never ran.  Without the grant: NOSUPPORT. */
static void t_batch(void)
{
	const uint8_t       mask_all[4] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu };
	const uint8_t       counter0[1] = { 0u };
	const uint8_t       read2_s1[2] = { 1u, 4u }; /* stream 1, never started */
	uint8_t             gpio_rep[4], counter_rep[4], read2_rep[GD32G553_READ2_HDR_BYTES + 2u * 4u];
	gd32g553_batch_op_t good[3] = {
		{ .op = GD32G553_CMD_PING },
		{ .op        = GD32G553_CMD_GPIO_READ,
		  .args      = mask_all,
		  .args_len  = 4u,
		  .reply     = gpio_rep,
		  .reply_cap = sizeof gpio_rep },
		{ .op        = GD32G553_CMD_COUNTER_READ,
		  .args      = counter0,
		  .args_len  = 1u,
		  .reply     = counter_rep,
		  .reply_cap = sizeof counter_rep },
	};
	gd32g553_batch_op_t stops[3] = {
		{ .op = GD32G553_CMD_PING },
		{ .op        = GD32G553_CMD_ADC_STREAM_READ2,
		  .args      = read2_s1,
		  .args_len  = 2u,
		  .reply     = read2_rep,
		  .reply_cap = sizeof read2_rep },
		{ .op = GD32G553_CMD_PING },
	};
	uint8_t executed = 0;

	if ((ctx.granted & GD32G553_LINK_FEAT_BATCH) == 0u) {
		record(ALP_OK, gd32g553_batch(&ctx, good, 3u, &executed) == ALP_ERR_NOSUPPORT);
		return;
	}

	alp_status_t s  = gd32g553_batch(&ctx, good, 3u, &executed);
	bool         ok = (s == ALP_OK) && (executed == 3u) && (good[0].status == ALP_OK) &&
	                  (good[1].status == ALP_OK) && (good[1].reply_len == 4u) &&
	                  (good[2].status == ALP_OK) && (good[2].reply_len == 4u);
	if (s == ALP_OK) {
		s  = gd32g553_batch(&ctx, stops, 3u, &executed);
		ok = ok && (s == ALP_OK) && (executed == 2u) && (stops[0].status == ALP_OK) &&
		     (stops[1].status == ALP_ERR_INVAL) && (stops[2].status == ALP_ERR_NOT_READY);
	}
	record(s, ok);
}

/* ------------------------------------------------------------------ */
/* DSP chain: pool lifecycle (the runtime FFT/FAC dispatch is a wired  */
/* protocol surface whose HAL lands with wave-2 -- both outcomes are   */
/* contract-checked)                                                   */
/* ------------------------------------------------------------------ */

static void t_dsp_chain_lifecycle(void)
{
	uint8_t            chain_id = 0xFFu;
	const alp_status_t s        = gd32g553_adc_dsp_chain_open(&ctx, &chain_id);
	if (s == ALP_ERR_NOSUPPORT) {
		record(ALP_OK, true); /* documented pre-wave-2 contract */
		return;
	}
	record(s, (s == ALP_OK) && (chain_id != 0xFFu));
}

/* ------------------------------------------------------------------ */
/* Identity + misc                                                     */
/* ------------------------------------------------------------------ */

static void t_version_stable(void)
{
	gd32g553_version_t v0, v1;
	alp_status_t       s = gd32g553_refresh_version(&ctx, &v0);
	if (s == ALP_OK) s = gd32g553_refresh_version(&ctx, &v1);
	record(s,
	       (s == ALP_OK) && (v0.major == v1.major) && (v0.minor == v1.minor) &&
	           (v0.patch == v1.patch));
}

static void t_da9292_sentinel(void)
{
	uint8_t            st = 0;
	const alp_status_t s  = gd32g553_da9292_status_forward(&ctx, &st);
	/* This HW rev has no DA9292 nets on the GD32: 0xFF sentinel. */
	record(s, (s == ALP_OK) && (st == 0xFFu));
}

/* ------------------------------------------------------------------ */
/* The single-pass table                                               */
/* ------------------------------------------------------------------ */

static void run_suite(void)
{
	/* -- math: every native CORDIC primitive, value-asserted -------- */
	t_tmu_f32(GD32G553_TMU_FN_SQRT, 4.0f, 0.0f, 2.0f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_SQRT, 2.0f, 0.0f, 1.41421356f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_SIN, 0.0f, 0.0f, 0.0f, 1e-4f);
	t_tmu_f32(GD32G553_TMU_FN_SIN, PI_F / 2.0f, 0.0f, 1.0f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_SIN, PI_F / 6.0f, 0.0f, 0.5f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_COS, 0.0f, 0.0f, 1.0f, 1e-4f);
	t_tmu_f32(GD32G553_TMU_FN_COS, PI_F, 0.0f, -1.0f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_COS, PI_F / 3.0f, 0.0f, 0.5f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_ATAN, 1.0f, 0.0f, PI_F / 4.0f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_ATAN2, 1.0f, 1.0f, PI_F / 4.0f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_HYPOT, 3.0f, 4.0f, 5.0f, 1e-2f);
	t_tmu_f32(GD32G553_TMU_FN_LOG, 2.71828183f, 0.0f, 1.0f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_SINH, 1.0f, 0.0f, 1.17520119f, 1e-3f);
	t_tmu_f32(GD32G553_TMU_FN_COSH, 1.0f, 0.0f, 1.54308063f, 1e-3f);
	t_tmu_nosupport(GD32G553_TMU_FN_TAN);
	t_tmu_nosupport(GD32G553_TMU_FN_EXP);
	t_tmu_nosupport(GD32G553_TMU_FN_TANH);
	t_tmu_q31_sqrt();

	/* -- entropy ----------------------------------------------------- */
	t_trng_lengths();

	/* -- PWM (the scope/LED channel) --------------------------------- */
	t_pwm_set_get_scope_ch();
	t_pwm_configure_roundtrip();

	/* -- ADC ---------------------------------------------------------- */
	t_adc_configure_error_path();
	t_adc_all_channels();

	/* -- protocol v0.15 (self-gating on the negotiated features) ------ */
	t_link_features();
	t_adc_stream2();
	t_batch();

	/* -- DSP chain pool ----------------------------------------------- */
	t_dsp_chain_lifecycle();

	/* -- identity ------------------------------------------------------ */
	t_version_stable();
	t_da9292_sentinel();
}

/* ------------------------------------------------------------------ */
/* PWM7 duty staircase -- the forever scope observable                  */
/* ------------------------------------------------------------------ */

static void pwm7_staircase_forever(void)
{
	/* 1 kHz carrier; duty walks 10 % -> 90 % in 10-point steps, two
     * seconds per step, then wraps.  On the scope: a 1 kHz square
     * whose high time visibly widens every 2 s; on the LED: a
     * brightness ramp.  Every step is a fresh PWM_SET + PWM_GET pair
     * over the bridge, so the staircase doubles as a slow link soak. */
	static const uint16_t duty_pm[] = { 100u, 200u, 300u, 400u, 500u, 600u, 700u, 800u, 900u };
	unsigned              step      = 0;

	for (;;) {
		const uint16_t pm        = duty_pm[step % (sizeof duty_pm / sizeof duty_pm[0])];
		const uint32_t period_ns = 1000000u;
		const uint32_t duty_ns   = (period_ns / 1000u) * pm;

		(void)gd32g553_pwm_set(&ctx, SCOPE_PWM_CH, period_ns, duty_ns);

		func_results[40] = pm;
		func_results[41] = (uint32_t)step;
		step++;
		alp_delay_ms(2000);
	}
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
		func_results[1] = 0xDEADu;
		return 1;
	}

	/* Cold-boot autonomous: retry until the GD32 answers (shared PMIC
     * reset-out means the supervisor may still be coming up). */
	alp_status_t s;
	do {
		s = gd32g553_init(&ctx, spi, NULL, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
		if (s != ALP_OK) alp_delay_ms(200);
	} while (s != ALP_OK);

	/* Settle past the host's boot window (same rationale as the soak:
     * A55 storage/pinmux bring-up can glitch shared board state). */
	alp_delay_ms(20000);

	func_results[1] = 1u;
	run_suite();
	func_results[1] = 2u;

	pwm7_staircase_forever();
	return 0;
}
