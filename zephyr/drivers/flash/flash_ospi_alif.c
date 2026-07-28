/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ============================== STATUS ==============================
 * ADR 0017 Tier-1.5 (in-tree thin driver over the Apache-2.0 hal_alif OSPI
 * register library, modules/hal/alif drivers/ospi/{include,src}/ospi*.{c,h})
 * -- HW-BLOCKED, BUILD-ONLY this batch.  The fork ships no Zephyr OSPI
 * class driver either (only the DT binding), so this thin shell -- authored
 * here against the documented hal_alif API, no offset/bitfield open-coded --
 * is the only path to AEN OSPI.  See docs/adr/0017.
 *
 * The E1M-AEN801 (Ensemble E8) has no octal-NOR/HyperBus part populated this
 * hardware batch, so there is nothing on the bus to silicon-verify: this
 * driver's init reads its `struct ospi_init` straight out of the devicetree
 * node (reg/aes-reg/cs-pin/rx-ds-delay/ddr-drive-edge/bus-speed) and calls
 * alif_hal_ospi_initialize() + alif_hal_ospi_xip_enable() ONCE at POST_KERNEL
 * -- proving the controller-side register program completes and the two
 * hal_alif entry points compile + link.  It does NOT prove a live XiP read
 * (no part on the bus) and does NOT implement flash_driver_api (read/write/
 * erase/SFDP) -- that is a larger silicon-gated follow-up once a part is
 * populated, not this batch.  See examples/aen/aen-ospi-regcheck, which
 * exercises the same two hal_alif calls directly from application code as an
 * independent compile+link+reachability proof.
 *
 * core_clk: the binding's `clock-frequency` property has no documented
 * default and this batch does not set it on the ospi0 node (per the alp-sdk
 * pending-hw-configs policy: the true OSPI core-clock source is a
 * silicon-determined HW fact that must come from the Alif Ensemble E8 TRM,
 * not be invented).  Falls back to the node's `bus-speed` (a real,
 * DFP/fork-sourced value, 100 MHz) as a clearly-marked placeholder -- wrong
 * only in that it may not match the true core-clock divider input; it does
 * not change what gets programmed into the controller from the other fields.
 * ======================================================================
 *
 * ====== OSPI0 clock-enable (CLKCTL_PER_SLV->OSPI_CTRL) -- FIXES A
 * REPRODUCED BUS FAULT, ROOT CAUSE PER DFP, GATING SCOPE UNVERIFIED ======
 * On AE822FA0E5597 (E8) the OSPI register window sits behind a per-instance
 * clock-enable gate that hal_alif's OSPI library never writes -- that library
 * targets parts without the gate.  Without it, alif_hal_ospi_initialize()'s
 * first register touch (ospi_set_tx_threshold() reading OSPI_TXFTLR, hal_alif
 * modules/hal/alif drivers/ospi/src/ospi.c:199 / ospi_hal.c:125) bus-faults:
 *
 *   ***** BUS FAULT ***** Precise data bus error, BFAR 0x83000018
 *
 * (base 0x83000000 + OSPI_TXFTLR offset 0x18 -- exactly BFAR; reproduced
 * identically on two bench runs of examples/aen/aen-ospi-regcheck).
 *
 * Per the DFP's own sequence, this write must happen BEFORE any OSPI
 * register touch:
 *   - AE822FA0E5597/include/soc_features.h:90 -- SOC_FEAT_OSPI_HAS_CLK_ENABLE (1)
 *     (AE722F80F55D5/soc_features.h:86 -- E7 has (0); the E7->E8 silicon delta)
 *   - ospi_xip/source/ospi/ospi_drv.c:305-307 -- vendor calls
 *     enable_ospi_clk(drv_instance) under that flag before any OSPI touch
 *   - drivers/include/sys_ctrl_ospi.h:45-48 -- the write itself:
 *     CLKCTL_PER_SLV->OSPI_CTRL |= (1 << drv_instance)
 *   - CLKCTL_PER_SLV_BASE 0x4902F000 (soc.h:3763), OSPI_CTRL offset 0x3C
 *     (soc.h:2584) -> OSPI0 = bit 0 of 0x4902F03C
 *   - OSPI0_BASE 0x83000000 (rtss_he/soc.h:3778; rtss_hp/soc.h:3784, same base)
 *
 * UNVERIFIED (flag, not fact): the DFP does not say -- and no HW reference
 * manual text was available to check -- whether OSPI_CTRL bit 0 gates the APB
 * register interface specifically vs only the serial/functional clock.  This
 * fix follows the DFP's documented enable-before-touch ORDERING, which is
 * sufficient to explain and (pending bench) fix the observed bus fault, but
 * the exact gating mechanism is NOT independently confirmed.  Pending bench
 * A/B: read 0x83000018 with 0x4902F03C bit 0 clear (expect data abort), set
 * bit 0, read again (expect success).
 *
 * aes-reg (0x83001000, ospi_hal.c:141) follows the same gate per the DFP
 * sequence -- no separate enable is written or expected.
 *
 * INSTANCE DERIVATION: computed from the DT reg address, not hardcoded --
 * OSPI0_BASE (0x83000000) maps to bit 0.  OSPI1_BASE was not sourced for
 * this fix (out of scope); if a second OSPI instance is ever bound at a
 * different base, look up OSPI1_BASE from the DFP before extending this map
 * instead of guessing the stride.
 *
 * Applies core-agnostically: OSPI_CTRL is a system-level CLKCTL_PER_SLV
 * register, not per-core -- the write covers both alp_e1m_aen801_m55_he and
 * ..._m55_hp.  Written once in this driver's POST_KERNEL init, which also
 * covers aen-ospi-regcheck's direct alif_hal_ospi_initialize() call (both
 * paths run after this init has already executed).
 * ===================================================================
 */

#define DT_DRV_COMPAT snps_designware_ospi

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include <ospi_hal.h>

LOG_MODULE_REGISTER(flash_ospi_alif, CONFIG_FLASH_LOG_LEVEL);

struct ospi_alif_config {
	uint32_t *base_regs;
	uint32_t *aes_regs;
	uint32_t  bus_speed;
	uint32_t  core_clk;
	uint32_t  cs_pin;
	uint32_t  rx_ds_delay;
	uint32_t  ddr_drive_edge;
	uint16_t  xip_wait_cycles;
};

struct ospi_alif_data {
	HAL_OSPI_Handle_T handle;
};

/* CLKCTL_PER_SLV->OSPI_CTRL; see the file-header provenance block for the
 * full DFP citation chain (soc.h:3763 base + soc.h:2584 offset). */
#define ALIF_CLKCTL_OSPI_CTRL 0x4902F03Cu

/* OSPI0's own reg base (rtss_he/soc.h:3778, rtss_hp/soc.h:3784 -- same
 * value).  OSPI1_BASE is not sourced for this fix; see the header note. */
#define ALIF_OSPI0_BASE 0x83000000u

/*
 * Enable the CLKCTL_PER_SLV->OSPI_CTRL clock-gate bit for the OSPI instance
 * at base_regs, per the DFP's documented enable_ospi_clk()/sys_ctrl_ospi.h
 * sequence: CLKCTL_PER_SLV->OSPI_CTRL |= (1 << drv_instance).  Derived from
 * the reg address rather than hardcoded, but only OSPI0 is currently
 * resolvable (see the header note on OSPI1_BASE); an unrecognized base skips
 * the write rather than guessing a bit.
 */
static void alif_ospi_clk_enable(uint32_t base_regs)
{
	if (base_regs != ALIF_OSPI0_BASE) {
		LOG_WRN("ospi clk-enable: unrecognized OSPI base 0x%08x (only OSPI0 0x%08x "
			"is DFP-cited for this fix); skipping clock-enable write -- register "
			"access may bus-fault",
			base_regs, ALIF_OSPI0_BASE);
		return;
	}

	sys_set_bit(ALIF_CLKCTL_OSPI_CTRL, 0);
}

static int ospi_alif_init(const struct device *dev)
{
	const struct ospi_alif_config *config = dev->config;
	struct ospi_alif_data         *data   = dev->data;
	struct ospi_init init_cfg = {
		.bus_speed       = config->bus_speed,
		.core_clk        = config->core_clk,
		.cs_pin          = config->cs_pin,
		.rx_ds_delay     = config->rx_ds_delay,
		.ddr_drive_edge  = config->ddr_drive_edge,
		.baud2_delay     = OSPI_BAUD2_DELAY_AUTO,
		.base_regs       = config->base_regs,
		.aes_regs        = config->aes_regs,
		.xip_wait_cycles = config->xip_wait_cycles,
	};
	int32_t rc;

	/* Must happen before the first OSPI register touch inside
	 * alif_hal_ospi_initialize() -- see the file-header provenance block. */
	alif_ospi_clk_enable((uint32_t)config->base_regs);

	rc = alif_hal_ospi_initialize(&data->handle, &init_cfg);
	if (rc != OSPI_ERR_NONE) {
		LOG_ERR("alif_hal_ospi_initialize failed: %d", rc);
		return -EIO;
	}

	/*
	 * BUILD-ONLY reachability proof: exercise the XiP-enable path so the
	 * linker keeps both hal_alif entry points reachable (LTO can't dead-strip
	 * a called symbol).  No octal-NOR/HyperBus part is populated this batch,
	 * so a non-zero rc here is EXPECTED -- the controller register program
	 * completes either way; only a live external part would make the XiP
	 * window actually readable.  Not fatal to driver init.
	 */
	rc = alif_hal_ospi_xip_enable(data->handle);
	if (rc != OSPI_ERR_NONE) {
		LOG_WRN("alif_hal_ospi_xip_enable rc=%d (expected: no OSPI flash/HyperBus "
			"part populated this batch)",
			rc);
	}

	return 0;
}

#define OSPI_ALIF_INIT(inst)                                                                      \
	static struct ospi_alif_data         ospi_alif_data_##inst;                                  \
	static const struct ospi_alif_config ospi_alif_config_##inst = {                             \
		.base_regs       = (uint32_t *)DT_INST_REG_ADDR(inst),                                   \
		.aes_regs        = (uint32_t *)DT_INST_PROP_BY_IDX(inst, aes_reg, 0),                    \
		.bus_speed       = DT_INST_PROP(inst, bus_speed),                                        \
		.core_clk        = DT_INST_PROP_OR(inst, clock_frequency, DT_INST_PROP(inst, bus_speed)),\
		.cs_pin          = DT_INST_PROP(inst, cs_pin),                                           \
		.rx_ds_delay     = DT_INST_PROP(inst, rx_ds_delay),                                       \
		.ddr_drive_edge  = DT_INST_PROP(inst, ddr_drive_edge),                                    \
		.xip_wait_cycles = DT_INST_PROP(inst, xip_wait_cycles),                                   \
	};                                                                                             \
	DEVICE_DT_INST_DEFINE(inst, ospi_alif_init, NULL, &ospi_alif_data_##inst,                    \
			       &ospi_alif_config_##inst, POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(OSPI_ALIF_INIT)
