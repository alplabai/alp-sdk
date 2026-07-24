/*
 * Copyright 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clause-22 MDIO controller for the Synopsys DesignWare Ethernet QoS (GMAC)
 * on the Alif Ensemble family. Unlike the DWC XGMAC (a separate IP with its
 * own "CORE_MDIO_SINGLE_COMMAND" block, targeted by upstream's
 * mdio_dwcxgmac.c), the GMAC's MDIO controller lives INSIDE the MAC register
 * window: MAC_MDIO_Address at offset 0x200 and MAC_MDIO_Data at offset 0x204
 * from the MAC base (0x4810_0000 on the Ensemble E8, RTSS-HE/-HP). This DT
 * child node (compatible "snps,designware-mdio") therefore carries no `reg`
 * of its own -- the base address is resolved from the parent `alif,ethernet`
 * MAC node at build time via DT_INST_PARENT().
 *
 * ====== ADR 0017 Tier-1.5 (in-tree glue reusing an UPSTREAM core header) --
 * INTERIM, BUILD-ONLY (compiles + links for the AEN801 M55-HE target; no
 * MDIO-managed PHY exercised on real E8 silicon this batch -- see the
 * aen-ethernet-link mdio-managed twister scenario) ======
 * No upstream Zephyr v4.4 driver targets this register layout, and hal_alif
 * ships no MDIO library, so this file is authored here. It is NOT a
 * reimplementation of vendor register work: the MAC_MDIO_Address/Data field
 * offsets and bitmasks (PA/RDA/GOC/C45E/GB) are CONSUMED from the upstream
 * eth_dwmac core's own private header (eth_dwmac_priv.h, Apache-2.0,
 * co-located on the include path by CONFIG_ETH_DWMAC_ALIF -- see
 * eth_dwmac_alif_ensemble.c and zephyr/CMakeLists.txt), the same header the
 * sibling Alif GMAC glue already reuses. Only the CR (MDC clock-range) field
 * mask is defined locally below, because eth_dwmac_priv.h's own
 * MAC_MDIO_ADDRESS_CR carries an upstream typo (`BIT(11, 8)`, a 2-arg BIT()
 * call that isn't the GENMASK(11, 8) the field needs) -- left unreferenced
 * here rather than "fixed" in a header this driver doesn't own.
 *
 * Control-flow (busy-wait poll-write-poll transfer, mdio_driver_api shape,
 * DEVICE_DT_INST_DEFINE wiring) is original, structured after the shape of
 * upstream's drivers/ethernet/mdio/mdio_dwcxgmac.c (a DIFFERENT IP's MDIO
 * block -- consulted for structure only, its register macros do not apply
 * here and are not used).
 *
 * HARDWARE REFERENCE (facts only, never copied -- the Alif CMSIS DFP is
 * licence-restricted): Alif DFP Device/soc/AE822FA0E5597/include/rtss_he/
 * soc.h confirms ETH_BASE = 0x4810_0000 and that the MDIO registers are
 * MAC-relative offsets 0x200/0x204 (not a separate IP block), matching
 * eth_dwmac_priv.h's MAC_MDIO_ADDRESS/MAC_MDIO_DATA offsets exactly. The
 * DFP's drivers/include/sys_ctrl_eth.h places the ETH clock gate at
 * CLKCTL_PER_MST PERIPH_CLK_ENA bit 12 -- already enabled by the parent
 * eth_dwmac_alif_ensemble.c glue via ALIF_ETHERNET_CLK; this driver does NOT
 * re-gate it (see dwmac_bus_init()). The DWC_ether_qos databook is the
 * reference for the CR (CSR clock-range) semantics used below.
 *
 * Compatible: `snps,designware-mdio`.
 * Driver:  zephyr/drivers/ethernet/mdio/mdio_dwmac_alif.c
 * Binding: zephyr/dts/bindings/mdio/snps,designware-mdio.yaml
 */

#define DT_DRV_COMPAT snps_designware_mdio

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include "eth_dwmac_priv.h" /* MAC_MDIO_ADDRESS{,_PA,_RDA,_GOC_*}/MAC_MDIO_DATA{,_GD} */

LOG_MODULE_REGISTER(mdio_dwmac_alif, CONFIG_MDIO_LOG_LEVEL);

/*
 * CSR (AHB) clock-range select for the MDC divider (MAC_MDIO_Address CR
 * field, bits 11:8) -- defined locally because eth_dwmac_priv.h's own
 * MAC_MDIO_ADDRESS_CR is a mistyped BIT(11, 8), see the file header. Per the
 * DWC_ether_qos databook, CR selects a CSR-clock bracket mapped to a fixed
 * MDC divisor (e.g. CR=4 -> 150-250 MHz CSR -> MDC = CSR/102); IEEE 802.3
 * caps MDC at 2.5 MHz. The Kconfig default below is the bench calibration
 * knob -- see its help text.
 */
#define MDIO_DWMAC_ALIF_ADDR_CR_MASK GENMASK(11, 8)

/* Bounded busy-wait: GB clears within a handful of MDC cycles on a healthy
 * bus; an absent/misbehaving PHY must not wedge the caller forever. */
#define MDIO_DWMAC_ALIF_BUSY_TRIES 200
#define MDIO_DWMAC_ALIF_BUSY_STEP_US 10

struct mdio_dwmac_alif_config {
	mem_addr_t mac_base;
	uint8_t clock_range;
};

static bool mdio_dwmac_alif_wait_idle(mem_addr_t addr_reg)
{
	for (int i = 0; i < MDIO_DWMAC_ALIF_BUSY_TRIES; i++) {
		if (!(sys_read32(addr_reg) & MAC_MDIO_ADDRESS_GOC_GB)) {
			return true;
		}
		k_busy_wait(MDIO_DWMAC_ALIF_BUSY_STEP_US);
	}
	return false;
}

static uint32_t mdio_dwmac_alif_addr_cmd(const struct mdio_dwmac_alif_config *cfg,
					  uint8_t prtad, uint8_t regad, uint32_t goc)
{
	return FIELD_PREP(MAC_MDIO_ADDRESS_PA, prtad) |
	       FIELD_PREP(MAC_MDIO_ADDRESS_RDA, regad) |
	       FIELD_PREP(MDIO_DWMAC_ALIF_ADDR_CR_MASK, cfg->clock_range) | goc |
	       MAC_MDIO_ADDRESS_GOC_GB;
}

static int mdio_dwmac_alif_read(const struct device *dev, uint8_t prtad, uint8_t regad,
				 uint16_t *data)
{
	const struct mdio_dwmac_alif_config *cfg = dev->config;
	mem_addr_t addr_reg = cfg->mac_base + MAC_MDIO_ADDRESS;
	mem_addr_t data_reg = cfg->mac_base + MAC_MDIO_DATA;

	if (!mdio_dwmac_alif_wait_idle(addr_reg)) {
		return -ETIMEDOUT;
	}

	sys_write32(mdio_dwmac_alif_addr_cmd(cfg, prtad, regad,
					      MAC_MDIO_ADDRESS_GOC_0 | MAC_MDIO_ADDRESS_GOC_1),
		    addr_reg);

	if (!mdio_dwmac_alif_wait_idle(addr_reg)) {
		return -ETIMEDOUT;
	}

	*data = (uint16_t)FIELD_GET(MAC_MDIO_DATA_GD, sys_read32(data_reg));
	return 0;
}

static int mdio_dwmac_alif_write(const struct device *dev, uint8_t prtad, uint8_t regad,
				  uint16_t data)
{
	const struct mdio_dwmac_alif_config *cfg = dev->config;
	mem_addr_t addr_reg = cfg->mac_base + MAC_MDIO_ADDRESS;
	mem_addr_t data_reg = cfg->mac_base + MAC_MDIO_DATA;

	if (!mdio_dwmac_alif_wait_idle(addr_reg)) {
		return -ETIMEDOUT;
	}

	sys_write32(FIELD_PREP(MAC_MDIO_DATA_GD, data), data_reg);
	sys_write32(mdio_dwmac_alif_addr_cmd(cfg, prtad, regad, MAC_MDIO_ADDRESS_GOC_0), addr_reg);

	return mdio_dwmac_alif_wait_idle(addr_reg) ? 0 : -ETIMEDOUT;
}

static DEVICE_API(mdio, mdio_dwmac_alif_api) = {
	.read = mdio_dwmac_alif_read,
	.write = mdio_dwmac_alif_write,
};

#define MDIO_DWMAC_ALIF_INIT(n)                                                                  \
	static const struct mdio_dwmac_alif_config mdio_dwmac_alif_cfg_##n = {                   \
		.mac_base = DT_REG_ADDR(DT_INST_PARENT(n)),                                      \
		.clock_range = CONFIG_MDIO_DWMAC_ALIF_CLOCK_RANGE,                                \
	};                                                                                        \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &mdio_dwmac_alif_cfg_##n, POST_KERNEL,        \
			       CONFIG_MDIO_INIT_PRIORITY, &mdio_dwmac_alif_api);

DT_INST_FOREACH_STATUS_OKAY(MDIO_DWMAC_ALIF_INIT)
