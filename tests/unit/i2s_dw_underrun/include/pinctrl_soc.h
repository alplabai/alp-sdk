/* SPDX-License-Identifier: Apache-2.0 */
/*
 * alp-sdk issue #2149 (round 2 review finding 4): native_sim ships no
 * <pinctrl_soc.h> (that only exists for boards that actually enable
 * CONFIG_PINCTRL, e.g. nrf_bsim) -- but <zephyr/drivers/pinctrl.h>
 * #includes it UNCONDITIONALLY, with no CONFIG_PINCTRL guard, and
 * zephyr/drivers/i2s/i2s_dw.c #includes <zephyr/drivers/pinctrl.h>
 * unconditionally too. This test never calls any PINCTRL_DT_*() macro
 * (they only appear inside the I2S_DW_INIT() device-instantiation macro,
 * which this test's zero DT instances never expand), so all this stub
 * needs to satisfy is pinctrl.h's own reference to `pinctrl_soc_pin_t`.
 */
#ifndef PINCTRL_SOC_H_
#define PINCTRL_SOC_H_

typedef int pinctrl_soc_pin_t;

#endif /* PINCTRL_SOC_H_ */
