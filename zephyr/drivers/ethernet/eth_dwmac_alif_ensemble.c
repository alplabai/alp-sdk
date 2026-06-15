// SPDX-License-Identifier: Apache-2.0
/*
 * Platform glue for the Synopsys DesignWare MAC (DWC-ETHERNET QoS, v4.x/5.x)
 * as integrated on the Alif Ensemble family (RMII MAC @ 0x4810_0000). Backs
 * the upstream Zephyr DWMAC *core* (drivers/ethernet/eth_dwmac.c) on the
 * Ensemble RTSS-HE / RTSS-HP cores; upstream Zephyr v4.4 ships the core but
 * NOT this Alif platform glue, so we carry it here.
 *
 * Inspiration / structure from the upstream STM32H7X glue
 * (eth_dwmac_stm32h7x.c) and the Alif fork's eth_dwmac_alif_ensemble.c, which
 * are:
 *   Copyright (c) 2021 BayLibre SAS
 *   Copyright (C) 2025 Alif Semiconductor
 *
 * Copyright 2026 Alp Lab AB
 *
 * ============================== STATUS ==============================
 * ADR 0017 Tier-1.5 (in-tree thin glue over an UPSTREAM core) -- INTERIM,
 * BENCH-UNVERIFIED.
 *
 * KEPT in-tree, NOT retired onto the sdk-alif fork: the fork forked the DWMAC
 * *core* (its local_to_global() is patched into the core's address path), so
 * consuming the fork's ETH_DWMAC_ALIF would drag a divergent core onto the
 * tree and violate alp-sdk's one-upstream-base invariant. And upstream
 * Kconfig.dwmac ships no Ensemble path (STM32-gated platform; the other path
 * is MMU-gated and the Cortex-M55 has an MPU, not an MMU; hal_alif ships no
 * GMAC library) -- so retiring this glue would be an UNCONDITIONAL silent
 * Ethernet loss on the upstream-only AEN build. This is the legitimate
 * Tier-1.5 case, not a fork-driver copy. See docs/adr/0017 + task #21.
 *
 * Ported from alifsemi/zephyr_alif's drivers/ethernet/eth_dwmac_alif_ensemble.c
 * (Apache-2.0) and adapted to the glue interface of the DWMAC core shipped in
 * OUR pinned upstream Zephyr v4.4. The MAC/DMA register writes are transcribed
 * verbatim from that source and from eth_dwmac_priv.h; no register value has
 * been invented. It has NOT been run on real silicon. Cannot build under
 * native_sim (Cortex-M55 target), hence bench-unverified in CI.  INTERIM until
 * the two E8 bench gates pass (address-translation placement + RMII ref-clk),
 * then promotes to permanent Tier-1.5.
 *
 * Compatible: `alif,ethernet` (+ `snps,designware-ethernet`).
 * Driver:  zephyr/drivers/ethernet/eth_dwmac_alif_ensemble.c
 * Binding: zephyr/dts/bindings/ethernet/alif,ethernet.yaml
 *
 * --------------------- v4.4-core vs fork-glue notes ---------------------
 * The upstream v4.4 core differs from the fork core the original glue was
 * written against, so this port deviates from the fork glue in two ways that
 * MUST stay in sync with whatever core is on the tree:
 *
 *   1. dwmac_platform_init() returns `int` upstream (the fork declared it
 *      `void`). We return 0 on success / a negative errno on failure, matching
 *      the upstream prototype in eth_dwmac_priv.h and the upstream STM32 glue.
 *
 *   2. The upstream core does PHY management through Zephyr's generic PHY API
 *      (struct dwmac_priv now carries `phy_dev`; the core arms
 *      phy_link_callback_set() and programs MAC speed/duplex from the PHY's
 *      phy_link_state). The fork core had no phy_dev and relied on the PHY's
 *      power-on defaults. So this glue MUST populate p->phy_dev from the DT
 *      `phy-handle`, exactly like eth_dwmac_stm32h7x.c. With a fixed-link child
 *      PHY node (ethernet-phy-fixed-link, no MDIO) this is enough to bring the
 *      link up; see the binding + the MDIO write-up in the porting report.
 *
 *   3. ADDRESS TRANSLATION. The fork core wrapped every descriptor/buffer
 *      pointer in hal_alif's local_to_global() (soc_memory_map.h) to convert an
 *      M55 ITCM/DTCM-local address into the global bus address the Ethernet DMA
 *      master sees. The upstream v4.4 core does NOT call local_to_global() on
 *      the non-MMU path -- it hands the raw CPU virtual address to the DMA
 *      (phys_lo32(&p->tx_descs[idx])). Therefore, with the upstream core, the
 *      descriptor rings AND the net_buf packet pool MUST live in memory whose
 *      CPU-virtual address EQUALS its DMA-global address -- i.e. in shared
 *      SRAM accessed through its global alias, NOT in TCM. We place the
 *      descriptor rings in a non-cached section below; the SoC overlay /
 *      project conf must likewise pin CONFIG_NET_BUF_POOL into such SRAM (the
 *      board/SoC layer owns that placement, not this driver). This is the one
 *      behavioural gap vs. the fork and is called out in the report.
 */

#define DT_DRV_COMPAT alif_ethernet

#define LOG_MODULE_NAME dwmac_plat_alif
#define LOG_LEVEL       CONFIG_ETHERNET_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/irq.h>

#include "eth_dwmac_priv.h"

PINCTRL_DT_INST_DEFINE(0);
static const struct pinctrl_dev_config *eth_pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0);

/*
 * The Alif clock controller (`alif,clockctrl`, upstream
 * drivers/clock_control/clock_control_alif.c) uses `#clock-cells = <1>` with a
 * single `clkid` cell. The node's `clocks = <&clockctrl ALIF_ETHERNET_CLK>`
 * encodes the module/register/bit in that cell. Upstream's bare
 * `snps,designware-ethernet` binding omits `clocks`; the `alif,ethernet`
 * binding requires it (see report).
 */
static const clock_control_subsys_t clkid =
	(clock_control_subsys_t)DT_INST_CLOCKS_CELL(0, clkid);

int dwmac_bus_init(struct dwmac_priv *p)
{
	int ret;

	p->base_addr = DT_INST_REG_ADDR(0);
	p->clock = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(0));

	/* check clock-controller availability */
	if (!device_is_ready(p->clock)) {
		LOG_ERR("clock controller not ready");
		return -ENODEV;
	}

	/* Enable the Ethernet clock from the Alif clock manager */
	ret = clock_control_on(p->clock, clkid);
	if (ret != 0) {
		LOG_ERR("failed to enable ethernet clock (%d)", ret);
		return ret;
	}

	/* Route the RMII pins (REFCLK/MDIO/MDC/RXD/TXD/RST) to the MAC */
	ret = pinctrl_apply_state(eth_pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		LOG_ERR("could not configure ethernet pins (%d)", ret);
		return ret;
	}

	return 0;
}

/*
 * Descriptor rings in non-cached SRAM. The Ensemble M55 cores have a data
 * cache; the DWMAC DMA is not cache-coherent, so the descriptor rings (and the
 * packet buffers, owned by the net_buf pool placement) must be uncached. We
 * follow the upstream STM32 glue convention rather than the fork's
 * `__alif_ns_section` (a fork-/linker-specific section that does not exist on
 * the upstream + hal_alif base). See the address-translation note in the file
 * header: with the upstream core these rings MUST resolve to the same address
 * for the CPU and the DMA, i.e. they must land in shared SRAM via its global
 * alias, which the SoC overlay's nocache region selection is responsible for.
 */
#if defined(CONFIG_NOCACHE_MEMORY)
#define __desc_mem __nocache __aligned(4)
#else
#define __desc_mem __aligned(4)
#endif

static struct dwmac_dma_desc dwmac_tx_descs[NB_TX_DESCS] __desc_mem;
static struct dwmac_dma_desc dwmac_rx_descs[NB_RX_DESCS] __desc_mem;

static const uint8_t dwmac_mac_addr[6] = DT_INST_PROP(0, local_mac_address);

int dwmac_platform_init(struct dwmac_priv *p)
{
	/*
	 * Generic PHY device from the DT `phy-handle`. May be a managed PHY
	 * sitting on an MDIO bus, or a fixed-link PHY (no MDIO). DEVICE_DT_GET_OR_NULL
	 * yields NULL when no phy-handle is present, in which case the core skips
	 * PHY link management entirely and leaves the MAC at its static
	 * MAC_CONF speed/duplex (set below).
	 */
	p->phy_dev = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(0, phy_handle));

	p->tx_descs = dwmac_tx_descs;
	p->rx_descs = dwmac_rx_descs;

	/*
	 * Basic per-platform MAC + DMA configuration (transcribed verbatim from
	 * the Alif fork glue):
	 *   MAC_CONF: PS (port-select = MII/RMII 10/100), FES (100 Mbps),
	 *             DM (full duplex) -- the RMII default; the core overrides
	 *             these from the PHY link callback once a PHY is present.
	 *   DMA_SYSBUS_MODE: AAL (address-aligned beats) | FB (fixed burst).
	 */
	REG_WRITE(MAC_CONF,
		  MAC_CONF_PS |
		  MAC_CONF_FES |
		  MAC_CONF_DM);
	REG_WRITE(DMA_SYSBUS_MODE,
		  DMA_SYSBUS_MODE_AAL |
		  DMA_SYSBUS_MODE_FB);

	/* set up IRQ (the core unmasks the DMA channel IRQs in iface_init) */
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), dwmac_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));

	/* retrieve the MAC address from the DT local-mac-address property */
	memcpy(p->mac_addr, dwmac_mac_addr, sizeof(p->mac_addr));

	return 0;
}

static struct dwmac_priv dwmac_instance;

ETH_NET_DEVICE_DT_INST_DEFINE(0,
			      dwmac_probe,
			      NULL,
			      &dwmac_instance,
			      NULL,
			      CONFIG_ETH_INIT_PRIORITY,
			      &dwmac_api,
			      NET_ETH_MTU);
