/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-ethernet-dual -- bring up the two RTL8211FDI Ethernet PHYs
 * on the V2N module, exercise their management surface (MDIO probe,
 * soft-reset, restart-autoneg), and poll link status for both PHYs.
 *
 * The example is chatty by design: every step prints what it did
 * so the source file doubles as a tutorial.  Comment density is
 * ~50 % because the example is documentation.
 *
 * The driver under chips/rtl8211fdi/ is intentionally portable:
 * it takes MDIO read/write callbacks rather than coupling to a
 * specific Zephyr MDIO controller binding.  On a real V2N board
 * this example's `my_mdio_read` / `my_mdio_write` callbacks wrap
 * Zephyr's mdio_read / mdio_write against the Renesas RZ/V2N MAC's
 * MDIO controller.  On native_sim there is no MDIO bus -- the
 * mock callbacks below return zeros so the example still builds
 * and prints the structure of the API calls.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#ifdef CONFIG_MDIO
#include <zephyr/drivers/mdio.h>
#endif

#include "alp/peripheral.h"
#include "alp/chips/rtl8211fdi.h"

/* ----------------------------------------------------------------- */
/* MDIO callback shims                                                */
/*                                                                    */
/* The driver doesn't know about Zephyr's mdio.h API directly -- it   */
/* takes function pointers so the same chip driver works on Zephyr,   */
/* bare-metal, or any other backend.  Below: a Zephyr-flavoured       */
/* shim guarded by CONFIG_MDIO; a native_sim mock that returns zeros. */
/* ----------------------------------------------------------------- */

#ifdef CONFIG_MDIO
static int zephyr_mdio_read_shim(uint8_t phy_addr, uint8_t reg, uint16_t *val, void *user)
{
	const struct device *mdio = (const struct device *)user;
	return mdio_read(mdio, phy_addr, reg, val);
}

static int zephyr_mdio_write_shim(uint8_t phy_addr, uint8_t reg, uint16_t val, void *user)
{
	const struct device *mdio = (const struct device *)user;
	return mdio_write(mdio, phy_addr, reg, val);
}
#else
/* native_sim fallback: pretend the bus is alive but every register
 * reads as 0x0000.  The driver's PHY-OUI check will then fail with
 * ALP_ERR_NOT_READY -- which IS the right behaviour for "no PHY
 * here".  Useful for catching API compile errors without hardware. */
static int mock_mdio_read(uint8_t phy_addr, uint8_t reg, uint16_t *val, void *user)
{
	(void)phy_addr;
	(void)reg;
	(void)user;
	*val = 0u;
	return 0;
}
static int mock_mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val, void *user)
{
	(void)phy_addr;
	(void)reg;
	(void)val;
	(void)user;
	return 0;
}
#endif

/* ----------------------------------------------------------------- */
/* Bring one PHY up + print its link state.                           */
/* ----------------------------------------------------------------- */

static void exercise_phy(const char             *label,
                         rtl8211fdi_t           *ctx,
                         uint8_t                 phy_addr,
                         rtl8211fdi_mdio_read_t  read,
                         rtl8211fdi_mdio_write_t write,
                         void                   *mdio_user)
{
	printf("[%s] init  -> ", label);
	alp_status_t s = rtl8211fdi_init(ctx, phy_addr, read, write, mdio_user);
	printf("status=%d", (int)s);
	if (s == ALP_OK) {
		printf("  PHYID1=0x%04x PHYID2=0x%04x", ctx->phy_id1, ctx->phy_id2);
	}
	printf("\n");
	if (s != ALP_OK) return;

	/* Soft-reset is a clean way to start.  IEEE-spec mandates the
     * chip self-clears BMCR.reset within ~0.5 s. */
	s = rtl8211fdi_soft_reset(ctx, 500000u);
	printf("[%s] reset -> status=%d\n", label, (int)s);

	/* Re-enable + restart autonegotiation.  The link partner has up
     * to ~5 s to complete its half of autoneg; we just kick it off
     * and read link status further down. */
	s = rtl8211fdi_restart_autoneg(ctx);
	printf("[%s] autoneg restart -> status=%d\n", label, (int)s);

	/* Read link state.  On native_sim with the mock callbacks the
     * registers always read zero, so link will report "down" --
     * but the call shape exercises the API.  On real hardware with a
     * 1 Gb link partner, expect up=true and speed=1000M within a
     * few seconds (poll in a loop if you want to time it). */
	bool               up;
	rtl8211fdi_speed_t speed;
	bool               full_duplex;
	s = rtl8211fdi_get_link(ctx, &up, &speed, &full_duplex);
	printf("[%s] link  -> status=%d up=%d speed=%d full_duplex=%d\n",
	       label,
	       (int)s,
	       (int)up,
	       (int)speed,
	       (int)full_duplex);
}

int main(void)
{
	printf("[v2n-ethernet] dual RTL8211FDI exercise\n");

	/* Pick the MDIO callbacks at compile time -- the chip driver
     * doesn't know which one is wired. */
#ifdef CONFIG_MDIO
	/* On a real V2N Zephyr build, replace these device-tree refs
     * with the actual MDIO controller bindings the board overlay
     * exposes (typically `DEVICE_DT_GET(DT_NODELABEL(mdio0))` etc.). */
	const struct device    *mdio0    = NULL; /* TODO: DT lookup on real board */
	const struct device    *mdio1    = NULL;
	rtl8211fdi_mdio_read_t  read_fn  = zephyr_mdio_read_shim;
	rtl8211fdi_mdio_write_t write_fn = zephyr_mdio_write_shim;
#else
	void                   *mdio0    = NULL;
	void                   *mdio1    = NULL;
	rtl8211fdi_mdio_read_t  read_fn  = mock_mdio_read;
	rtl8211fdi_mdio_write_t write_fn = mock_mdio_write;
#endif

	/* Two PHY contexts -- same chip driver, two separate instances.
     * On V2N the two PHYs sit on separate MDIO buses (ET0 + ET1)
     * with distinct controllers, so the `user` pointer changes per
     * PHY.  PHY addresses default to 0 + 1 -- check your schematic's
     * PHYAD strap if your board uses different values. */
	rtl8211fdi_t phy0;
	rtl8211fdi_t phy1;
	exercise_phy("phy0", &phy0, /* phy_addr */ 0u, read_fn, write_fn, mdio0);
	exercise_phy("phy1", &phy1, /* phy_addr */ 1u, read_fn, write_fn, mdio1);

	/* Optional: configure Wake-on-LAN to wake the host on a magic
     * packet matching the locally-administered MAC below. */
	const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
	if (phy0.initialised) {
		alp_status_t s = rtl8211fdi_wol_set_mac(&phy0, mac);
		printf("[phy0] wol_set_mac -> status=%d\n", (int)s);
		s = rtl8211fdi_wol_enable(&phy0, true);
		printf("[phy0] wol_enable  -> status=%d\n", (int)s);
	}

	rtl8211fdi_deinit(&phy0);
	rtl8211fdi_deinit(&phy1);
	printf("[v2n-ethernet] done\n");
	return 0;
}
