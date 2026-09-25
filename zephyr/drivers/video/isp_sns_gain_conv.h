/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ISP-Pico AE sensor-register unit conversion (#2287 Stage B unit 3).
 *
 * isp_pico.c's isp_apply_ae() and hal_alif's isp_api_wrapper.c writeback
 * (AE gain/exposure push/readback) both need to convert between this
 * library's own fixed-point gain scale (ISP_SNS_GAIN_ACCU = 1024 = 1.0x,
 * lines for exposure) and a SENSOR's own control-register units -- and
 * those register units differ by sensor:
 *
 *   - OV5647's AGC_GAIN (0x350a) is LINEAR: register 0x10 (16) = 1.0x, so
 *     "register units per 1x" is a constant (CONFIG_VIDEO_ISP_VSI_SNS_GAIN_
 *     REG_PER_1X, default 16) and the conversion is a plain multiply/divide.
 *     Its VIDEO_CID_EXPOSURE control is in 1/16-line units (CONFIG_VIDEO_
 *     ISP_VSI_SNS_EXPOSURE_CTRL_PER_LINE = 16).
 *
 *   - IMX296's GAIN (0x3204-0x3205, datasheet p.56) is LOGARITHMIC: 0.1 dB
 *     per register count, 0 = 0 dB (1.0x) up to 480 = 48.0 dB. Converting
 *     that to/from this library's linear 1024=1x scale needs gain_linear =
 *     10^(dB/20) -- gain_linear(reg) = 1024 * 10^(reg/200) (reg is in
 *     TENTHS of a dB, hence /200 not /20). Its VIDEO_CID_EXPOSURE control
 *     (zephyr/drivers/video/imx296.c's imx296_set_ctrl()) is already in
 *     whole LINES (CONFIG_VIDEO_ISP_VSI_SNS_EXPOSURE_CTRL_PER_LINE = 1) --
 *     NOT the sensor's own SHS register (0x308D-0x308F) directly: SHS is
 *     inversely related (SHS = lines_per_frame - lines, imx296.c's own
 *     IMX296_REG_SHS comment, datasheet p.60); imx296_set_ctrl() does that
 *     conversion itself once it receives the ctrl's line count, so nothing
 *     in THIS header or its callers ever computes or writes an SHS value.
 *
 * No floating point and no libm dependency here (this runs from an ISP
 * bottom-half IRQ context, isp_vsi_bottom_half()): isp_gain_db_tenths_table
 * below is a 481-entry (0..480 inclusive) precomputed LOOKUP table for
 * gain_linear(reg) = round(1024 * 10^(reg/200)) -- both directions use this
 * SAME table: the forward direction (register -> library units) is a
 * direct index, the reverse (library units -> register) is a binary search
 * for the entry closest by linear distance, which lands within 1 count of
 * round(200*log10(total/1024)) (the two can differ by at most 1 near a
 * table-entry boundary, since "closest by value" and "closest by log" are
 * not exactly the same rounding rule) without ever computing a log10.
 *
 * This header is intentionally free of any Zephyr or hal_alif include (only
 * <stdint.h>) so it can be included standalone by a native_sim unit test
 * (tests/zephyr/isp_ae_conv) as well as by isp_pico.c.
 */
#ifndef ZEPHYR_DRIVERS_VIDEO_ISP_SNS_GAIN_CONV_H_
#define ZEPHYR_DRIVERS_VIDEO_ISP_SNS_GAIN_CONV_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISP_SNS_GAIN_LIB_UNITY 1024U /* library's own ISP_SNS_GAIN_ACCU: 1024 = 1.0x */

/*
 * Generator formula (each table[n] below): gain_linear(n) = round(1024 * 10^(n/200)) for
 * n = 0..480 (0.0..48.0 dB in 0.1 dB steps) -- IMX296's GAIN register range (datasheet p.56).
 * table[0] = 1024 (0 dB = 1.0x); table[480] = 257217 (48.0 dB), matching the #2287 unit-3 design
 * figure of ~257216 within the rounding of 10^2.4. Generated with a one-off Python loop (not
 * committed as a script -- 481 entries, regenerate the same way if the formula or range changes).
 */
static const uint32_t isp_gain_db_tenths_table[481] = {
	1024, 1036, 1048, 1060, 1072, 1085, 1097, 1110,
	1123, 1136, 1149, 1162, 1176, 1189, 1203, 1217,
	1231, 1245, 1260, 1274, 1289, 1304, 1319, 1334,
	1350, 1366, 1381, 1397, 1414, 1430, 1446, 1463,
	1480, 1497, 1515, 1532, 1550, 1568, 1586, 1604,
	1623, 1642, 1661, 1680, 1699, 1719, 1739, 1759,
	1780, 1800, 1821, 1842, 1863, 1885, 1907, 1929,
	1951, 1974, 1997, 2020, 2043, 2067, 2091, 2115,
	2139, 2164, 2189, 2215, 2240, 2266, 2292, 2319,
	2346, 2373, 2400, 2428, 2456, 2485, 2514, 2543,
	2572, 2602, 2632, 2663, 2693, 2725, 2756, 2788,
	2820, 2853, 2886, 2919, 2953, 2987, 3022, 3057,
	3092, 3128, 3164, 3201, 3238, 3276, 3314, 3352,
	3391, 3430, 3470, 3510, 3551, 3592, 3633, 3675,
	3718, 3761, 3805, 3849, 3893, 3938, 3984, 4030,
	4077, 4124, 4172, 4220, 4269, 4318, 4368, 4419,
	4470, 4522, 4574, 4627, 4681, 4735, 4790, 4845,
	4901, 4958, 5015, 5073, 5132, 5192, 5252, 5313,
	5374, 5436, 5499, 5563, 5627, 5692, 5758, 5825,
	5893, 5961, 6030, 6100, 6170, 6242, 6314, 6387,
	6461, 6536, 6611, 6688, 6766, 6844, 6923, 7003,
	7084, 7166, 7249, 7333, 7418, 7504, 7591, 7679,
	7768, 7858, 7949, 8041, 8134, 8228, 8323, 8420,
	8517, 8616, 8716, 8817, 8919, 9022, 9126, 9232,
	9339, 9447, 9557, 9667, 9779, 9892, 10007, 10123,
	10240, 10359, 10479, 10600, 10723, 10847, 10972, 11099,
	11228, 11358, 11489, 11623, 11757, 11893, 12031, 12170,
	12311, 12454, 12598, 12744, 12891, 13041, 13192, 13344,
	13499, 13655, 13813, 13973, 14135, 14299, 14464, 14632,
	14801, 14973, 15146, 15321, 15499, 15678, 15860, 16044,
	16229, 16417, 16607, 16800, 16994, 17191, 17390, 17591,
	17795, 18001, 18210, 18420, 18634, 18850, 19068, 19289,
	19512, 19738, 19966, 20198, 20431, 20668, 20907, 21149,
	21394, 21642, 21893, 22146, 22403, 22662, 22925, 23190,
	23458, 23730, 24005, 24283, 24564, 24848, 25136, 25427,
	25722, 26020, 26321, 26626, 26934, 27246, 27561, 27880,
	28203, 28530, 28860, 29194, 29532, 29874, 30220, 30570,
	30924, 31282, 31645, 32011, 32382, 32757, 33136, 33520,
	33908, 34300, 34698, 35099, 35506, 35917, 36333, 36754,
	37179, 37610, 38045, 38486, 38931, 39382, 39838, 40300,
	40766, 41238, 41716, 42199, 42687, 43182, 43682, 44188,
	44699, 45217, 45740, 46270, 46806, 47348, 47896, 48451,
	49012, 49579, 50153, 50734, 51322, 51916, 52517, 53125,
	53740, 54363, 54992, 55629, 56273, 56925, 57584, 58251,
	58925, 59607, 60298, 60996, 61702, 62417, 63139, 63870,
	64610, 65358, 66115, 66881, 67655, 68438, 69231, 70033,
	70843, 71664, 72494, 73333, 74182, 75041, 75910, 76789,
	77678, 78578, 79488, 80408, 81339, 82281, 83234, 84198,
	85173, 86159, 87157, 88166, 89187, 90219, 91264, 92321,
	93390, 94471, 95565, 96672, 97791, 98924, 100069, 101228,
	102400, 103586, 104785, 105999, 107226, 108468, 109724, 110994,
	112279, 113580, 114895, 116225, 117571, 118932, 120310, 121703,
	123112, 124537, 125980, 127438, 128914, 130407, 131917, 133444,
	134989, 136553, 138134, 139733, 141351, 142988, 144644, 146319,
	148013, 149727, 151461, 153215, 154989, 156783, 158599, 160435,
	162293, 164172, 166073, 167996, 169942, 171910, 173900, 175914,
	177951, 180011, 182096, 184204, 186337, 188495, 190678, 192886,
	195119, 197379, 199664, 201976, 204315, 206681, 209074, 211495,
	213944, 216421, 218927, 221462, 224027, 226621, 229245, 231900,
	234585, 237301, 240049, 242829, 245640, 248485, 251362, 254273,
	257217,
};

/* dB-tenths register (0..480) -> library gain units (1024 = 1x). Forward lookup. */
static inline uint32_t isp_sns_gain_db_tenths_to_lib(uint32_t reg)
{
	if (reg > 480U) {
		reg = 480U;
	}
	return isp_gain_db_tenths_table[reg];
}

/*
 * Library gain units (1024 = 1x) -> nearest dB-tenths register (0..480).
 * Binary search for the closest table entry -- equivalent to
 * round(200*log10(total/1024)) clamped to [0, 480], without a log10 call.
 */
static inline uint32_t isp_sns_gain_lib_to_db_tenths(uint32_t total_1024)
{
	uint32_t lo = 0U, hi = 480U;

	if (total_1024 <= isp_gain_db_tenths_table[0]) {
		return 0U;
	}
	if (total_1024 >= isp_gain_db_tenths_table[480]) {
		return 480U;
	}
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2U;

		if (isp_gain_db_tenths_table[mid] < total_1024) {
			lo = mid + 1U;
		} else {
			hi = mid;
		}
	}
	/* lo is the first index with table[lo] >= total_1024; pick whichever of
	 * lo-1/lo is numerically closer (a tie rounds up, matching round()).
	 */
	if (lo > 0U) {
		uint32_t hi_diff = isp_gain_db_tenths_table[lo] - total_1024;
		uint32_t lo_diff = total_1024 - isp_gain_db_tenths_table[lo - 1U];

		if (lo_diff < hi_diff) {
			return lo - 1U;
		}
	}
	return lo;
}

/*
 * Linear sensor gain register (e.g. OV5647's AGC_GAIN, reg_per_1x units per
 * 1.0x) -> library gain units (1024 = 1x).
 */
static inline uint32_t isp_sns_gain_linear_reg_to_lib(uint32_t reg, uint32_t reg_per_1x)
{
	if (reg_per_1x == 0U) {
		return ISP_SNS_GAIN_LIB_UNITY;
	}
	return (uint32_t)((uint64_t)reg * ISP_SNS_GAIN_LIB_UNITY / reg_per_1x);
}

/*
 * Library gain units (1024 = 1x) -> linear sensor gain register (reg_per_1x
 * units per 1.0x).
 */
static inline uint32_t isp_sns_gain_lib_to_linear_reg(uint32_t total_1024, uint32_t reg_per_1x)
{
	return (uint32_t)((uint64_t)total_1024 * reg_per_1x / ISP_SNS_GAIN_LIB_UNITY);
}

/*
 * Sensor exposure LINES -> the sensor's own VIDEO_CID_EXPOSURE control units
 * (ctrl_per_line: 16 for OV5647's 1/16-line control, 1 for IMX296's
 * whole-line control) -- NOT the sensor's own hardware register directly:
 * for IMX296 the ctrl value is still LINES, and imx296_set_ctrl() converts
 * that to the inversely-related SHS register itself (see this file's own
 * header comment).
 */
static inline uint32_t isp_sns_exposure_lines_to_ctrl(uint32_t lines, uint32_t ctrl_per_line)
{
	return lines * ctrl_per_line;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_VIDEO_ISP_SNS_GAIN_CONV_H_ */
