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
 * RESULT PASS (bench-validated, both sides): the SOM pulls a real DHCP lease off
 * the bench switch (e.g. 192.168.10.137) and is REACHABLE in the server's ARP
 * table. Three things had to be right, in order:
 *   1. PHY power before the probe -> the AUTO probe selects the EXTERNAL 50 MHz
 *      oscillator (ETH_CTRL bit4: 1->0); the PHY is clocked. The "needs power
 *      enable" the bench called out IS this P15_4 enable.
 *   2. PHY RMII clock mode: RCSR bit7 REF_CLK_SEL=1 (50 MHz ref) -> media link up
 *      (BMSR bit2, 0x786d). bit7=0 keeps it down (0x7849).
 *   3. THE DECISIVE FIX -- DMA buffers off the DTCM. The board defaults
 *      `zephyr,sram = &dtcm`, so the GMAC descriptor rings + net_buf pool sat in
 *      the M55 DTCM, which is NOT on the GMAC DMA's bus -> zero frames moved
 *      either way (SOM rx_bytes=0 AND server NIC RX=0) even with the link up. The
 *      overlay moves system RAM to the global SRAM0 (`zephyr,sram = &sram0`,
 *      @0x02000000, CPU addr == DMA addr) + CONFIG_DCACHE=n. See the overlay and
 *      the eth_dwmac_alif_ensemble.c header (its documented Tier-1.5 gap).
 *
 * PASS gate: a DHCP lease is acquired (full bidirectional link). carrier_ok is
 * fixed-link synthetic (no managed PHY) and is NOT a link proof; the lease is.
 * PHY GPIO polarity is the conventional DP83825I reading (PWRDWN enable HIGH;
 * RST_N active-low).
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
	k_busy_wait(50000); /* let the PHY supply + its reference clock stabilize */

	/* Reset pulse, conventional DP83825I RST_N (active-low): LOW asserts, HIGH
	 * releases. Long assert + post-reset settle so the reference clock is stable
	 * across the reset (a clock that isn't stable at deassert leaves the analog
	 * front-end uninitialised). Both reset polarities were bench-tried. */
	gpio_pin_configure(gpio11, PHY_RESET_PIN, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set(gpio11, PHY_RESET_PIN, 0); /* assert */
	k_busy_wait(50000);
	gpio_pin_set(gpio11, PHY_RESET_PIN, 1); /* release */
	k_busy_wait(100000);                    /* DP83825I post-reset settle (>=50 ms) */
	return 0;
}
/* Priority 50: gpio_dw(40) < 50 < eth_dwmac(ETH_INIT_PRIORITY 60). */
SYS_INIT(phy_power_init, POST_KERNEL, 50);

/*
 * Raw MDIO read via the DWMAC MAC_MDIO registers (GMAC base 0x48100000): the
 * fixed-link config does no MDIO, so we read the PHY directly to diagnose the
 * link. MAC_MDIO_ADDRESS=0x200 (PA[25:21], RDA[20:16], CR[11:8], GOC read=bit3+
 * bit2, GB=bit0), MAC_MDIO_DATA=0x204 (data[15:0]). CR=4 -> slow, safe MDC.
 */
#define GMAC_MDIO_ADDR 0x48100200U
#define GMAC_MDIO_DATA 0x48100204U

static uint16_t mdio_read(uint8_t phy, uint8_t reg)
{
	while (sys_read32(GMAC_MDIO_ADDR) & BIT(0)) {
	}
	uint32_t a =
	    ((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(3) | BIT(2) | BIT(0);
	sys_write32(a, GMAC_MDIO_ADDR);
	for (int i = 0; i < 100000 && (sys_read32(GMAC_MDIO_ADDR) & BIT(0)); i++) {
		k_busy_wait(1);
	}
	return (uint16_t)(sys_read32(GMAC_MDIO_DATA) & 0xFFFFU);
}

static void mdio_write(uint8_t phy, uint8_t reg, uint16_t val)
{
	while (sys_read32(GMAC_MDIO_ADDR) & BIT(0)) {
	}
	sys_write32(val, GMAC_MDIO_DATA);
	uint32_t a = ((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(2) | BIT(0);
	sys_write32(a, GMAC_MDIO_ADDR);
	for (int i = 0; i < 100000 && (sys_read32(GMAC_MDIO_ADDR) & BIT(0)); i++) {
		k_busy_wait(1);
	}
}

/* Find the PHY (returns addr 0-31, or -1). DP83825I OUI = 0x2000a140. */
static int phy_find(void)
{
	for (uint8_t phy = 0; phy < 32; phy++) {
		uint16_t id1 = mdio_read(phy, 2);
		if (id1 != 0xFFFF && id1 != 0x0000) {
			printf(
			    "[eth] MDIO PHY@%u id=%04x%04x (DP83825I=2000a140)\n", phy, id1, mdio_read(phy, 3));
			return phy;
		}
	}
	return -1;
}

/* Restart auto-negotiation and poll BMSR (reg 1) for link-up (bit 2) +
 * autoneg-done (bit 5). Returns true if the link comes up within ~8 s. */
static bool phy_wait_link(int phy)
{
	/*
	 * Put the PHY in 50 MHz-reference RMII mode: RCSR(0x17) bit7 REF_CLK_SEL=1
	 * (== phy_ti_dp83825's DP83825_RMII). Bench-confirmed: with bit7=1 the PHY
	 * forms a media link (BMSR bit2 set, 0x786d); with bit7=0 it does not
	 * (0x7849) -- so this board feeds the PHY's REF_CLK an external 50 MHz, and
	 * the PHY must be the RMII slave. (The data plane stalling separately turned
	 * out to be a DMA-buffers-in-DTCM placement bug, not this clock mode.)
	 */
	uint16_t rcsr = mdio_read(phy, 0x17);
	mdio_write(phy, 0x17, rcsr | BIT(7));
	printf("[eth] RCSR 0x%04x -> 0x%04x (REF_CLK_SEL=50MHz ref)\n", rcsr, mdio_read(phy, 0x17));

	mdio_write(phy, 0, BIT(12) | BIT(9)); /* BMCR: auto-neg enable + restart */
	for (int i = 0; i < 32; i++) {
		k_msleep(250);
		uint16_t bmsr = mdio_read(phy, 1);
		if ((bmsr & BIT(2)) && (bmsr & BIT(5))) {
			printf("[eth] PHY link UP after %d ms (BMSR=%04x ANLPAR=%04x)\n",
			       (i + 1) * 250,
			       bmsr,
			       mdio_read(phy, 0x05));
			return true;
		}
	}
	printf("[eth] PHY link DOWN after 8s (BMSR=%04x ANLPAR=%04x)\n",
	       mdio_read(phy, 1),
	       mdio_read(phy, 0x05));
	return false;
}

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

	/* Read the PHY over MDIO (fixed-link is blind to the real wire state), restart
	 * auto-neg, and wait for the link to actually come up before trying DHCP. */
	int phy = phy_find();
	if (phy >= 0) {
		/* DP83825I state: ANAR(4)=our advert, ANLPAR(5)=partner advert (0 => not
		 * hearing the switch = no RX/clock/cable), PHYSTS(0x10)=link/speed,
		 * RCSR(0x17)=RMII mode/clock-select. */
		printf("[eth] PHY regs: ANAR=%04x ANLPAR=%04x PHYSTS=%04x RCSR=%04x\n",
		       mdio_read(phy, 0x04),
		       mdio_read(phy, 0x05),
		       mdio_read(phy, 0x10),
		       mdio_read(phy, 0x17));
	}
	/* Restart auto-neg and wait for the wire link to come up. */
	bool link = (phy >= 0) && phy_wait_link(phy);

	/* End-to-end proof: with the link up, pull a DHCP lease off the bench switch
	 * (dnsmasq). A lease completes only over a genuinely live bidirectional link. */
	bool bound = false;
	if (link) {
		net_dhcpv4_start(iface);
		for (int i = 0; i < 30; i++) { /* up to ~15 s for DISCOVER/OFFER/REQUEST/ACK */
			if (iface->config.dhcpv4.state == NET_DHCPV4_BOUND) {
				bound = true;
				break;
			}
			k_msleep(500);
		}
	}

	printf("[eth] admin_up=%d carrier_ok=%d wire_link=%d rx_bytes=%u dhcp_bound=%d\n",
	       net_if_is_admin_up(iface),
	       net_if_is_carrier_ok(iface),
	       link,
	       iface->stats.bytes.received,
	       bound);

	if (bound) {
		char                ip[NET_IPV4_ADDR_LEN] = { 0 };
		struct net_if_addr *ua                    = &iface->config.ip.ipv4->unicast[0].ipv4;
		net_addr_ntop(AF_INET, &ua->address.in_addr, ip, sizeof(ip));
		printf("[eth] DHCP lease = %s\n", ip);
	}

	printf("[eth] RESULT %s: %s\n",
	       (link && bound) ? "PASS" : "PARTIAL",
	       (link && bound) ? "wire link UP + DHCP lease acquired = Ethernet fully WORKING"
	       : link          ? "PHY wire link UP (auto-neg complete) but no DHCP lease -- check the "
	                         "DHCP server / IP path"
	                       : "PHY link DOWN -- check cable / switch port / PHY reference clock");
	printf("[eth] done\n");
	return 0;
}
