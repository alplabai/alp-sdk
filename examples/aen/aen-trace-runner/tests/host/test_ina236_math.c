/* tests/host/test_ina236_math.c -- pure INA236 shunt/bus -> power conversion
 * (src/platform/ina236_math.h). No I2C, no board: every value here is
 * derived from the SBOSA81D equations by hand, independent of the code
 * under test, so a regression in the LSB constants or the V=IR/P=VI wiring
 * cannot cancel itself out.
 */
#include <assert.h>
#include <stdio.h>

#include "../../src/platform/ina236_math.h"

int main(void)
{
	/* 1. Bus LSB is 1.6 mV: 5.000 V -> raw 3125 (5000 / 1.6 = 3125.0 exactly). */
	assert(tr_ina236_bus_mv(3125) == 5000);
	assert(tr_ina236_bus_mv(0) == 0);
	assert(tr_ina236_bus_mv(-3125) == -5000);

	/* 2. Shunt LSB: 2.5 uV/count coarse (ADCRANGE=0), 0.625 uV/count fine
	 * (ADCRANGE=1) -- table 7-4. 800 counts coarse = 2000 uV; the SAME
	 * physical 2000 uV needs 4x the counts (3200) on the fine range --
	 * this is exactly the ratio the shipped driver's missing /4 on
	 * SHUNT_CAL got backwards (see ina236_math.h's header comment). */
	assert(tr_ina236_shunt_uv(800, false) == 2000);
	assert(tr_ina236_shunt_uv(3200, true) == 2000);
	assert(tr_ina236_shunt_uv(0, false) == 0);

	/* 3. Current = V/R: 2000 uV / 20 mOhm = 100000 uA (100 mA). */
	assert(tr_ina236_current_ua(2000, 0.020f) == 100000);
	assert(tr_ina236_current_ua(0, 0.020f) == 0);

	/* 4. Power = V*I: 5000 mV * 100000 uA = 0.5 W = 500 mW. This is the
	 * exact scale the shipped driver's power_uw got 625x wrong (an extra
	 * 1.6 mV bus-LSB factor on top of eq. 4's 32) -- a broken version of
	 * this function computing bus_mv * current_ua / 1000000 / 625 would
	 * report 0 mW here (integer truncation), failing this assert loudly
	 * rather than passing on a rounding coincidence. */
	assert(tr_ina236_power_mw(5000, 100000) == 500);
	assert(tr_ina236_power_mw(0, 100000) == 0);
	assert(tr_ina236_power_mw(5000, 0) == 0);
	assert(tr_ina236_power_mw(5000, -1) == 0); /* never negative */

	/* 5. End to end, the +5V net's real 20 mOhm/coarse-range configuration
	 * (rail5v_power.c): 102 mA at 5.10 V (the aen-inference-energy doc's
	 * measured load) -> 800 counts shunt (see step 2), 3188 counts bus
	 * (5100 / 1.6 = 3187.5, truncates to 3187 -> 5099 mV, 1 mV off by
	 * integer truncation, which is the real hardware's own quantisation,
	 * not a bug in this math). */
	{
		int32_t bus_mv     = tr_ina236_bus_mv(3188);
		int32_t shunt_uv   = tr_ina236_shunt_uv(816 /* 2.04 mV / 2.5 uV */, false);
		int32_t current_ua = tr_ina236_current_ua(shunt_uv, 0.020f);
		int32_t power_mw   = tr_ina236_power_mw(bus_mv, current_ua);

		assert(bus_mv == 5100);
		assert(shunt_uv == 2040);
		assert(current_ua == 102000);
		assert(power_mw == 520); /* 5.100 V * 0.102 A */
	}

	/* CONFIG, decoded field by field against TI SBOSA81D table 7-4 (tables written out here from
	 * the datasheet, not read from the code under test): RST[15] 0, reserved[14:13] 10b,
	 * ADCRANGE[12] 0, AVG[11:9], VBUSCT[8:6], VSHCT[5:3], MODE[2:0]. */
	{
		static const uint16_t ct_us[8]  = { 140, 204, 332, 588, 1100, 2116, 4156, 8244 };
		static const uint16_t avg_n[8]  = { 1, 4, 16, 64, 128, 256, 512, 1024 };
		unsigned              avg       = (TR_INA236_CONFIG >> 9) & 7u;
		unsigned              vbusct    = (TR_INA236_CONFIG >> 6) & 7u;
		unsigned              vshct     = (TR_INA236_CONFIG >> 3) & 7u;
		uint32_t              window_us = (uint32_t)avg_n[avg] * (ct_us[vbusct] + ct_us[vshct]);

		assert((TR_INA236_CONFIG >> 15) == 0u && ((TR_INA236_CONFIG >> 13) & 3u) == 2u);
		assert(((TR_INA236_CONFIG >> 12) & 1u) == 0u); /* ADCRANGE 0: +-81.92 mV, 2.5 uV/LSB */
		assert((TR_INA236_CONFIG & 7u) == 7u);         /* continuous shunt + bus */
		assert(avg_n[avg] == 128 && ct_us[vbusct] == 204 && ct_us[vshct] == 588);
		assert(window_us == 101376u); /* one result: ~100 ms, the graph's 10 Hz */
		assert(TR_INA236_CONFIG == 0x485Fu);
	}
	assert(tr_ina236_config_ok(TR_INA236_CONFIG));
	assert(tr_ina236_config_ok(TR_INA236_CONFIG | 0x8000u)); /* RST/reserved bits are not ours */
	assert(!tr_ina236_config_ok(0x4127u));                   /* power-on reset: AVG 1 */
	assert(!tr_ina236_config_ok(0x4927u)); /* the previous CONFIG: AVG 128 of 1.1 ms, 282 ms */
	assert(
	    !tr_ina236_config_ok(TR_INA236_CONFIG | 1u << 12)); /* ADCRANGE 1: a 4x shunt LSB error */
	assert(!tr_ina236_config_ok(0xFFFFu));                  /* a bus reading all ones */

	printf("PASS: test_ina236_math\n");
	return 0;
}
