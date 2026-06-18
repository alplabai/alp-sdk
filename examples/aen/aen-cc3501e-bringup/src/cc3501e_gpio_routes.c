/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * E1M-AEN SoM (BDE-BW35N / CC3501E) GPIO-proxy route table.
 *
 * Strong override of the WEAK cc3501e_gpio_routes[] / cc3501e_gpio_route_count in
 * src/backends/gpio/cc3501e_proxy.c.  Maps the portable E1M GPIO pin_id
 * (alp_gpio_open(E1M_GPIO_IOxx)) -> the RAW CC3501E GPIO index the inter-chip bridge
 * drives, so an alp_gpio_* call on a proxied E1M IO is routed over the bridge while
 * the Alif's own pins delegate to the platform driver.
 *
 * Built from the E1M-AEN netlist: SoM U4 = BDE-BW35N, edge connector E1 -- each
 * WIFI_GPIOxx net ties U4.GPIO_N to E1.<pin> (PinName = IOxx) which mates the carrier
 * E2.<same coord>; the carrier net gives the function.  e1m_pinout.h's pad comments
 * (e.g. IO13 = pad E3) match the connector coords, confirming the binding.
 *
 * *** NEXT-REV SoM netlist -- "similar to, NOT identical to" the current bench rev.
 * Confirm against the current rev before trusting on silicon. ***
 *
 * IO17 (EN_W_DIS1n) is intentionally OMITTED: its CC3501E pin GPIO_16 is the bridge
 * SPI0 dummy-CS this rev, so it is not host-proxied (bench call: "GPIO16 is ok for now").
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ E1M_GPIO_IO8,  30u }, /* I2S_EN           <- CC35 GPIO_30 */
	{ E1M_GPIO_IO9,  12u }, /* PCIE_IO_EXP.RST  <- GPIO_12 */
	{ E1M_GPIO_IO10, 35u }, /* PCIE0_I2C.EN     <- GPIO_35 */
	{ E1M_GPIO_IO11, 2u },  /* USB2_SELECT      <- GPIO_2  */
	{ E1M_GPIO_IO13, 13u }, /* I2S_SELECT       <- GPIO_13 */
	{ E1M_GPIO_IO15, 14u }, /* S_BMI323.INT1    <- GPIO_14 */
	{ E1M_GPIO_IO16, 17u }, /* EN_W_DIS2n       <- GPIO_17 (open-drain W_DISABLE2) */
	{ E1M_GPIO_IO18, 18u }, /* M2E_SDIO_WAKEn   <- GPIO_18 */
	{ E1M_GPIO_IO19, 19u }, /* M2E_UART.WAKEn_L <- GPIO_19 */
	{ E1M_GPIO_IO20, 26u }, /* MUX_EN           <- GPIO_26 */
};

const size_t cc3501e_gpio_route_count =
	sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
