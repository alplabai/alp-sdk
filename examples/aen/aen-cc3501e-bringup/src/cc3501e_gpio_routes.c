/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * E1M-AEN SoM (BDE-BW35N / CC3501E) GPIO-proxy route table.
 *
 * Strong override of the WEAK cc3501e_gpio_routes[] / cc3501e_gpio_route_count in
 * src/backends/gpio/cc3501e_proxy.c.  Maps the portable E1M GPIO pin_id
 * (alp_gpio_open(ALP_E1M_GPIO_IOxx)) -> the RAW CC3501E GPIO index the inter-chip bridge
 * drives, so an alp_gpio_* call on a proxied E1M IO is routed over the bridge while
 * the Alif's own pins delegate to the platform driver.
 *
 * Sanitized E1M-AEN route metadata maps each proxied WIFI_GPIOxx signal to the
 * public E1M GPIO ID that application code opens.  Confirm the table against
 * the active board metadata before relying on it for a new hardware revision.
 *
 * CONFLICT, UNRESOLVED: IO16 (EN_W_DIS2n) maps to CC3501E pin GPIO_17, which is
 * ALSO the bridge READY/host-IRQ line (CC35 GPIO17 -> Alif P2_6).  The firmware
 * owns that pin and reserves it in gpio_pad_reserved(), so every proxy command on
 * IO16 is refused -- alp_gpio_open(ALP_E1M_GPIO_IO16) looks wired and never works.
 *
 * The entry is kept because the SoM metadata declares the mapping and
 * tests/scripts/test_aen_cc3501e_routes.py derives this table from it: dropping
 * it here alone just makes the example disagree with the metadata.  Resolving it
 * properly is a METADATA decision -- either IO16 stops being advertised as a
 * proxied IO on a board where GPIO_17 is the READY line, or READY moves.  Do not
 * "fix" it by editing this table in isolation.
 *
 * IO17 (EN_W_DIS1n) is intentionally OMITTED: its CC3501E pin GPIO_16 is the bridge
 * SPI0 dummy-CS this rev, so it is not host-proxied (bench call: "GPIO16 is ok for now").
 *
 * cc3501e_gpio_unrouted[] is DELIBERATELY left at the WEAK empty default
 * (src/backends/gpio/cc3501e_proxy.c) rather than populated with IO21: the
 * bench module this example targets is r1 (`alp board` -> E1M-AEN801 r1),
 * where IO21 IS routed to CC3501E GPIO_30 -- only r2 leaves it open. The
 * SoM metadata's `dispatch: unrouted` on r2 (issue #1854) is revision-
 * specific and this table is not yet revision-aware (#1859); populate it
 * from the composed route table once that lands, and only once a strong
 * override of this WEAK array is verified to actually survive compilation
 * (issue #1860: it was constant-folded away and dropped from the ELF
 * under -Os on the real target).
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>
#include <alp/e1m_pinout.h>

const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {
	{ ALP_E1M_GPIO_IO8, 30u },  /* I2S_EN           <- CC35 GPIO_30 */
	{ ALP_E1M_GPIO_IO9, 12u },  /* PCIE_IO_EXP.RST  <- GPIO_12 */
	{ ALP_E1M_GPIO_IO10, 35u }, /* PCIE0_I2C.EN     <- GPIO_35 */
	{ ALP_E1M_GPIO_IO11, 2u },  /* USB2_SELECT      <- GPIO_2  */
	{ ALP_E1M_GPIO_IO13, 13u }, /* I2S_SELECT       <- GPIO_13 */
	{ ALP_E1M_GPIO_IO15, 14u }, /* S_BMI323.INT1    <- GPIO_14 */
	{ ALP_E1M_GPIO_IO16, 17u }, /* EN_W_DIS2n <- GPIO_17 -- SEE THE CONFLICT NOTE ABOVE */
	{ ALP_E1M_GPIO_IO18, 18u }, /* M2E_SDIO_WAKEn   <- GPIO_18 */
	{ ALP_E1M_GPIO_IO19, 19u }, /* M2E_UART.WAKEn_L <- GPIO_19 */
	{ ALP_E1M_GPIO_IO20, 26u }, /* MUX_EN           <- GPIO_26 */
};

const size_t cc3501e_gpio_route_count =
    sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);
