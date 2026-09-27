/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native_sim unit test for zephyr/drivers/video/isp_sns_gain_conv.h (#2287 Stage B unit 3) --
 * the ISP-Pico AE gain/exposure sensor-register conversion helpers. The header is deliberately
 * free of any Zephyr/hal_alif dependency (only <stdint.h>), so this test includes it directly
 * with no emulator, no ISP driver, no device tree -- pure logic, the smallest thing that fails
 * if the conversion math breaks.
 *
 * Coverage: the two register scales this driver supports --
 *   - IMX296's logarithmic dB-tenths GAIN register (0 dB, 6 dB, 48 dB, and the 0/480 clamps)
 *   - OV5647's linear AGC_GAIN register (the x16 path, both directions)
 * and the line-based exposure-control scaling both sensors share the same helper for.
 */
#include <zephyr/ztest.h>

#include "isp_sns_gain_conv.h"

ZTEST_SUITE(isp_ae_conv, NULL, NULL, NULL, NULL, NULL);

/* 0 dB (0 register counts) is unity gain -- table[0] must read back exactly ISP_SNS_GAIN_LIB_UNITY
 * (1024), and the reverse conversion of exactly 1024 must land back on register 0.
 */
ZTEST(isp_ae_conv, test_db_tenths_0db_is_unity)
{
	zassert_equal(isp_sns_gain_db_tenths_to_lib(0), ISP_SNS_GAIN_LIB_UNITY, NULL);
	zassert_equal(isp_sns_gain_lib_to_db_tenths(ISP_SNS_GAIN_LIB_UNITY), 0, NULL);
}

/* 6 dB (60 tenths) approximately doubles linear gain: round(1024 * 10^(6/20)) = 2043 -- not
 * exactly 2048 (a true x2) because 6 dB is only an approximation of a linear doubling, the
 * well-known "6 dB ~= x2" audio/RF rule of thumb (the EXACT doubling point is 20*log10(2) =
 * 6.0206 dB, i.e. 60.206 tenths, not the whole number 60 this test checks).
 */
ZTEST(isp_ae_conv, test_db_tenths_6db_doubles)
{
	uint32_t lib = isp_sns_gain_db_tenths_to_lib(60);

	zassert_within(lib, 2 * ISP_SNS_GAIN_LIB_UNITY, 40, "6 dB should be close to a 2x gain");
	/* Round-trip: converting that same library value back should land on register 60 again. */
	zassert_equal(isp_sns_gain_lib_to_db_tenths(lib), 60, NULL);
}

/* 48 dB (480 tenths, the IMX296 GAIN register's documented ceiling) is the top table entry. */
ZTEST(isp_ae_conv, test_db_tenths_48db_ceiling)
{
	uint32_t lib = isp_sns_gain_db_tenths_to_lib(480);

	zassert_equal(lib, 257217, "48 dB ceiling should match isp_gain_db_tenths_table[480]");
	zassert_equal(isp_sns_gain_lib_to_db_tenths(lib), 480, NULL);
}

/* Out-of-range register/gain inputs clamp to the table's own [0, 480] / [1024, 257217] ends
 * instead of indexing out of bounds or wrapping.
 */
ZTEST(isp_ae_conv, test_db_tenths_clamps)
{
	zassert_equal(isp_sns_gain_db_tenths_to_lib(9999), 257217, "reg > 480 clamps to table[480]");
	zassert_equal(isp_sns_gain_lib_to_db_tenths(0), 0, "gain below table[0] clamps to reg 0");
	zassert_equal(
	    isp_sns_gain_lib_to_db_tenths(UINT32_MAX), 480, "gain above table[480] clamps to reg 480");
}

/* OV5647's linear AGC_GAIN path (register value directly proportional to gain, reg_per_1x = 16):
 * register 16 = 1.0x = library units 1024, and the writeback scale isp_api_wrapper.c used to
 * hardcode ("totalGain * 16 / ISP_SNS_GAIN_ACCU") must still be exactly reproduced.
 */
ZTEST(isp_ae_conv, test_linear_reg_ov5647_scale)
{
	zassert_equal(isp_sns_gain_linear_reg_to_lib(16, 16), ISP_SNS_GAIN_LIB_UNITY, NULL);
	zassert_equal(isp_sns_gain_lib_to_linear_reg(ISP_SNS_GAIN_LIB_UNITY, 16), 16, NULL);
	/* A concrete non-unity case matching the old hardcoded formula bit-for-bit. */
	zassert_equal(isp_sns_gain_lib_to_linear_reg(65472, 16), 65472 * 16 / 1024, NULL);
}

/* Exposure-control scaling: OV5647's 1/16-line VIDEO_CID_EXPOSURE (ctrl_per_line = 16, the old
 * hardcoded "intLine * 16") and IMX296's whole-line VIDEO_CID_EXPOSURE (ctrl_per_line = 1) -- NOT
 * IMX296's SHS hardware register, which is inversely related (SHS = lines_per_frame - lines,
 * imx296_set_ctrl()'s own job, not this helper's -- see isp_sns_gain_conv.h's header comment).
 */
ZTEST(isp_ae_conv, test_exposure_lines_to_ctrl)
{
	zassert_equal(isp_sns_exposure_lines_to_ctrl(100, 16), 1600, "OV5647: 100 lines * 16");
	zassert_equal(isp_sns_exposure_lines_to_ctrl(100, 1), 100, "IMX296: 100 lines * 1");
}

/* Whole-table sweep: isp_gain_db_tenths_table[] must be strictly monotonically increasing (the
 * binary search in isp_sns_gain_lib_to_db_tenths() assumes this) and every entry must round-trip
 * through both conversion directions back to its own index -- a table[reg] -> lib -> reg round
 * trip can only fail this way if two adjacent entries are close enough for the "closest by linear
 * distance" search to prefer the wrong neighbour, which would also mean the table drifted from its
 * own generator formula (isp_sns_gain_conv.h's comment).
 */
ZTEST(isp_ae_conv, test_db_tenths_table_monotonic_and_round_trips)
{
	for (uint32_t reg = 0; reg <= 480; reg++) {
		uint32_t lib = isp_sns_gain_db_tenths_to_lib(reg);

		if (reg > 0) {
			zassert_true(lib > isp_sns_gain_db_tenths_to_lib(reg - 1),
			             "table[%u] must be strictly greater than table[%u]",
			             reg,
			             reg - 1);
		}
		zassert_equal(isp_sns_gain_lib_to_db_tenths(lib),
		              reg,
		              "table[%u] -> lib -> reg round-trip landed on a different reg",
		              reg);
	}
}

/*
 * #2327 Stage B: IMX335's VIDEO_CID_ANALOGUE_GAIN control is dB-log like IMX296's, but the
 * upstream driver pre-scales it to MILLI-dB (ctrl_per_db_tenth = 100 -- 100 mdB = 1 tenth of a
 * dB) instead of exposing the raw 0.1 dB/count register value directly the way IMX296 does.
 * ctrl_per_db_tenth = 1 must reduce these two functions to the plain db_tenths ones exactly (the
 * identity case IMX296 already relies on); ctrl_per_db_tenth = 100 is IMX335's own case.
 */
ZTEST(isp_ae_conv, test_db_tenths_ctrl_identity_matches_imx296)
{
	for (uint32_t reg = 0; reg <= 480; reg += 30) {
		zassert_equal(isp_sns_gain_db_tenths_ctrl_to_lib(reg, 1),
		              isp_sns_gain_db_tenths_to_lib(reg),
		              "ctrl_per_db_tenth=1 must match the plain db_tenths conversion");
	}
	uint32_t lib = isp_sns_gain_db_tenths_to_lib(240);

	zassert_equal(isp_sns_gain_lib_to_db_tenths_ctrl(lib, 1, 1),
	              isp_sns_gain_lib_to_db_tenths(lib),
	              "ctrl_per_db_tenth=1, ctrl_step=1 must match the plain db_tenths reverse "
	              "conversion (both no-ops)");
}

/* IMX335's 30.0 dB analog-gain cap (imx335_ae_envelope.h's IMX335_AE_MAX_AGAIN = 32382, hal_alif
 * patch 0014) is register code 100 in the upstream driver's own 0.3 dB/count GAIN register, i.e.
 * control value 30000 mdB (IMX335_GAIN_UNIT_MDB = 300, so 100 * 300 = 30000) -- ctrl_per_db_tenth
 * = 100 (100 mdB = 1 tenth of a dB) must convert that control value to isp_gain_db_tenths_table[300]
 * exactly (300 tenths = 30.0 dB), matching the literal hal_alif patch 0014 hand-duplicates.
 */
ZTEST(isp_ae_conv, test_db_tenths_ctrl_imx335_30db_cap)
{
	zassert_equal(isp_sns_gain_db_tenths_ctrl_to_lib(30000, 100),
	              32382,
	              "IMX335 30.0 dB cap (30000 mdB) must land on isp_gain_db_tenths_table[300]");
	zassert_equal(isp_sns_gain_lib_to_db_tenths_ctrl(32382, 100, 300),
	              30000,
	              "reverse: library units for 30.0 dB must land on 30000 mdB, already an exact "
	              "multiple of the IMX335_GAIN_UNIT_MDB=300 register step");
}

/*
 * #2327 Stage B reviewer follow-up: isp_sns_gain_lib_to_db_tenths_ctrl()'s ctrl_step rounding
 * must land on an EXACT multiple of 300 (IMX335_GAIN_UNIT_MDB) for EVERY table entry, not just
 * the ones that already happen to be -- a naive tenths*ctrl_per_db_tenth scale-up (no rounding)
 * would produce a non-multiple-of-300 value whenever tenths is not itself a multiple of 3, which
 * imx335_set_ctrl()'s own truncating `ctrl.val / 300` divide would then silently under-drive.
 */
ZTEST(isp_ae_conv, test_db_tenths_ctrl_imx335_step_always_multiple_of_300)
{
	for (uint32_t reg = 0; reg <= 480; reg++) {
		uint32_t lib = isp_sns_gain_db_tenths_to_lib(reg);
		uint32_t ctrl = isp_sns_gain_lib_to_db_tenths_ctrl(lib, 100, 300);

		zassert_true((ctrl % 300U) == 0U,
		             "table[%u] -> ctrl=%u is not an exact multiple of the 300 mdB "
		             "register step",
		             reg,
		             ctrl);
	}
}

/* 0 dB (0 mdB) is still unity at any ctrl_per_db_tenth scale. */
ZTEST(isp_ae_conv, test_db_tenths_ctrl_0db_is_unity)
{
	zassert_equal(isp_sns_gain_db_tenths_ctrl_to_lib(0, 100), ISP_SNS_GAIN_LIB_UNITY, NULL);
	zassert_equal(isp_sns_gain_lib_to_db_tenths_ctrl(ISP_SNS_GAIN_LIB_UNITY, 100, 300), 0, NULL);
}
