/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Pad mux / pad-configuration state of the SoM power layer (#2784, U5).
 *
 * The generated `alp,som-power` node carries a pinctrl-0 "default" state that
 * covers every pad the layer drives (P15_n on the LPGPIO island, P11_6, P5_5).
 * Writing a level to an unmuxed pad reports success and does nothing, so the
 * layer applies this state before its first drive and fails when it is absent.
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>

#include <alp/peripheral.h>

#include "som_power.h"

#define SOMPD_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(alp_som_power)

#if DT_HAS_COMPAT_STATUS_OKAY(alp_som_power) && DT_NODE_HAS_PROP(SOMPD_NODE, pinctrl_0)

PINCTRL_DT_DEFINE(SOMPD_NODE);

alp_status_t alp_som_power_pads_apply(void)
{
	const struct pinctrl_dev_config *cfg = PINCTRL_DT_DEV_CONFIG_GET(SOMPD_NODE);

	return (pinctrl_apply_state(cfg, PINCTRL_STATE_DEFAULT) == 0) ? ALP_OK : ALP_ERR_IO;
}

#else

alp_status_t alp_som_power_pads_apply(void)
{
	return ALP_ERR_NOT_READY; /* no pinctrl-0 on the alp,som-power node */
}

#endif
