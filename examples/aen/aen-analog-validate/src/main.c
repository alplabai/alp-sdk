/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-analog-validate -- on-silicon DAC0 -> ADC loopback validation for the
 * E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
 *
 * UNLIKE aen-dac-regcheck / aen-adc-regcheck (which validate that the drivers
 * PROGRAM the registers, with no analog path), this app closes the analog loop:
 * it drives a known code on dac0 and reads it back through an adc12 instance,
 * then asserts the readback matches the DAC setpoint scaled by the two
 * converters' VREF RATIO.
 *
 * Why a RATIO check is the right PASS gate here
 * ---------------------------------------------
 * The two converters run at DIFFERENT, register-fixed references:
 *   - DAC0: the dac_alif driver fixes DAC12_VREF_CONT = 0x4 = 0.750 V full-scale
 *           (dac_alif.c CMP_COMP_REG2_DAC12_VREF_CONT = (0x4U << 17), ==
 *           hal_alif analog_ctrl.h:41; the analog_ctrl.h:31-40 table reads
 *           code 0x4 (100b) = 0.750 V).
 *   - ADC:  the adc_alif driver fixes ADC_VREF_CONT = 0x10 with the reference
 *           divider OFF (ADC_VREF_BUF_RDIV_EN = (0x0U << 16) -> RDIV=0 = 1.8 V;
 *           adc_alif.c ADC_VREF_CONT = (0x10U << 10) / ADC_VREF_BUF_RDIV_EN =
 *           (0x0U << 16), == hal_alif analog_ctrl.h:63 / :46).  So ADC
 *           full-scale = 1.8 V.
 *
 * A DAC code C produces an ideal pad voltage:
 *      V = C * Vdac_ref / DAC_FS              (Vdac_ref = 0.750 V, DAC_FS = 0xFFF)
 * and the ADC digitises that voltage to:
 *      raw = V * ADC_FS / Vadc_ref            (Vadc_ref = 1.8 V, ADC_FS = 4096)
 * Combining (the voltage CANCELS as a unit, so the result depends only on the
 * register-grounded VREF RATIO, not on the absolute pad voltage):
 *      expected_raw = C * Vdac_ref * ADC_FS / (DAC_FS * Vadc_ref)
 *
 * This is TRM-INDEPENDENT in the sense that it needs only the two VREF register
 * codes (both verified vs hal_alif), NOT the absolute pad accuracy from the TRM.
 * It does NOT, and cannot, validate absolute mV accuracy: the DAC output buffer
 * offset/gain, the ADC input buffer, source impedance and trim all shift the
 * absolute reading -- so the tolerance is deliberately GENEROUS and the gate
 * is "the loopback tracks the VREF ratio", not "the reading is accurate to N mV".
 *
 * BENCH JUMPER REQUIRED: wire the DAC0 pad (E1M A16 = Alif P2_2 = DAC_0, per
 * metadata/e1m_modules/aen/from-alif.tsv) to the ANA_S input pad routed to the
 * ADC instance/channel under test.  The exact ANA_S->instance/channel map is a
 * TRM detail not in the fetched fork source -- see README.md + the overlay.  The
 * defaults below (adc12_0, channel 0) are a PLAUSIBLE PICK, not a verified route.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/printk.h>

/* --- DAC side (source) --------------------------------------------------- */

#define DAC_NODE DT_NODELABEL(dac0)

/* 12-bit DAC; channel_setup rejects any resolution != 12. */
#define DAC_RESOLUTION 12U
#define DAC_FS_CODE    ((1U << DAC_RESOLUTION) - 1U) /* 0xFFF full-scale code */
#define DAC_CHANNEL    0U

/*
 * DAC full-scale reference (mV) from DT `alif,reference-mv`.  The dac_alif driver
 * fixes DAC12_VREF_CONT=0x4 = 0.750 V (analog_ctrl.h:41), so the dtsi sets
 * alif,reference-mv=750; default 750 if the prop is ever dropped.  Used both for
 * the REPORTED ideal pad voltage and as the numerator of the VREF ratio.
 */
#define DAC_REFERENCE_MV ((uint32_t)DT_PROP_OR(DAC_NODE, alif_reference_mv, 750))

/*
 * Test code: ~mid-scale (0x800).  Mid-scale keeps the loopback well away from
 * both rails (0 V and the buffer's near-rail compression), where the ratio
 * relationship is cleanest.
 */
#define DAC_TEST_CODE (DAC_FS_CODE / 2U + 1U) /* 0x800 */

/* --- ADC side (sink) ----------------------------------------------------- */

/*
 * TBD (bench unknown): the ADC instance + channel the DAC0 jumper lands on is a
 * TRM detail not in the fork source.  adc12_0 / channel 0 is the plausible
 * default (ANA_S0 = A15 = P0_0, lowest-numbered sense pad).  Change BOTH the
 * node-label here and the overlay's enabled instance together if the route
 * differs.  See README.md.
 */
#define ADC_NODE         DT_NODELABEL(adc12_0)
#define ADC_TEST_CHANNEL 0U

/* 12-bit ADC code span (2^12 = 4096 quanta). */
#define ADC_RESOLUTION 12U
#define ADC_FS_SPAN    (1U << ADC_RESOLUTION) /* 4096 */
#define ADC_CODE_MASK  (ADC_FS_SPAN - 1U)

/*
 * ADC full-scale reference (mV).  Driver-fixed to ADC_VREF_CONT=0x10, RDIV=0 =
 * 1.8 V (analog_ctrl.h:46/63).  Used for the REPORTED measured voltage and as
 * the denominator of the VREF ratio.  Not a DT-read value (the adc_alif driver
 * does not consume alif,reference-mv); the adc binding documents it for
 * reporting only.
 */
#define ADC_REFERENCE_MV 1800U

/*
 * PASS tolerance, in ADC LSB.  The check is a VREF-ratio MATCH, not an absolute
 * accuracy spec -- buffer offset/gain, source impedance and trim all shift the
 * reading -- so the window is generous: +/-256 LSB = +/-1/16 of the 4096-code
 * span = ~+/-112 mV at the 1.8 V ADC reference.  This still catches a gross
 * failure (wrong ratio, dead DAC, open jumper, wrong channel -> floating pad)
 * while not flagging a real but offset/gain-shifted loopback.  Bench may tighten
 * this once the converters are characterised.  TBD: exact bench-verified window.
 */
#define ADC_TOLERANCE_LSB 256

static inline uint32_t imuldiv(uint32_t v, uint32_t mul, uint32_t div)
{
	/* 64-bit intermediate to avoid 32-bit overflow on code*mul. */
	return (uint32_t)(((uint64_t)v * (uint64_t)mul) / (uint64_t)div);
}

int main(void)
{
	const struct device *dac = DEVICE_DT_GET(DAC_NODE);
	const struct device *adc = DEVICE_DT_GET(ADC_NODE);

	printk("\n=== aen-analog-validate (DAC0 -> ADC loopback) ===\n");
	printk("dac node   : %s  (VREF %u mV, fixed DAC12_VREF_CONT=0x4)\n",
	       DT_NODE_FULL_NAME(DAC_NODE),
	       DAC_REFERENCE_MV);
	printk("adc node   : %s  (VREF %u mV, fixed ADC_VREF_CONT=0x10 RDIV=0)\n",
	       DT_NODE_FULL_NAME(ADC_NODE),
	       ADC_REFERENCE_MV);
	printk("jumper     : DAC0 pad (A16/P2_2) -> ADC ch%u  [INSTANCE/CH is TBD]\n",
	       ADC_TEST_CHANNEL);

	if (!device_is_ready(dac)) {
		printk("RESULT FAIL: dac device not ready (init/clock/VREF failed)\n");
		return 0;
	}
	if (!device_is_ready(adc)) {
		printk("RESULT FAIL: adc device not ready (init/clock/VREF failed)\n");
		return 0;
	}

	/* --- drive the DAC -------------------------------------------------- */
	struct dac_channel_cfg dac_cfg = {
		.channel_id = DAC_CHANNEL,
		.resolution = DAC_RESOLUTION,
		.buffered   = false,
	};

	int rc_dac_setup = dac_channel_setup(dac, &dac_cfg);
	printk("dac_channel_setup rc = %d\n", rc_dac_setup);

	int rc_dac_write = dac_write_value(dac, DAC_CHANNEL, DAC_TEST_CODE);
	printk("dac_write_value(code=0x%03x) rc = %d\n", DAC_TEST_CODE, rc_dac_write);

	/* Let the DAC output buffer + jumper RC settle before sampling.  The exact
	 * settling time is load-dependent (bench unknown); 5 ms is comfortably long
	 * for a short jumper into the ADC input. */
	k_msleep(5);

	/* --- read it back on the ADC ---------------------------------------- */
	struct adc_channel_cfg adc_cfg = {
		.gain             = ADC_GAIN_1,
		.reference        = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id       = ADC_TEST_CHANNEL,
		.differential     = 0,
	};

	int rc_adc_setup = adc_channel_setup(adc, &adc_cfg);
	printk("adc_channel_setup(ch=%u) rc = %d\n", ADC_TEST_CHANNEL, rc_adc_setup);

	/* The adc_alif driver stores a 32-bit result per channel and dereferences
	 * sequence.options->user_data (comparator-status stash), so options MUST be
	 * non-NULL and the buffer MUST be uint32_t -- same quirks honoured in
	 * aen-adc-regcheck (driver quirks, not invented). */
	uint32_t sample     = 0;
	uint8_t  cmp_status = 0;

	const struct adc_sequence_options opts = {
		.interval_us     = 0,
		.callback        = NULL,
		.user_data       = &cmp_status,
		.extra_samplings = 0,
	};

	struct adc_sequence seq = {
		.options      = &opts,
		.channels     = BIT(ADC_TEST_CHANNEL),
		.buffer       = &sample,
		.buffer_size  = sizeof(sample),
		.resolution   = 0, /* driver rejects non-zero; programmed via DT */
		.oversampling = 0,
		.calibrate    = false,
	};

	int rc_adc_read = adc_read(adc, &seq);
	printk("adc_read rc = %d\n", rc_adc_read);

	uint32_t raw = sample & ADC_CODE_MASK;

	/* --- expected ADC raw from the VREF ratio --------------------------- */
	/*
	 * expected_raw = C * Vdac_ref * ADC_FS / (DAC_FS * Vadc_ref)
	 *             = (C * ADC_FS_SPAN / DAC_FS_CODE) * (DAC_REFERENCE_MV / ADC_REFERENCE_MV)
	 * Done in two imuldiv steps (64-bit intermediate) to keep precision and
	 * avoid overflow.  Voltage cancels: only the register-fixed VREF ratio +
	 * code spans enter -- this is the TRM-INDEPENDENT prediction.
	 */
	uint32_t code_in_adc_fs = imuldiv(DAC_TEST_CODE, ADC_FS_SPAN, DAC_FS_CODE);
	uint32_t expected_raw   = imuldiv(code_in_adc_fs, DAC_REFERENCE_MV, ADC_REFERENCE_MV);

	/* REPORTED voltages (ideal; absolute pad mV is a bench/TRM unknown). */
	uint32_t dac_out_mv  = imuldiv(DAC_TEST_CODE, DAC_REFERENCE_MV, DAC_FS_CODE);
	uint32_t adc_meas_mv = imuldiv(raw, ADC_REFERENCE_MV, ADC_FS_SPAN);

	int32_t  err     = (int32_t)raw - (int32_t)expected_raw;
	uint32_t abs_err = (uint32_t)((err < 0) ? -err : err);

	printk("-- loopback --\n");
	printk("DAC drove   : code 0x%03x -> ideal %u mV (REPORTED, pad unscoped)\n",
	       DAC_TEST_CODE,
	       dac_out_mv);
	printk("ADC read    : raw %u -> %u mV (REPORTED at 1.8 V VREF)\n", raw, adc_meas_mv);
	printk("expected raw: %u (VREF-ratio %u/%u scaled; TRM-independent)\n",
	       expected_raw,
	       DAC_REFERENCE_MV,
	       ADC_REFERENCE_MV);
	printk("error       : %d LSB (|err| %u, tol +/-%d LSB)\n", err, abs_err, ADC_TOLERANCE_LSB);

	/*
	 * PASS gate:
	 *   - all four driver calls returned 0 (DAC + ADC programmed, conversion ran),
	 *   - the ADC readback is within ADC_TOLERANCE_LSB of the VREF-ratio-scaled
	 *     DAC setpoint (the loopback tracks the register-grounded VREF ratio).
	 * No absolute-mV assertion (buffer/load/trim shift the reading; the absolute
	 * pad accuracy is a TRM/bench unknown).
	 */
	bool calls_ok =
	    (rc_dac_setup == 0) && (rc_dac_write == 0) && (rc_adc_setup == 0) && (rc_adc_read == 0);
	bool ratio_ok = (abs_err <= (uint32_t)ADC_TOLERANCE_LSB);

	if (calls_ok && ratio_ok) {
		printk("RESULT PASS: DAC0->ADC loopback tracks the VREF ratio "
		       "(raw %u vs expected %u, |err| %u <= %d LSB)\n",
		       raw,
		       expected_raw,
		       abs_err,
		       ADC_TOLERANCE_LSB);
	} else {
		printk("RESULT FAIL: calls_ok=%s ratio_ok=%s "
		       "(dac_setup=%d dac_write=%d adc_setup=%d adc_read=%d "
		       "raw=%u expected=%u |err|=%u)\n",
		       calls_ok ? "OK" : "RC-NONZERO",
		       ratio_ok ? "OK" : "OUT-OF-TOL",
		       rc_dac_setup,
		       rc_dac_write,
		       rc_adc_setup,
		       rc_adc_read,
		       raw,
		       expected_raw,
		       abs_err);
	}

	return 0;
}
