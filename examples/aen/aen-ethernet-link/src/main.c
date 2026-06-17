/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-ethernet-link -- bring up the Ensemble E8 GMAC (eth_dwmac core + the
 * alp-sdk "alif,ethernet" glue) on the E1M-AEN801 (M55-HE) and report the
 * interface + carrier state.  The MAC/driver/DT already live in the SoC dtsi
 * (ethernet@48100000, fixed-link RMII PHY); this example just enables the node
 * (via the overlay) and exercises the Zephyr net-if path.
 *
 * EVK NOTE: the on-module TI DP83825I PHY needs a power-enable (a CC3501E-side
 * line) and a 50 MHz RMII ref-clock (on-module oscillator into P11_0); the EVK
 * RJ45 is wired to the bench switch.  Until the PHY is powered + the ref-clock
 * is sourced, the MAC sees no carrier -- so this example validates the
 * driver/interface bring-up and reports the link as DOWN.  See the README.
 *
 * RESULT: the node uses a FIXED-LINK PHY (ethernet-phy-fixed-link), which reports
 * carrier unconditionally -- so carrier_ok is NOT proof of a real wire link.  What
 * this example actually proves is that the GMAC driver + Zephyr interface come up
 * on silicon (the MAC software-reset completes -> the RMII ref-clock IS present on
 * P11_0; the driver did not hang).  Real connectivity to the bench switch (TX/RX,
 * ping) still needs the DP83825I PHY powered (a CC3501E enable line) -- so this is
 * reported PARTIAL.  It only escalates beyond PARTIAL once a managed-PHY link or a
 * real ping is wired up.
 */

#include <stdio.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>

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

	/* Admin-up the interface; the eth_dwmac driver starts the MAC DMA.
	 * -EALREADY is fine: the fixed-link PHY may have brought it up at init. */
	int rc = net_if_up(iface);
	printf("[eth] net_if_up -> %d%s\n", rc, (rc == -EALREADY) ? " (already up)" : "");

	bool admin_up = net_if_is_admin_up(iface);
	bool carrier  = net_if_is_carrier_ok(iface);
	bool mac_up   = admin_up && (rc == 0 || rc == -EALREADY);
	printf(
	    "[eth] admin_up=%d carrier_ok=%d (carrier is fixed-link synthetic)\n", admin_up, carrier);

	/* The GMAC/interface initialising on silicon (no MAC-reset hang -> the RMII
	 * ref-clock is present) is the real, verifiable result here.  The fixed-link
	 * PHY makes carrier_ok unconditionally true, so it does NOT prove a wire link
	 * -- hence PARTIAL until the DP83825I is powered and a ping actually flows. */
	printf("[eth] RESULT %s: %s\n",
	       mac_up ? "PARTIAL" : "FAIL",
	       mac_up ? "GMAC + net interface up on silicon (MAC reset OK = RMII refclk "
	                "present); real link pends DP83825I PHY power (CC3501E) + a ping"
	              : "interface did not come up (see net_if_up rc)");
	printf("[eth] done\n");
	return 0;
}
