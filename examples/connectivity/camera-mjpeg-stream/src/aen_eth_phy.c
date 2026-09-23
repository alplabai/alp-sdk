/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * INTERIM AEN-only Ethernet PHY bring-up: power + reset the on-module TI
 * DP83825 PHY and put it in 50 MHz-reference RMII mode. There is no
 * portable alp-sdk PHY header yet (see check_example_portability.py's
 * `_ZEPHYR_DRIVER_INCLUDE_ALLOWLIST` entry for this example), so this
 * file reaches straight into the raw Zephyr GPIO/pinctrl driver API and
 * the GMAC's own MMIO MDIO registers -- the pads it drives are SoC-
 * internal PHY control lines, not the E1M portable pin namespace. It is
 * compiled in ONLY for AEN board targets (CMakeLists.txt's
 * `BOARD MATCHES "^alp_e1m_aen"` guard) -- drop this whole file once
 * board generation grows a real Ethernet PHY surface of its own; main.c
 * and mjpeg_http.c never reference it.
 *
 * A sibling of this bring-up sequence already exists in
 * examples/aen/aen-ethernet-link's main.c; consolidating both into one
 * shared board-support source is real follow-up work, not done here --
 * that file lives in a different example and touching it is outside this
 * change's scope. Duplication is the known, accepted cost of INTERIM
 * until board generation removes the need for either copy.
 *
 * Both steps below are plain SYS_INIT hooks: they run themselves, in
 * priority order, before main() -- main.c's camera-capture loop and the
 * HTTP server thread never need to know Ethernet exists.  Priorities:
 *   50  phy_power_init   -- GPIO power/reset (gpio_dw inits at 40)
 *   60  (eth_dwmac driver's own ETH_INIT_PRIORITY, elsewhere)
 *   70  phy_refclk_init  -- MDIO refclk-mode set + autoneg restart, AFTER
 *                           the eth_dwmac driver has clocked the GMAC
 *                           (its MDIO registers are otherwise dead)
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define PHY_RESET_PIN  6 /* E_PHY_RESET  = P11_6 (gpio11) */
#define PHY_PWRDWN_PIN 4 /* E_PHY_PWRDWN = P15_4 (lpgpio) */

static const pinctrl_soc_pin_t phy_reset_mux[]  = { PIN_P11_6__GPIO };
static const pinctrl_soc_pin_t phy_pwrdwn_mux[] = { PIN_P15_4__LPGPIO };

/* Power + reset the PHY EARLY, before the eth_dwmac driver's RMII
 * ref-clock AUTO probe runs (ETH_INIT_PRIORITY=60): the probe falls back
 * to the internal PLL if it sees no external 50 MHz clock at that moment,
 * and the PHY/oscillator is what supplies it. */
static int phy_power_init(void)
{
	const struct device *gpio11 = DEVICE_DT_GET(DT_NODELABEL(gpio11));
	const struct device *lpgpio = DEVICE_DT_GET(DT_NODELABEL(lpgpio));

	if (!device_is_ready(gpio11) || !device_is_ready(lpgpio)) {
		return -ENODEV;
	}

	pinctrl_configure_pins(phy_reset_mux, ARRAY_SIZE(phy_reset_mux), 0U);
	pinctrl_configure_pins(phy_pwrdwn_mux, ARRAY_SIZE(phy_pwrdwn_mux), 0U);

	/* E_PHY_PWRDWN gates a board power switch, active-HIGH enable. */
	gpio_pin_configure(lpgpio, PHY_PWRDWN_PIN, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set(lpgpio, PHY_PWRDWN_PIN, 1);
	k_busy_wait(50000); /* let the PHY supply + its reference clock stabilize */

	/* Reset pulse, conventional DP83825 RST_N (active-low). */
	gpio_pin_configure(gpio11, PHY_RESET_PIN, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set(gpio11, PHY_RESET_PIN, 0); /* assert */
	k_busy_wait(50000);
	gpio_pin_set(gpio11, PHY_RESET_PIN, 1); /* release */
	k_busy_wait(100000);                    /* DP83825 post-reset settle (>=50 ms) */
	return 0;
}
SYS_INIT(phy_power_init, POST_KERNEL, 50);

/* Raw MDIO access via the DWMAC MAC_MDIO registers (GMAC base
 * 0x48100000) -- this app's fixed-link DT config does no MDIO of its own,
 * so this pokes the PHY directly. MAC_MDIO_ADDRESS=0x200 (PA[25:21],
 * RDA[20:16], CR[11:8], GOC read=bit3+bit2, GB=bit0),
 * MAC_MDIO_DATA=0x204 (data[15:0]). CR=4 -> slow, safe MDC. */
#define GMAC_MDIO_ADDR  0x48100200U
#define GMAC_MDIO_DATA  0x48100204U
#define MDIO_POLL_ITERS 100000 /* ~100ms at the 1us busy_wait below -- never spin forever */

/* Poll GB (bit0, "transaction in flight") down to 0, bounded. Returns
 * false on timeout -- a genuinely wedged MDIO bus (e.g. no PHY answering,
 * or the GMAC itself unclocked) must not hang this SYS_INIT hook, which
 * would hang the whole boot behind it. */
static bool mdio_wait_idle(void)
{
	for (int i = 0; i < MDIO_POLL_ITERS; i++) {
		if (!(sys_read32(GMAC_MDIO_ADDR) & BIT(0))) {
			return true;
		}
		k_busy_wait(1);
	}
	return false;
}

static uint16_t mdio_read(uint8_t phy, uint8_t reg)
{
	if (!mdio_wait_idle()) {
		return 0xFFFF; /* matches phy_find()'s own "nothing here" sentinel */
	}
	uint32_t a =
	    ((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(3) | BIT(2) | BIT(0);
	sys_write32(a, GMAC_MDIO_ADDR);
	mdio_wait_idle();
	return (uint16_t)(sys_read32(GMAC_MDIO_DATA) & 0xFFFFU);
}

static void mdio_write(uint8_t phy, uint8_t reg, uint16_t val)
{
	if (!mdio_wait_idle()) {
		return;
	}
	sys_write32(val, GMAC_MDIO_DATA);
	uint32_t a = ((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(2) | BIT(0);
	sys_write32(a, GMAC_MDIO_ADDR);
	mdio_wait_idle();
}

/* Find the PHY (returns addr 0-31, or -1). DP83825 OUI = 0x2000a140. */
static int phy_find(void)
{
	for (uint8_t phy = 0; phy < 32; phy++) {
		uint16_t id1 = mdio_read(phy, 2);

		if (id1 != 0xFFFF && id1 != 0x0000) {
			return phy;
		}
	}
	return -1;
}

/* Put the PHY in 50 MHz-reference RMII mode (RCSR(0x17) bit7
 * REF_CLK_SEL=1) and restart auto-negotiation. Bench-confirmed on
 * aen-ethernet-link: with bit7=1 the PHY forms a media link; with bit7=0
 * it does not. This app does not block waiting for the link the way
 * aen-ethernet-link's diagnostic main() does -- Zephyr's own DHCP client
 * (started from main.c) just keeps retrying until the wire comes up, so
 * the camera-capture loop is never held hostage by it. */
static int phy_refclk_init(void)
{
	const struct device *eth = DEVICE_DT_GET(DT_NODELABEL(ethernet));

	if (!device_is_ready(eth)) {
		/* The eth_dwmac driver's own init (priority 60, before this
		 * hook at 70) failed or never ran -- the GMAC's MDIO
		 * registers are not safely accessible. */
		return -ENODEV;
	}

	int phy = phy_find();

	if (phy < 0) {
		return -ENODEV;
	}

	uint16_t rcsr = mdio_read(phy, 0x17);
	mdio_write(phy, 0x17, rcsr | BIT(7));
	mdio_write(phy, 0, BIT(12) | BIT(9)); /* BMCR: auto-neg enable + restart */
	return 0;
}
/* Priority 70: after the eth_dwmac driver (ETH_INIT_PRIORITY=60) has
 * clocked the GMAC -- MDIO transactions are dead before that. */
SYS_INIT(phy_refclk_init, POST_KERNEL, 70);
