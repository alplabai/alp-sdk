/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-power-domains -- round-trip every quiesce-able on-module domain of the
 * E1M-AEN803 (Alif Ensemble E8, M55-HE) through the SoM power-domain runtime
 * (#2784, unit U5), RAM-run on the bench.
 *
 *
 * ==== WHAT THIS PROVES ==============================================
 *
 * Before STOP / STANDBY the SDK holds the on-module consumers that would
 * otherwise burn power through the sleep, and puts them back on the wake.  This
 * app runs that quiesce -> hold -> restore cycle in RUN mode (no STOP involved,
 * so no wake source and no cold boot), which isolates the half that has to work
 * before the STOP backend can rely on it: each chip must answer again after it
 * was held.
 *
 *   domain        quiesce (AUTO)                 comes back with
 *   -----------   ----------------------------   --------------------------------
 *   wifi_ble      E_WIFI_NRST (P15_1) held low   nRESET release = the
 *                                                cc3501e_hard_reset() semantics
 *   eth_phy       E_PHY_PWRDWN (P15_4) low       P15_4 high + E_PHY_RESET pulse
 *   ext_flash     OSPI1_RESETn (P15_7) low       release; driver told the part
 *                                                is back in 1-1-1 SPI
 *   ext_ram       OSPI0_RESETn (P15_6) low       release
 *   temp_sensor   TMP112 CONFIG.SD = 1           CONFIG.SD = 0
 *   rtc           CLKOUT low (RV-3028 stays on)  nothing: it was never powered down
 *   backlight     BACKLIGHT_EN (P5_5) low        P5_5 back to its old level
 *
 * Two details worth knowing before reading the code:
 *
 *   - The CC3501E restore is NEVER cc3501e_reset().  That call drops WIFI_EN
 *     for 50 ms (a supply cold cycle), which on an activated module may never
 *     relaunch the firmware.  The SDK restore releases nRESET only, so the
 *     bench check "PING after restore, no WIFI_EN toggle" is the point of the
 *     CC3501E step.  (The one cc3501e_reset() in this app is the bring-up at
 *     the very start, before anything is quiesced.)
 *
 *   - The PHY's power state is tracked in SOFTWARE.  An unpowered DP83825 does
 *     not read back 0xFFFF over MDIO, it reads back stale data, so "the ID reads"
 *     is only meaningful after the restore, never as proof of the held state.
 *     E_PHY_PWRDWN is also the tri-state pin of the Y3 50 MHz reference
 *     oscillator, so the PHY clock stops and restarts with it.
 *
 *
 * ==== BENCH CONTRACT ================================================
 *
 * Each step prints ONE stable line a bench script greps:
 *     POWER_DOMAINS: <step> <PASS|FAIL>
 * in this order:
 *     baseline_ping baseline_phy_id baseline_jedec baseline_temp
 *     rail_off_refused quiesce held_temp_sd held_ping_down
 *     restore ping_after phy_id_after phy_link_after jedec_after temp_after
 * and a final:
 *     POWER_DOMAINS: SUMMARY pass=<n> fail=<n>
 * Keep these strings byte-for-byte.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <alp/chips/cc3501e.h>
#include <alp/power.h>

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */
#include "som_power.h"      /* quiesce / restore pair (SDK-internal) */
#include "som_power_chips.h"

/* ---- Pads ------------------------------------------------------------------ */

/*
 * The power layer drives levels; it does not own pad muxing.  Select the GPIO
 * function (and with it the output driver) on the pads this app quiesces.  The
 * WIFI_EN / nRESET pads are handled by cc3501e_bridge_bringup().  P15_6 is the
 * HyperRAM reset and is muxed here too so the ext_ram domain works on any SKU
 * that fits the part (this module reports it absent if the SKU does not).
 */
static const pinctrl_soc_pin_t domain_pads[] = {
	PIN_P15_4__LPGPIO, /* E_PHY_PWRDWN */
	PIN_P15_6__LPGPIO, /* OSPI0_RESETn (HyperRAM) */
	PIN_P15_7__LPGPIO, /* OSPI1_RESETn (NOR) */
	PIN_P11_6__GPIO,   /* E_PHY_RESET */
	PIN_P5_5__GPIO,    /* BACKLIGHT_EN */
};

/*
 * Power the PHY BEFORE the Ethernet driver's reference-clock probe looks for the
 * external 50 MHz oscillator, which is downstream of E_PHY_PWRDWN.  Same
 * sequence and priority as aen-ethernet-link (bench-verified): POST_KERNEL 50
 * sits after the GPIO controllers and before eth_dwmac (CONFIG_ETH_INIT_PRIORITY
 * 60).  Later, the power layer drives these same pads for the quiesce.
 */
static int phy_power_init(void)
{
	const struct device *gpio11 = DEVICE_DT_GET(DT_NODELABEL(gpio11));
	const struct device *lpgpio = DEVICE_DT_GET(DT_NODELABEL(lpgpio));

	if (!device_is_ready(gpio11) || !device_is_ready(lpgpio)) {
		return -ENODEV;
	}
	(void)pinctrl_configure_pins(domain_pads, ARRAY_SIZE(domain_pads), 0U);

	(void)gpio_pin_configure(lpgpio, 4, GPIO_OUTPUT_ACTIVE); /* E_PHY_PWRDWN high = on */
	(void)gpio_pin_set(lpgpio, 4, 1);
	k_busy_wait(50000); /* supply + reference clock settle */
	(void)gpio_pin_configure(gpio11, 6, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_set(gpio11, 6, 0); /* assert E_PHY_RESET */
	k_busy_wait(50000);
	(void)gpio_pin_set(gpio11, 6, 1); /* release */
	k_busy_wait(100000);              /* DP83825 post-reset settle */
	return 0;
}
SYS_INIT(phy_power_init, POST_KERNEL, 50);

/* ---- Step bookkeeping --------------------------------------------------------- */

static int g_pass;
static int g_fail;

static void step(const char *name, bool ok)
{
	printf("POWER_DOMAINS: %s %s\n", name, ok ? "PASS" : "FAIL");
	if (ok) {
		g_pass++;
	} else {
		g_fail++;
	}
}

/* ---- CC3501E ------------------------------------------------------------------- */

/* The inter-chip link needs a moment after a reset before it answers; poll by
 * repeating a zero-payload PING rather than stretching one timeout. */
#define PING_RETRIES 25
#define PING_GAP_MS  200

static cc3501e_t g_fw;

static bool ping_with_retry(void)
{
	for (int i = 0; i < PING_RETRIES; i++) {
		if (cc3501e_ping(&g_fw) == ALP_OK) {
			return true;
		}
		k_msleep(PING_GAP_MS);
	}
	return false;
}

/* ---- PHY (raw MDIO through the GMAC) -------------------------------------------- */

/*
 * MAC_MDIO_ADDRESS = GMAC base + 0x200: PA[25:21] PHY address, RDA[20:16]
 * register, CR[11:8] MDC divider, GOC bits 3..2 (read = both, write = bit 2),
 * GB bit 0 = busy; MAC_MDIO_DATA = base + 0x204.  Same access aen-ethernet-link
 * proved on this silicon.
 */
#define GMAC_MDIO_ADDR 0x48100200U
#define GMAC_MDIO_DATA 0x48100204U
#define PHY_ID_DP83825 0x2000a140U

static uint16_t mdio_read(uint8_t phy, uint8_t reg)
{
	while ((sys_read32(GMAC_MDIO_ADDR) & BIT(0)) != 0U) {
	}
	sys_write32(((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(3) | BIT(2) |
	                BIT(0),
	            GMAC_MDIO_ADDR);
	for (int i = 0; i < 100000 && (sys_read32(GMAC_MDIO_ADDR) & BIT(0)) != 0U; i++) {
		k_busy_wait(1);
	}
	return (uint16_t)(sys_read32(GMAC_MDIO_DATA) & 0xFFFFU);
}

static void mdio_write(uint8_t phy, uint8_t reg, uint16_t val)
{
	while ((sys_read32(GMAC_MDIO_ADDR) & BIT(0)) != 0U) {
	}
	sys_write32(val, GMAC_MDIO_DATA);
	sys_write32(((uint32_t)phy << 21) | ((uint32_t)reg << 16) | (0x4U << 8) | BIT(2) | BIT(0),
	            GMAC_MDIO_ADDR);
	for (int i = 0; i < 100000 && (sys_read32(GMAC_MDIO_ADDR) & BIT(0)) != 0U; i++) {
		k_busy_wait(1);
	}
}

/* Scan the 32 MDIO addresses for a responding PHY; returns its address or -1. */
static int phy_find(uint32_t *id_out)
{
	for (uint8_t phy = 0; phy < 32; phy++) {
		uint16_t id1 = mdio_read(phy, 2);
		if (id1 != 0xFFFFU && id1 != 0x0000U) {
			*id_out = ((uint32_t)id1 << 16) | mdio_read(phy, 3);
			return phy;
		}
	}
	return -1;
}

/*
 * A PHY that lost power also lost its strap-independent setting: select the
 * 50 MHz reference clock mode (RCSR bit 7) and restart auto-negotiation, then
 * wait for the wire link.  Same sequence as aen-ethernet-link's phy_wait_link().
 */
static bool phy_link_up(int phy)
{
	uint16_t rcsr = mdio_read((uint8_t)phy, 0x17);

	mdio_write((uint8_t)phy, 0x17, rcsr | BIT(7));
	mdio_write((uint8_t)phy, 0, BIT(12) | BIT(9)); /* BMCR: auto-neg enable + restart */
	for (int i = 0; i < 32; i++) {
		k_msleep(250);
		uint16_t bmsr = mdio_read((uint8_t)phy, 1);
		if ((bmsr & BIT(2)) != 0U && (bmsr & BIT(5)) != 0U) {
			return true;
		}
	}
	return false;
}

/* ---- NOR (OSPI0 SS1) ------------------------------------------------------------- */

#define NOR_JEDEC_IS25WX256_MFR 0x9dU /* ISSI; the fitted part answers 9d 5b 19 */

static bool nor_jedec_ok(uint8_t id[3])
{
	const struct device *ospi = DEVICE_DT_GET(DT_NODELABEL(ospi0));

	if (!device_is_ready(ospi)) {
		return false;
	}
	return flash_read_jedec_id(ospi, id) == 0 && id[0] == NOR_JEDEC_IS25WX256_MFR;
}

/* ---- TMP112 (BRD_I2C) -------------------------------------------------------------- */

#define TMP112_REG_TEMP 0x00U
#define TMP112_REG_CONF 0x01U
#define TMP112_CONF_SD  0x0100U

static const struct i2c_dt_spec tmp112 = I2C_DT_SPEC_GET(DT_NODELABEL(tmp112));

static bool tmp112_read16(uint8_t reg, uint16_t *out)
{
	uint8_t b[2];

	if (!device_is_ready(tmp112.bus) || i2c_burst_read_dt(&tmp112, reg, b, sizeof(b)) != 0) {
		return false;
	}
	*out = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
	return true;
}

/* 12-bit, left-justified, 0.0625 C/LSB; returns milli-degrees C. */
static bool tmp112_temp_milli_c(int32_t *out)
{
	uint16_t raw;

	if (!tmp112_read16(TMP112_REG_TEMP, &raw)) {
		return false;
	}
	*out = ((int32_t)(int16_t)raw >> 4) * 625 / 10;
	return true;
}

/* ---- Domain state table --------------------------------------------------------------- */

static const char *const domain_name[ALP_POWER_DOMAIN_COUNT] = {
	[ALP_POWER_DOMAIN_WIFI_BLE] = "wifi_ble",       [ALP_POWER_DOMAIN_ETH_PHY] = "eth_phy",
	[ALP_POWER_DOMAIN_EXT_FLASH] = "ext_flash",     [ALP_POWER_DOMAIN_EXT_RAM] = "ext_ram",
	[ALP_POWER_DOMAIN_TEMP_SENSOR] = "temp_sensor", [ALP_POWER_DOMAIN_RTC] = "rtc",
	[ALP_POWER_DOMAIN_BACKLIGHT] = "backlight",
};

static const char *state_name(alp_som_pd_state_t s)
{
	switch (s) {
	case ALP_SOM_PD_QUIESCED:
		return "QUIESCED";
	case ALP_SOM_PD_RESTORE_FAILED:
		return "RESTORE_FAILED";
	case ALP_SOM_PD_ACTIVE:
	default:
		return "ACTIVE";
	}
}

static void print_domains(const char *when)
{
	printf("domains %s:\n", when);
	for (int d = 0; d < (int)ALP_POWER_DOMAIN_COUNT; d++) {
		alp_power_domain_info_t info;

		if (alp_power_domain_info((alp_power_domain_t)d, &info) != ALP_OK || !info.present) {
			printf("  %-12s absent on this SKU\n", domain_name[d]);
			continue;
		}
		printf("  %-12s %-14s default_action=0x%02x holds_through_stop=%d dependents=0x%02x\n",
		       domain_name[d],
		       state_name(alp_som_power_state((alp_power_domain_t)d)),
		       (unsigned)info.default_action,
		       (int)info.holds_through_stop,
		       (unsigned)info.dependents);
	}
}

#define HOLD_MS 5000

int main(void)
{
	printf("aen-power-domains: SoM power-domain quiesce/restore round trip (E1M-AEN803)\n");

	/* -- Bring-up (the only WIFI_EN toggle in this app) ----------------------------- */
	alp_status_t br = cc3501e_bridge_bringup(&g_fw);

	printf("cc3501e_bridge_bringup -> %d\n", (int)br);

	struct net_if *iface = net_if_get_default();

	if (iface != NULL) {
		(void)net_if_up(iface); /* enables the GMAC clock the MDIO block needs */
	}
	k_msleep(500);

	/* Route the Wi-Fi/BLE domain through the CC3501E driver context so its own
	 * state (initialised) follows the quiesce.  The other domains use the
	 * devicetree pins / I2C address; the NOR hook binds itself at boot. */
	(void)alp_som_power_bind_cc3501e(&g_fw);

	/* -- Baseline ---------------------------------------------------------------------- */
	uint32_t phy_id = 0U;
	int      phy    = phy_find(&phy_id);
	uint8_t  jedec[3];
	int32_t  temp_mc = 0;

	step("baseline_ping", br == ALP_OK && ping_with_retry());
	step("baseline_phy_id", phy >= 0 && phy_id == PHY_ID_DP83825);
	step("baseline_jedec", nor_jedec_ok(jedec));
	step("baseline_temp", tmp112_temp_milli_c(&temp_mc));
	printf("baseline: phy@%d id=%08x jedec=%02x %02x %02x temp=%d mC\n",
	       phy,
	       (unsigned)phy_id,
	       jedec[0],
	       jedec[1],
	       jedec[2],
	       (int)temp_mc);

	/* -- Policy API ------------------------------------------------------------------- */
	alp_power_t *pw = alp_power_open();

	print_domains("before");
	/* RAIL_OFF is opt-in behind CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF, which this
	 * build leaves off: the request must be refused, and nothing changes. */
	alp_status_t ro = (pw != NULL) ? alp_power_domain_policy_set(pw,
	                                                             ALP_POWER_DOMAIN_WIFI_BLE,
	                                                             ALP_POWER_DOMAIN_POLICY_RAIL_OFF)
	                               : ALP_ERR_NOT_READY;

	step("rail_off_refused", ro == ALP_ERR_NOSUPPORT);

	/* -- Quiesce, hold ----------------------------------------------------------------- */
	alp_status_t q = alp_som_power_quiesce(ALP_POWER_MODE_RUN);

	step("quiesce", q == ALP_OK && alp_som_power_quiesced() != 0U);
	print_domains("held");

	printf("holding %d ms (expect: PING down, TMP112 shutdown bit set, PHY clock stopped)\n",
	       HOLD_MS);
	k_msleep(HOLD_MS);

	uint16_t conf = 0U;

	step("held_temp_sd", tmp112_read16(TMP112_REG_CONF, &conf) && (conf & TMP112_CONF_SD) != 0U);
	/* The context is marked down while nRESET is low, so this fails at once
	 * instead of burning a full bridge timeout. */
	step("held_ping_down", cc3501e_ping(&g_fw) != ALP_OK);

	/* -- Restore ------------------------------------------------------------------------ */
	uint32_t     failed = 0U;
	alp_status_t r      = alp_som_power_restore(&failed);

	step("restore", r == ALP_OK && failed == 0U);
	print_domains("after");
	printf("restore: rc=%d failed_mask=0x%02x\n", (int)r, (unsigned)failed);

	/* -- Does each chip answer again? ------------------------------------------------------ */
	step("ping_after", ping_with_retry());

	phy = phy_find(&phy_id);
	step("phy_id_after", phy >= 0 && phy_id == PHY_ID_DP83825);
	step("phy_link_after", phy >= 0 && phy_link_up(phy));

	step("jedec_after", nor_jedec_ok(jedec));
	step("temp_after", tmp112_temp_milli_c(&temp_mc));
	printf("after: phy@%d id=%08x jedec=%02x %02x %02x temp=%d mC\n",
	       phy,
	       (unsigned)phy_id,
	       jedec[0],
	       jedec[1],
	       jedec[2],
	       (int)temp_mc);

	alp_power_close(pw);
	printf("POWER_DOMAINS: SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
	return 0;
}
