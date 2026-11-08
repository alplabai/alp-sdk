/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * zephyr_drv ADC sample-width handling against native_sim's real
 * zephyr,adc-emul controller (adc0).  alp-adc1 resolves to a 16-bit
 * single-ended channel (boards/native_sim*.overlay, channel@1).
 *
 * Regression for #2924: the backend used to sign-extend every sample of
 * 16 bits or less, so a single-ended 16-bit conversion above 0x7FFF
 * (e.g. 0xC000 = 49152) read back negative (-16384).  Single-ended
 * samples are unsigned and must be zero-extended.
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/adc_emul.h>

#include <alp/adc.h>
#include <alp/peripheral.h>

#if DT_NODE_EXISTS(DT_ALIAS(alp_adc1))

ZTEST_SUITE(alp_adc_emul_sign, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_adc_emul_sign, test_16bit_single_ended_high_sample_stays_positive)
{
	const struct adc_dt_spec spec = ADC_DT_SPEC_GET(DT_ALIAS(alp_adc1));

	zassert_true(device_is_ready(spec.dev), "adc-emul controller not ready");
	zassert_false(spec.channel_cfg.differential, "channel@1 must be single-ended");

	/* adc-emul takes millivolts against ref-internal-mv = 3300:
	 * 2475 mV is 75 % of full scale, i.e. raw ~= 0xC000 (49152). */
	zassert_ok(adc_emul_const_value_set(spec.dev, spec.channel_id, 2475U));

	alp_adc_config_t cfg = ALP_ADC_CONFIG_DEFAULT(1);
	alp_adc_t       *h   = alp_adc_open(&cfg);
	zassert_not_null(h, "alp_adc_open(1) failed: %d", (int)alp_last_error());

	int32_t raw = 0;
	zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK);
	/* Old int16_t path: ~49151 -> ~-16385.  Fixed path: stays ~49151. */
	zassert_true(raw > 0x7FFF && raw <= 0xFFFF, "raw %d was sign-extended", (int)raw);
	zassert_within(raw, 0xC000, 4, "raw %d, expected ~49152", (int)raw);

	alp_adc_close(h);
}

#endif /* DT_NODE_EXISTS(DT_ALIAS(alp_adc1)) */
