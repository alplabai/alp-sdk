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
 *   - enable: refused unless @ref pmic_rail_limit_t::enable_writable;
 *   - disable: refused unless @ref pmic_rail_limit_t::enable_writable AND
 *     NOT @ref pmic_rail_limit_t::critical -- a critical rail can never be
 *     turned off by software, whatever else the table says.
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
