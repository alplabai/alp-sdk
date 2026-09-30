/* src/platform/ina236_math.h -- pure INA236 shunt/bus -> power conversion
 * (TI SBOSA81D), reading only the raw SHUNT (0x01) and BUS (0x02) registers.
 *
 * Deliberately does NOT go through the part's CALIBRATION/CURRENT/POWER
 * registers. The alp-sdk chips/ina236 driver this SDK build (alp-sdk-lcd)
 * ships has two real scaling bugs in that path (alp-sdk commit 38784b723,
 * not yet on alp-sdk dev/main, so alp-sdk-lcd still carries them):
 *   1. ina236_read_power_uw() applies the 1.6 mV bus LSB a second time on
 *      top of the 32 that already carries it (eq. 4) -- 625x under-report.
 *   2. apply_calibration() never divides SHUNT_CAL by 4 for ADCRANGE=1
 *      (section 8.1.2) -- 4x high current/power on the fine range.
 * Computing P = V_bus * (V_shunt / R_shunt) straight from the two raw ADC
 * registers cannot inherit either bug -- there is no SHUNT_CAL in this path.
 *
 * Header-only + static inline: usable unmodified from both the target build
 * (platform/rail5v_power.c) and the host tests (tests/host/runner.sh's generic
 * source glob does not include the src/platform directory, so a separate
 * .c file here would need its own build-script entry; a header needs none).
 */
#ifndef TR_PLATFORM_INA236_MATH_H
#define TR_PLATFORM_INA236_MATH_H

#include <stdbool.h>
#include <stdint.h>

/* Bus-voltage LSB is fixed at 1.6 mV (SBOSA81D section 7.6.1, BUS_VOLTAGE
 * register). raw is the signed 16-bit register value. */
static inline int32_t tr_ina236_bus_mv(int16_t raw)
{
	return ((int32_t)raw * 16) / 10;
}

/* Shunt-voltage LSB depends on ADCRANGE (table 7-4): false (ADCRANGE=0) is
 * the +-81.92 mV range at 2.5 uV/LSB; true (ADCRANGE=1) is the +-20.48 mV
 * range at 0.625 uV/LSB. raw is the signed 16-bit SHUNT_VOLTAGE register. */
static inline int32_t tr_ina236_shunt_uv(int16_t raw, bool adcrange_20mv)
{
	int32_t lsb_nv = adcrange_20mv ? 625 : 2500;

	return (int32_t)(((int64_t)raw * lsb_nv) / 1000);
}

/* I[uA] = V_shunt[uV] / R_shunt[ohm] -- Ohm's law across the sense
 * resistor, not the part's internal CURRENT_LSB/SHUNT_CAL round-trip. */
static inline int32_t tr_ina236_current_ua(int32_t shunt_uv, float shunt_ohms)
{
	return (int32_t)((double)shunt_uv / (double)shunt_ohms);
}

/* P[mW] = V_bus[V] * I[A], done in integer micro-units: mV * uA is
 * 1e-3 V * 1e-6 A = 1e-9 W = nW; /1000 for mW. Never negative (a module
 * rail does not source current back onto the +5V input). */
static inline int32_t tr_ina236_power_mw(int32_t bus_mv, int32_t current_ua)
{
	int64_t nw = (int64_t)bus_mv * (int64_t)current_ua;

	if (nw < 0) {
		nw = 0;
	}
	return (int32_t)(nw / 1000000);
}

/* CONFIGURATION register (0x00, SBOSA81D 7.6.1.1): RST[15], reserved
 * [14:13] (read 10b), ADCRANGE[12], AVG[11:9], VBUSCT[8:6], VSHCT[5:3],
 * MODE[2:0]. Power-on reset 0x4127 = AVG 1: one 1.1 ms shunt conversion.
 * rail5v_power.c writes TR_INA236_CONFIG instead: ADCRANGE 0 (+-81.92 mV,
 * what tr_ina236_shunt_uv(.., false) assumes), AVG 128 (100b), VBUSCT =
 * VSHCT = 1.1 ms (100b), continuous shunt + bus (111b) -- one result is
 * the mean over 128 x 2.2 ms = 282 ms, just under the 320 ms poll. */
#define TR_INA236_REG_CONFIG 0x00u
#define TR_INA236_CONFIG     0x4927u
#define TR_INA236_CONFIG_RW  0x1FFFu /* the bits a write sets: ADCRANGE..MODE */

/* A CONFIG readback carries the settings this file's maths assumes. */
static inline bool tr_ina236_config_ok(uint16_t readback)
{
	return (readback & TR_INA236_CONFIG_RW) == (TR_INA236_CONFIG & TR_INA236_CONFIG_RW);
}

#endif /* TR_PLATFORM_INA236_MATH_H */
