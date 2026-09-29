/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Strong override of the SDK's WEAK (empty) cc3501e_gpio_routes[]: the two
 * CC3501E-proxied pins this app drives, the I2S0 mux in front of the TAS2563
 * amps. Same table as the bench-proven aen-i2s-tas2563-probe; hw_rev
 * 2626-r2 routes (IO8 -> GPIO_30 holds only on r2 -- the SDK's per-pin guard
 * refuses IO8 unless the identity EEPROM confirms CONFIG_ALP_SDK_SOM_HW_REV).
 */
#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ ALP_E1M_GPIO_IO13, 13u }, /* I2S0 mux S (I2S_SELECT): 0 = TAS2563 amps. NEVER drive 1 on a reworked carrier. */
	{ ALP_E1M_GPIO_IO8, 30u },  /* I2S0 mux /E (I2S_EN), active low. */
};
const size_t cc3501e_gpio_route_count = sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
