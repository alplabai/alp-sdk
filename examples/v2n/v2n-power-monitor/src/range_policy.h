/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shunt-range policy for the INA228 `--ina228-range auto` mode: a pure
 * function of the current range, the measured shunt voltage and an over-range
 * flag, with no I/O, so it can be unit-tested without hardware
 * (tests/zephyr/chips/src/test_ina228.c).
 *
 * The INA228 has two shunt scales (SLYS021A table 7-5): +/-163.84 mV
 * (312.5 nV/LSB, the wide range) and +/-40.96 mV (78.125 nV/LSB, four times
 * the resolution).  Auto mode starts on the wide range and
 *   - drops to the narrow range when the shunt voltage falls below 75 % of
 *     the narrow full scale (30720 uV), leaving headroom;
 *   - climbs back to the wide range when it reaches 95 % of the narrow full
 *     scale (38912 uV), or immediately when the narrow range is clipped;
 *   - holds the current range in the 30720..38912 uV band between the two
 *     (the hysteresis), so a reading hovering near one threshold does not
 *     flip the range every sample.
 */

#ifndef V2N_POWER_MONITOR_RANGE_POLICY_H
#define V2N_POWER_MONITOR_RANGE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define RANGE_POLICY_NARROW_FULL_SCALE_UV 40960 /**< +/-40.96 mV in uV. */
#define RANGE_POLICY_DOWN_UV              30720 /**< 75 % of the narrow full scale. */
#define RANGE_POLICY_UP_UV                38912 /**< 95 % of the narrow full scale. */

/**
 * @brief Decide the INA228 range for the next sample.
 *
 * @param on_narrow    True when the 40.96 mV range is selected now.
 * @param shunt_uv     Measured shunt voltage in micro-volts (either sign).
 * @param over_range   True when the reading is clipped (ina228_check_over_range()).
 * @return true to use the narrow (40.96 mV) range next, false for the wide one.
 */
static inline bool range_policy_want_narrow(bool on_narrow, int32_t shunt_uv, bool over_range)
{
	int32_t mag = (shunt_uv < 0) ? -shunt_uv : shunt_uv;

	if (on_narrow) {
		/* Clipped, or at 95 % of full scale or more: go wide. */
		return !(over_range || mag >= RANGE_POLICY_UP_UV);
	}
	/* On the wide range: go narrow only when well inside it and not clipped. */
	return !over_range && mag < RANGE_POLICY_DOWN_UV;
}

#endif /* V2N_POWER_MONITOR_RANGE_POLICY_H */
