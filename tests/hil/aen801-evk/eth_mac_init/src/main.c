/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bench HIL test -- Alif Ensemble E8 GMAC (ethernet@48100000) MAC-INIT.
 *
 * GOAL
 * ----
 * Validate that the alp-sdk Alif Ethernet glue (compatible "alif,ethernet",
 * ADR 0017 Tier-1.5, over the upstream eth_dwmac core) brings the GMAC up far
 * enough that it has (a) latched the DT local-mac-address into the MAC station
 * address registers, (b) programmed the DMA bus/mode registers, and (c) enabled
 * the MAC TX/RX path. NO cable/link/traffic is required: with the fixed-link
 * child PHY (ethernet-phy-fixed-link, 100BASE Full) the core's link callback
 * fires synchronously at iface init and flips MAC_CONF speed/duplex + RE/TE.
 *
 * HOW IT RUNS (no driver hooks needed)
 * ------------------------------------
 * The driver registers via ETH_NET_DEVICE_DT_INST_DEFINE at POST_KERNEL, so by
 * the time main() runs the probe + iface init have already executed. main()
 * then:
 *   1. confirms the Zephyr net iface bound to the GMAC device exists and is
 *      "up" at L2 (net_if_up was issued; carrier is on via the fixed PHY),
 *   2. reads the GMAC registers DIRECTLY over MMIO at the absolute SoC address
 *      (so the result does not depend on any driver accessor), and
 *   3. printk()s exactly one RESULT PASS / RESULT FAIL line plus every register
 *      it checked, so the human's SWD readback of 'ram_console_buf' is decisive.
 *
 * GROUND-TRUTH ADDRESSES (no invented values)
 * -------------------------------------------
 * GMAC base .............. 0x48100000   (ethernet@48100000 reg, from
 *                          zephyr/dts/alif/ensemble_e8_peripherals.dtsi)
 * Register offsets ....... from upstream eth_dwmac_priv.h:
 *     MAC_CONF            0x0000   DMA_MODE          0x1000
 *     MAC_VERSION         0x0110   DMA_SYSBUS_MODE   0x1004
 *     MAC_ADDRESS_HIGH(0) 0x0300   MAC_ADDRESS_LOW(0) 0x0304
 * MAC-address byte packing -- from eth_dwmac.c dwmac_set_mac_addr():
 *     HIGH = (addr[5]<<8 | addr[4]) | BIT(31)/AE
 *     LOW  = (addr[3]<<24 | addr[2]<<16 | addr[1]<<8 | addr[0])
 *   For 02:01:56:78:43:21  ->  HIGH = 0x80002143 , LOW = 0x78560102
 * MAC_CONF init bits -- from eth_dwmac.c: dwmac_iface_init() sets
 *     CST(21) | TE(1) | RE(0); the fixed-link callback adds PS(15)|FES(14)|DM(13)
 *     => the FES|PS|DM|TE|RE bits must all read back set.
 * DMA_SYSBUS_MODE -- the Alif glue (eth_dwmac_alif_ensemble.c dwmac_platform_init)
 *     writes AAL(12) | FB(0) => bits 12 and 0 set.
 * DMA_MODE -- after the core's soft-reset spin, SWR(0) must read back CLEAR.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

/* GMAC node + base from the SoC overlay (single alif,ethernet instance). */
#define GMAC_NODE   DT_NODELABEL(ethernet)
#define GMAC_BASE   DT_REG_ADDR(GMAC_NODE)

/* Register offsets -- verbatim from drivers/ethernet/eth_dwmac_priv.h. */
#define R_MAC_CONF          0x0000U
#define R_MAC_VERSION       0x0110U
#define R_MAC_ADDR_HIGH0    0x0300U
#define R_MAC_ADDR_LOW0     0x0304U
#define R_DMA_MODE          0x1000U
#define R_DMA_SYSBUS_MODE   0x1004U

/* MAC_CONF bits we expect set after a full init with the fixed-link PHY. */
#define MAC_CONF_RE         BIT(0)
#define MAC_CONF_TE         BIT(1)
#define MAC_CONF_DM         BIT(13)
#define MAC_CONF_FES        BIT(14)
#define MAC_CONF_PS         BIT(15)

/* MAC_ADDRESS_HIGH address-enable bit. */
#define MAC_ADDR_HIGH_AE    BIT(31)

/* DMA bits. */
#define DMA_SYSBUS_MODE_FB  BIT(0)
#define DMA_SYSBUS_MODE_AAL BIT(12)
#define DMA_MODE_SWR        BIT(0)

/* DT local-mac-address, restated so the expected register words are derivable
 * from this file alone. Keep in sync with the overlay's local-mac-address. */
static const uint8_t expect_mac[6] = {0x02, 0x01, 0x56, 0x78, 0x43, 0x21};

static inline uint32_t rd(uint32_t off)
{
	return sys_read32(GMAC_BASE + off);
}

int main(void)
{
	/* Let POST_KERNEL device init + the net iface bring-up settle. */
	k_sleep(K_MSEC(200));

	printk("\n=== AEN801 E8 GMAC MAC-INIT bench test ===\n");
	printk("GMAC base = 0x%08x\n", (unsigned int)GMAC_BASE);

	/* --- net iface presence + L2 up --- */
	const struct device *eth_dev = DEVICE_DT_GET(GMAC_NODE);
	struct net_if *iface = NULL;
	bool dev_ready = device_is_ready(eth_dev);

	if (dev_ready) {
		iface = net_if_lookup_by_dev(eth_dev);
		if (iface != NULL) {
			/* Bring the L2 interface administratively up (idempotent
			 * if the net stack already did). Traffic is not needed. */
			(void)net_if_up(iface);
		}
	}

	bool iface_ok = (iface != NULL);
	bool admin_up = iface_ok && net_if_is_admin_up(iface);

	printk("eth dev ready = %d, iface found = %d, admin_up = %d\n",
	       dev_ready, iface_ok, admin_up);

	/* --- decisive register readbacks --- */
	uint32_t ver   = rd(R_MAC_VERSION);
	uint32_t mconf = rd(R_MAC_CONF);
	uint32_t hi    = rd(R_MAC_ADDR_HIGH0);
	uint32_t lo    = rd(R_MAC_ADDR_LOW0);
	uint32_t sysb  = rd(R_DMA_SYSBUS_MODE);
	uint32_t dmode = rd(R_DMA_MODE);

	uint32_t exp_hi = ((uint32_t)expect_mac[5] << 8) | expect_mac[4]
			  | MAC_ADDR_HIGH_AE;
	uint32_t exp_lo = ((uint32_t)expect_mac[3] << 24)
			  | ((uint32_t)expect_mac[2] << 16)
			  | ((uint32_t)expect_mac[1] << 8)
			  | expect_mac[0];

	printk("MAC_VERSION      [0x%03x] = 0x%08x (snps ver 0x%02x)\n",
	       R_MAC_VERSION, ver, (unsigned int)(ver & 0xff));
	printk("MAC_ADDR_HIGH(0) [0x%03x] = 0x%08x (expect 0x%08x)\n",
	       R_MAC_ADDR_HIGH0, hi, exp_hi);
	printk("MAC_ADDR_LOW(0)  [0x%03x] = 0x%08x (expect 0x%08x)\n",
	       R_MAC_ADDR_LOW0, lo, exp_lo);
	printk("MAC_CONF         [0x%03x] = 0x%08x (want PS|FES|DM|TE|RE set)\n",
	       R_MAC_CONF, mconf);
	printk("DMA_SYSBUS_MODE  [0x%03x] = 0x%08x (want AAL|FB set)\n",
	       R_DMA_SYSBUS_MODE, sysb);
	printk("DMA_MODE         [0x%03x] = 0x%08x (want SWR clear)\n",
	       R_DMA_MODE, dmode);

	/* --- pass criteria --- */
	bool ver_ok  = ((ver & 0xff) >= 0x40);          /* DWC-ETH >= 4.00 */
	bool addr_ok = (hi == exp_hi) && (lo == exp_lo);
	bool conf_ok = (mconf & (MAC_CONF_PS | MAC_CONF_FES | MAC_CONF_DM |
				 MAC_CONF_TE | MAC_CONF_RE))
		       == (MAC_CONF_PS | MAC_CONF_FES | MAC_CONF_DM |
			   MAC_CONF_TE | MAC_CONF_RE);
	bool sysb_ok = (sysb & (DMA_SYSBUS_MODE_AAL | DMA_SYSBUS_MODE_FB))
		       == (DMA_SYSBUS_MODE_AAL | DMA_SYSBUS_MODE_FB);
	bool dma_ok  = ((dmode & DMA_MODE_SWR) == 0);

	printk("checks: ver=%d addr=%d conf=%d sysbus=%d dma=%d iface=%d up=%d\n",
	       ver_ok, addr_ok, conf_ok, sysb_ok, dma_ok, iface_ok, admin_up);

	if (ver_ok && addr_ok && conf_ok && sysb_ok && dma_ok &&
	    iface_ok && admin_up) {
		printk("RESULT PASS: GMAC init -- MAC addr latched, DMA programmed, "
		       "TX/RX enabled, iface up\n");
	} else {
		printk("RESULT FAIL: GMAC init incomplete -- see register dump above "
		       "(ver=%d addr=%d conf=%d sysbus=%d dma=%d iface=%d up=%d)\n",
		       ver_ok, addr_ok, conf_ok, sysb_ok, dma_ok, iface_ok, admin_up);
	}

	return 0;
}
