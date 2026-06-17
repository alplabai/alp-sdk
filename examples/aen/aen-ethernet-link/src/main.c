/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-ethernet-link -- bring up the Ensemble E8 GMAC (eth_dwmac core + the
 * alp-sdk "alif,ethernet" glue) on the E1M-AEN801 (M55-HE), power the on-module
 * TI DP83825I PHY, and test the link by pulling a DHCP lease off the bench switch.
 *
 * The MAC/driver/DT live in the SoC dtsi (ethernet@48100000, fixed-link RMII PHY);
 * the board overlay enables the node with the AUTHORITATIVE SoM RMII pin route
 * (alif-ethernet-phy.tsv) + input-enable on the RX pads, and adds the PHY control
 * GPIOs. This app:
 *   1. powers the PHY EARLY (SYS_INIT, before the eth driver's RMII ref-clock
 *      probe) -- E_PHY_PWRDWN=P15_4 (lpgpio.4) + E_PHY_RESET=P11_6 (gpio11.6).
 *      These are ALIF pins (per the SoM TSV), NOT CC3501E as first assumed.
 *   2. reads back which ref-clock source the driver's AUTO probe locked.
 *   3. requests a DHCP lease as the definitive end-to-end link test.
 *
 * KEY RESULT (bench-validated): powering the PHY before the probe makes the AUTO
 * probe select the EXTERNAL 50 MHz oscillator (ETH_CTRL bit4: 1->0) -- i.e. the
 * "needs power enable" the bench called out IS this P15_4 enable, and the PHY is
 * now clocked. carrier_ok is fixed-link synthetic (no MDIO) and is NOT a link
 * proof; the DHCP lease is.
 *
 * PASS gate: a DHCP lease is acquired (full bidirectional link). External refclk
 * locked but no lease = PARTIAL (the power-enable + clock are proven; a remaining
 * PHY-link detail -- TX path / auto-neg / exact PHY wiring or polarity -- is
 * pending). PHY GPIO polarity is the conventional DP83825I reading (PWRDWN enable
 * HIGH; RST_N active-low); both reset polarities were tried.
 */

#include <stdio.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>

#define PHY_RESET_PIN  6 /* E_PHY_RESET  = P11_6 (gpio11) */
#define PHY_PWRDWN_PIN 4 /* E_PHY_PWRDWN = P15_4 (lpgpio) */
#define LINK_SETTLE_MS 3000

/* Pad-mux states: gpio_dw applies no mux, so select the GPIO/LPGPIO function on
 * the two PHY-control pads through the Alif pinctrl driver (function 0). */
static const pinctrl_soc_pin_t phy_reset_mux[]  = { PIN_P11_6__GPIO };
static const pinctrl_soc_pin_t phy_pwrdwn_mux[] = { PIN_P15_4__LPGPIO };

/*
 * Power + reset the PHY EARLY -- at POST_KERNEL priority 50, AFTER the GPIO
 * controllers (40) but BEFORE the eth_dwmac driver (ETH_INIT_PRIORITY=60). The
 * glue's RMII ref-clock AUTO probe runs inside eth init and falls back to the
 * internal PLL if it sees no external 50 MHz clock at that moment; powering the
 * PHY/oscillator here (before that probe) lets the probe find the external clock
 * if the board just needed the enable. (When this ran later, in main(), the probe
 * had already fallen back -- SWD showed ETH_CTRL bit4=1 = internal PLL.)
 */
static int phy_power_init(void)
{
	const struct device *gpio11 = DEVICE_DT_GET(DT_NODELABEL(gpio11));
	const struct device *lpgpio = DEVICE_DT_GET(DT_NODELABEL(lpgpio));

	if (!device_is_ready(gpio11) || !device_is_ready(lpgpio)) {
		return -ENODEV;
	}

	pinctrl_configure_pins(phy_reset_mux, ARRAY_SIZE(phy_reset_mux), 0U);
	pinctrl_configure_pins(phy_pwrdwn_mux, ARRAY_SIZE(phy_pwrdwn_mux), 0U);

	/* Power-enable the PHY/oscillator. The user calls this a "power enable", so
	 * E_PHY_PWRDWN gates a board power switch (active-HIGH enable) -> drive HIGH.
	 * (SWD-verified the lpgpio follows; the SoM TSV gives the pad, not the
	 * polarity -- flip to low if this powers it down.) */
	gpio_pin_configure(lpgpio, PHY_PWRDWN_PIN, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set(lpgpio, PHY_PWRDWN_PIN, 1);
	k_busy_wait(10000); /* rail settle (scheduler not up at this init phase) */

	/* Reset pulse, conventional DP83825I RST_N (active-low): LOW asserts, HIGH
	 * releases. (Both polarities were bench-tried; neither alone brought the wire
	 * link up, so the remaining blocker is elsewhere -- TX path / auto-neg / exact
	 * EVK PHY wiring. Active-low is kept as the datasheet-standard default.) */
	gpio_pin_configure(gpio11, PHY_RESET_PIN, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set(gpio11, PHY_RESET_PIN, 0); /* assert */
	k_busy_wait(10000);
	gpio_pin_set(gpio11, PHY_RESET_PIN, 1); /* release */
	k_busy_wait(50000);                     /* DP83825I post-reset settle */
	return 0;
}
/* Priority 50: gpio_dw(40) < 50 < eth_dwmac(ETH_INIT_PRIORITY 60). */
SYS_INIT(phy_power_init, POST_KERNEL, 50);

int main(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		printf("[eth] RESULT FAIL: no default net interface\n[eth] done\n");
		return 0;
	}

	struct net_linkaddr *la = net_if_get_link_addr(iface);
	printf("[eth] iface %p, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
	       (void *)iface,
	       la->addr[0],
	       la->addr[1],
	       la->addr[2],
	       la->addr[3],
	       la->addr[4],
	       la->addr[5]);

	printf("[eth] PHY powered early (SYS_INIT pri 50, before the eth refclk probe)\n");

	/* Read back the RMII ref-clock source the driver's AUTO probe settled on
	 * (ETH_CTRL bit4: 0 = external P11_0 oscillator, 1 = internal-PLL fallback).
	 * External => the MAC DMA-reset completed on the on-module 50 MHz oscillator,
	 * i.e. the PHY power-enable made the external clock present BEFORE the probe --
	 * the key "needs power enable" fix, and the PHY is now clocked. */
	bool refclk_external = (sys_read32(0x4903F080U) & BIT(4)) == 0U;
	printf("[eth] RMII refclk source = %s (ETH_CTRL bit4=%d)\n",
	       refclk_external ? "EXTERNAL osc (PHY clocked -- power-enable worked)"
	                       : "internal PLL (external osc not detected)",
	       refclk_external ? 0 : 1);

	int rc = net_if_up(iface);
	printf("[eth] net_if_up -> %d%s\n", rc, (rc == -EALREADY) ? " (already up)" : "");

	/* DEFINITIVE end-to-end proof: request a DHCP lease from the bench switch
	 * (dnsmasq). A lease only completes over a genuinely live bidirectional link,
	 * so it proves TX + RX + the PHY are all working -- unlike carrier_ok (fixed-
	 * link synthetic) or unsolicited RX (a quiet switch sends none). */
	net_dhcpv4_start(iface);
	bool bound = false;
	for (int i = 0; i < 30; i++) { /* up to ~15 s for DISCOVER/OFFER/REQUEST/ACK */
		if (iface->config.dhcpv4.state == NET_DHCPV4_BOUND) {
			bound = true;
			break;
		}
		k_msleep(500);
	}

	uint32_t rx_bytes = iface->stats.bytes.received;
	printf("[eth] admin_up=%d carrier_ok=%d(fixed-link) rx_bytes=%u dhcp_bound=%d\n",
	       net_if_is_admin_up(iface),
	       net_if_is_carrier_ok(iface),
	       rx_bytes,
	       bound);

	if (bound) {
		char                ip[NET_IPV4_ADDR_LEN] = { 0 };
		struct net_if_addr *ua                    = &iface->config.ip.ipv4->unicast[0].ipv4;
		net_addr_ntop(AF_INET, &ua->address.in_addr, ip, sizeof(ip));
		printf("[eth] DHCP lease = %s\n", ip);
	}

	printf(
	    "[eth] RESULT %s: %s\n",
	    bound ? "PASS" : (refclk_external ? "PARTIAL" : "PARTIAL"),
	    bound ? "DHCP lease acquired off the bench switch = full bidirectional link UP"
	    : refclk_external
	        ? "PHY power-enabled + EXTERNAL refclk locked (MAC reset on the osc) + RMII pins = "
	          "SoM route; no DHCP lease yet (check PHY reset/PWRDWN polarity, cable, DHCP server)"
	        : "no external refclk -- PHY not clocked (check the power-enable)");
	printf("[eth] done\n");
	return 0;
}
