/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal pinctrl_soc.h so CONFIG_PINCTRL builds on native_sim (pattern of
 * Zephyr's tests/drivers/pinctrl/api).  A pin is just the `pins` cell.
 */
#ifndef POWER_SOM_PM_POLICY_PINCTRL_SOC_H_
#define POWER_SOM_PM_POLICY_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>

typedef uint32_t pinctrl_soc_pin_t;

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx),

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop) \
	{ DT_FOREACH_CHILD_VARGS( \
		DT_PROP_BY_IDX(node_id, prop, 0), DT_FOREACH_PROP_ELEM, pins, Z_PINCTRL_STATE_PIN_INIT) }

#endif
