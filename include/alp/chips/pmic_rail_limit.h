/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pmic_rail_limit.h
 * @brief Per-rail runtime guard entry shared by the on-module PMIC /
 *        regulator drivers (ACT88760, DA9292, TPS628640).
 *
 * A limits table is the ONLY thing that unlocks a control write in those
 * drivers.  Each driver keeps a pointer to a caller-owned table installed
 * with its `*_set_limits()` call; with no table installed every control
 * write (voltage, enable/disable, GPIO polarity, raw register write,
 * sequence) returns ::ALP_ERR_NOSUPPORT -- fail-closed.  Read APIs never
 * need a table.
 *
 * The tables are not hand-written: `scripts/gen_power_tree.py` projects
 * them from the SoM family power tree
 * (`metadata/e1m_modules/<family>/power-tree.yaml`) into a generated
 * header such as `<alp/chips/v2n_power_tree.h>`, as brace initialisers the
 * caller instantiates in its own storage (the drivers store only the
 * pointer, so the table must outlive the driver context).
 *
 * Guard semantics every driver applies to a rail entry:
 *   - voltage write: refused (::ALP_ERR_NOSUPPORT) unless
 *     @ref pmic_rail_limit_t::voltage_writable; refused
 *     (::ALP_ERR_OUT_OF_RANGE) unless the ENCODED setpoint the driver
 *     would program lies inside [@ref pmic_rail_limit_t::min_mv,
 *     @ref pmic_rail_limit_t::max_mv];
 *   - enable: refused (::ALP_ERR_NOSUPPORT) unless
 *     @ref pmic_rail_limit_t::enable_writable;
 *   - disable: refused (::ALP_ERR_NOSUPPORT) unless
 *     @ref pmic_rail_limit_t::enable_writable AND NOT
 *     @ref pmic_rail_limit_t::critical -- a critical rail can never be
 *     turned off by software, whatever else the table says.
 *
 * @par The ONE enable-time window rule
 * Every driver applies exactly this rule when @p enable is `true`:
 * **enable refuses (::ALP_ERR_OUT_OF_RANGE) when a window exists
 * (`max_mv != 0` -- `min_mv`/`max_mv` both `0` means "no window", e.g. an
 * enable-only load switch) and the live setpoint(s) the chip would
 * energize is outside it.**  This check runs independent of
 * @ref pmic_rail_limit_t::voltage_writable -- a rail can have a window
 * worth enforcing before it is switched on even when software is not
 * allowed to reprogram its voltage.  ACT88760 (act8760_rail_set_enable()),
 * DA9292 (da9292_set_enable()) and TPS628640 (tps628640_software_enable(),
 * which checks VOUT1 *and* VOUT2 because the VID pin picks which one is
 * live) all read the rail's actual live setpoint(s) back over I2C and
 * check them against the window before energizing anything -- never skip
 * this check on the theory that the rail "isn't voltage-writable so
 * there's nothing to validate".
 *
 * One driver-specific exception to note when reading any of the three:
 * ACT88760 and TPS628640 treat "no window" (`max_mv == 0`) as "nothing to
 * check", so enabling a windowless entry proceeds once `enable_writable`
 * allows it at all.  DA9292's da9292_set_enable() is stricter -- it
 * refuses (::ALP_ERR_NOSUPPORT) enabling a windowless entry outright,
 * since CH2 has no OTP-safe default worth energizing blind to.  Both are
 * "fail-closed when the window can't be checked", just at different
 * points: DA9292 closes at the missing-window case itself, ACT88760 /
 * TPS628640 close only once a window exists and the live reading
 * disagrees with it.
 */

#ifndef ALP_CHIPS_PMIC_RAIL_LIMIT_H
#define ALP_CHIPS_PMIC_RAIL_LIMIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One rail's guard window + control permissions (all-zero = no control). */
typedef struct {
	uint16_t min_mv;           /**< Lowest writable setpoint, inclusive (0 = no window). */
	uint16_t max_mv;           /**< Highest writable setpoint, inclusive (0 = no window). */
	bool     critical;         /**< The running SoC depends on it: never disabled. */
	bool     voltage_writable; /**< Voltage writes allowed (inside the window). */
	bool     enable_writable;  /**< Enable writes allowed (disable also needs !critical). */
} pmic_rail_limit_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_PMIC_RAIL_LIMIT_H */
