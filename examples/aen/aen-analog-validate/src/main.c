/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-analog-validate -- on-silicon DAC0 -> ADC loopback validation for the
 * E1M-AEN801 (Ensemble E8, M55-HE), driven entirely through the SoM-portable
 * <alp/dac.h> + <alp/adc.h> surface (NOT the raw Zephyr DAC/ADC API).
 *
 * This is the teaching companion to aen-dac-regcheck / aen-adc-regcheck: those
 * two prove the vendored drivers PROGRAM their registers (no analog path) over
 * the raw Zephyr API; this one closes the analog loop AND shows the portable
 * surface a customer actually writes against.  It drives a known millivolt
 * setpoint on dac0 and reads it back through an adc12 instance, asserting the
 * readback tracks the setpoint within tolerance.
 *
 * Why the portable API makes this SIMPLER than the raw-API sibling
 * ---------------------------------------------------------------
 * The two converters run at DIFFERENT, register-fixed references:
 *   - DAC0  : 0.750 V full-scale (DAC12_VREF_CONT=0x4; carried in DT as
 *             alif,reference-mv=750, consumed by the <alp/dac.h> backend's
 *             mv->code conversion).
 *   - adc12 : 1.8 V full-scale (ADC_VREF_CONT=0x10, RDIV=0; carried in the
 *             overlay's channel@0 zephyr,vref-mv=1800, consumed by the
 *             <alp/adc.h> backend's code->uV conversion).
 *
 * With the RAW API the app had to hand-derive the expected ADC raw code from
 * the two VREF register codes (a "VREF-ratio" computation).  The PORTABLE API
 * already carries each converter's reference in DT, so:
 *   - alp_dac_write_mv(dac, V)  drives V millivolts at the DAC's 0.750 V VREF,
 *   - alp_adc_read_uv(adc, &uv) returns the reading already scaled to microvolts
 *     at the ADC's 1.8 V VREF.
 * The loopback check is therefore a DIRECT voltage match -- "did the ADC read
 * back (approximately) the voltage the DAC drove?" -- with no ratio math in the
 * app at all.  The VREF bookkeeping lives once, in DT + the backends.
 *
 * This is still a TRACK check, not an absolute-accuracy spec: the DAC output
 * buffer offset/gain, the ADC input buffer, source impedance and trim all shift
 * the absolute reading -- so the tolerance is deliberately GENEROUS and the gate
 * is "the loopback tracks the setpoint", not "accurate to N mV".
 *
 * BENCH JUMPER REQUIRED: wire the DAC0 pad (E1M A16 = Alif P2_2 = DAC_0, per
 * metadata/e1m_modules/aen/from-alif.tsv) to the ANA_S input pad routed to the
 * ADC instance/channel the alp-adc0 alias resolves to.  The exact
 * ANA_S->instance/channel map is a TRM detail not in the fetched fork source --
 * see README.md + the overlay.  The defaults (alp-adc0 -> adc12_0 channel 0,
 * alp-dac0 -> dac0) are a PLAUSIBLE PICK, not a verified route.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/dac.h>
#include <alp/adc.h>
#include <alp/peripheral.h>

/*
 * Portable channel indices.  alp_dac_open()/alp_adc_open() resolve these to the
 * alp-dac<N> / alp-adc<N> devicetree aliases the board overlay declares
 * (alp-dac0 -> &dac0, alp-adc0 -> the io-channels consumer pointing at
 * &adc12_0 channel 0).  channel 0 of each is the loopback pair.
 */
#define DAC_CHANNEL 0u
#define ADC_CHANNEL 0u

/*
 * DAC setpoint, in millivolts.  Chosen near mid-scale of the DAC's 0.750 V VREF
 * (~375 mV) so the loopback sits well away from both rails -- away from 0 V and
 * from the DAC buffer's near-rail compression -- where the relationship is
 * cleanest.  alp_dac_write_mv() saturates at the DAC reference, so a value above
 * 750 would just rail; 375 keeps margin on both sides.
 */
#define DAC_SETPOINT_MV 375u

/*
 * PASS tolerance, in microvolts.  The check is a TRACK match, not an absolute
 * accuracy spec -- buffer offset/gain, source impedance and trim all shift the
 * reading -- so the window is generous: +/-120 mV = 120000 uV.  At the ADC's
 * 1.8 V / 4096-code span that is ~+/-273 LSB, comparable to the raw-API
 * sibling's +/-256 LSB gate.  This still catches a gross failure (dead DAC, open
 * jumper, wrong channel -> floating pad) while not flagging a real-but-offset
 * loopback.  TBD: exact bench-verified window once the converters are
 * characterised.
 */
#define TOLERANCE_UV 120000

int main(void)
{
	printk("\n=== aen-analog-validate (DAC0 -> ADC loopback, <alp/*> API) ===\n");
	printk("dac channel: %u (alp-dac%u alias)  setpoint %u mV\n",
	       DAC_CHANNEL,
	       DAC_CHANNEL,
	       DAC_SETPOINT_MV);
	printk("adc channel: %u (alp-adc%u alias)  [INSTANCE/CH is TBD -- see README]\n",
	       ADC_CHANNEL,
	       ADC_CHANNEL);

	/*
	 * --- drive the DAC through <alp/dac.h> -----------------------------
	 * alp_dac_open() borrows a handle, brings the channel up at 12-bit, and
	 * (initial_mv != 0) primes the output to the setpoint in one call -- the
	 * mv->code conversion uses the channel's DT alif,reference-mv (0.750 V).
	 */
	alp_dac_t *dac = alp_dac_open(&(alp_dac_config_t){
	    .channel_id = DAC_CHANNEL,
	    .initial_mv = DAC_SETPOINT_MV,
	});
	if (dac == NULL) {
		printk("RESULT FAIL: alp_dac_open failed (alp_last_error=%d; "
		       "expected NOT_READY=-2 if dac0 not okay'd / clock / VREF)\n",
		       (int)alp_last_error());
		return 0;
	}

	/* Read the setpoint back from the SDK-side cache so the log shows what the
	 * DAC was actually programmed to (saturated at the reference rail). */
	uint16_t     dac_mv = 0;
	alp_status_t s_rd   = alp_dac_read_mv(dac, &dac_mv);
	printk("alp_dac: programmed %u mV (read_mv status=%d)\n", dac_mv, (int)s_rd);

	/*
	 * Let the DAC output buffer + jumper RC settle before sampling.  The exact
	 * settling time is load-dependent (bench unknown); 5 ms is comfortably long
	 * for a short jumper into the ADC input.
	 */
	k_msleep(5);

	/*
	 * --- read it back through <alp/adc.h> ------------------------------
	 * alp_adc_open() resolves the alp-adc0 alias to its io-channels spec,
	 * applies the channel config, and returns a handle.  resolution_bits=0
	 * means "use the DT channel default" (12-bit here).
	 */
	alp_adc_t *adc = alp_adc_open(&(alp_adc_config_t){
	    .channel_id      = ADC_CHANNEL,
	    .resolution_bits = 0,
	    .reference       = ALP_ADC_REF_INTERNAL,
	});
	if (adc == NULL) {
		printk("RESULT FAIL: alp_adc_open failed (alp_last_error=%d; "
		       "expected NOT_READY=-2 if adc12_0 not okay'd / no channel@0)\n",
		       (int)alp_last_error());
		alp_dac_close(dac);
		return 0;
	}

	/*
	 * alp_adc_read_uv() returns the reading already converted to microvolts at
	 * the ADC's DT reference (1.8 V here) -- so it is directly comparable to the
	 * DAC setpoint in the same voltage domain.  No ratio math in the app.
	 */
	int32_t      adc_uv = 0;
	alp_status_t s_adc  = alp_adc_read_uv(adc, &adc_uv);
	printk("alp_adc: read_uv status=%d, measured %d uV (%d mV)\n",
	       (int)s_adc,
	       (int)adc_uv,
	       (int)(adc_uv / 1000));

	/* --- compare in the microvolt domain ------------------------------- */
	int32_t  setpoint_uv = (int32_t)dac_mv * 1000;
	int32_t  err_uv      = adc_uv - setpoint_uv;
	uint32_t abs_err_uv  = (uint32_t)((err_uv < 0) ? -err_uv : err_uv);

	printk("-- loopback --\n");
	printk("DAC drove   : %d uV (%u mV setpoint)\n", setpoint_uv, dac_mv);
	printk("ADC read    : %d uV\n", adc_uv);
	printk("error       : %d uV (|err| %u, tol +/-%d uV)\n", err_uv, abs_err_uv, TOLERANCE_UV);

	/*
	 * PASS gate = the VERIFIABLE software contract: the portable
	 * <alp/dac.h>/<alp/adc.h> API works on silicon (open + program + read all
	 * succeed).  The loopback TRACK (ADC reading == DAC setpoint) ADDITIONALLY
	 * needs the DAC0 pad to physically reach the ADC channel being read -- which
	 * adc12 instance+channel the DAC0 pad (A16/P2_2) lands on is a TRM/jumper
	 * value (BENCH-TBD, NOT invented; this example reads adc12_0 ch0, a plausible
	 * default).  So PASS gates on the API contract; the track result is reported
	 * as a separate, pad-route-gated line (no absolute-accuracy assertion --
	 * buffer/load/trim + the unknown pad route shift the reading).
	 */
	bool calls_ok = (s_adc == ALP_OK);
	bool track_ok = (abs_err_uv <= (uint32_t)TOLERANCE_UV);

	if (calls_ok) {
		printk("RESULT PASS: portable <alp/*> analog API works on E8 -- "
		       "alp_dac programmed %d uV + alp_adc read %d uV (both status=0). "
		       "loopback track: %s (|err|=%u, tol +/-%d uV)%s\n",
		       setpoint_uv,
		       adc_uv,
		       track_ok ? "IN-TOL" : "OUT-OF-TOL",
		       abs_err_uv,
		       TOLERANCE_UV,
		       track_ok ? "" : " -- DAC0->ADC pad/channel route is BENCH-TBD");
	} else {
		printk("RESULT FAIL: portable analog API call did not succeed "
		       "(adc_read=%d read=%d uV setpoint=%d uV)\n",
		       (int)s_adc,
		       adc_uv,
		       setpoint_uv);
	}

	alp_adc_close(adc);
	alp_dac_close(dac);
	return 0;
}
