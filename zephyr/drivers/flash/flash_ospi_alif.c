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
 * -- exercising the controller-side register program (expected to complete
 * now that the OSPI0 clock-enable below has landed and its gating mechanism
 * is bench-measured, see that block) and proving the two hal_alif entry
 * points compile + link.
 * It does NOT prove a live XiP read (no part on the bus) and does NOT
 * implement flash_driver_api (read/write/erase/SFDP) -- that is a larger
 * silicon-gated follow-up once a part is populated, not this batch.  See
 * examples/aen/aen-ospi-regcheck, which
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
 * REPRODUCED BUS FAULT, ROOT CAUSE PER DFP, GATING SCOPE MEASURED ======
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
 * MEASURED 2026-07-28 (was UNVERIFIED -- the pending bench A/B below has now
 * run): OSPI_CTRL bit 0 gates the APB register interface itself, not just the
 * serial/functional clock. One held J-Link session on E1M-AEN801 / AE822
 * M55-HE, verbatim:
 *
 *   J-Link>mem32 0x4902F03C,1
 *   4902F03C = 00000000
 *   J-Link>mem32 0x83000018,1
 *   Could not read memory.
 *   J-Link>w4 0x4902F03C, 0x00000001
 *   Writing 00000001 -> 4902F03C
 *   J-Link>mem32 0x83000018,1
 *   83000018 = 00000000
 *   J-Link>mem32 0x83000000,1
 *   83000000 = 00C00407
 *
 * With CLKCTL_PER_SLV->OSPI_CTRL bit 0 clear, the debugger's own AHB-AP
 * access to the OSPI register window (0x83000018, OSPI_TXFTLR -- the same
 * offset that produced BFAR 0x83000018 above) aborts ("Could not read
 * memory"); with the bit set, the identical read succeeds and a second
 * register (0x83000000) also reads back cleanly. That confirms the APB-gating
 * reading: the bit gates register-interface access, not merely the serial
 * clock. This is narrower than "the fix works" (already known from the bus
 * fault going away) -- it is now also known WHY.
 *
 * CORROBORATION: Alif_CMSIS/Source/Driver_OSPI.c:441 calls enable_ospi_clk()
 * in ARM_OSPI_PowerControl() immediately before ospi_set_tx_threshold(OSPI->regs,
 * ...) -- the identical first register touch whose OSPI_TXFTLR read produced
 * BFAR 0x83000018 here.  Tighter than the ospi_xip/ospi_drv.c:305-307 citation
 * above (same file, same call site, same register); kept alongside it.
 *
 * COUNTER-EVIDENCE CITATION (kept for context, does NOT carry over now that
 * OSPI_CTRL is independently measured above): zephyr/drivers/gpio/gpio_clk_alif.c:23-36
 * records that on this SAME SoC and this SAME CLKCTL_PER_SLV block,
 * GPIO_CTRL bit 16 (CKEN) was bench-measured NOT required for pad drive.
 * That bit was inert for pad drive; the OSPI0 A/B above demonstrates
 * OSPI_CTRL bit 0 is not inert -- it gates register access (AHB-AP read
 * aborts with it clear, succeeds with it set). Different bit, different
 * block function, different measured outcome -- the GPIO result was never
 * proof for OSPI and is retained only as the prior data point the OSPI
 * question was weighed against before its own bench A/B ran.
 *
 * aes-reg (0x83001000, ospi_hal.c:141) follows the same gate per the DFP
 * sequence -- no separate enable is written or expected.
 *
 * INSTANCE DERIVATION: computed from the DT reg address, not hardcoded --
 * both instances are DFP-sourced and mapped to their enable_ospi_clk()
 * OSPI_INSTANCE bit (sys_ctrl_ospi.h:32-35, bit = drv_instance):
 *   - OSPI0_BASE 0x83000000 (rtss_he/soc.h:3778, rtss_hp/soc.h:3784) -> bit 0
 *   - OSPI1_BASE 0x83002000 (rtss_he/soc.h:3780, rtss_hp/soc.h:3786) -> bit 1
 * No ospi1 DT node exists in-tree today (only ospi0 is declared in the fork
 * e1.dtsi), so the second entry is currently unreachable -- kept anyway:
 * an unrecognized-base skip previously reproduced the identical bus fault it
 * was meant to avoid (see alif_ospi_clk_enable()), and unreachable-today is
 * exactly how that class of bug survives.
 *
 * Applies core-agnostically: OSPI_CTRL is a system-level CLKCTL_PER_SLV
 * register, not per-core -- the write covers both alp_e1m_aen801_m55_he and
 * ..._m55_hp, which also means concurrent init on both cores races on this
 * same register (sys_set_bit() is a plain non-atomic read-modify-write,
 * sys_bitops.h:24-29).  The DFP's own enable_ospi_clk() has the identical
 * hazard, so this mirrors the vendor's sequence rather than introducing a
 * new race -- not fixed here; deviating from that sequence is out of scope.
 * Written once in this driver's POST_KERNEL init, which also covers
 * aen-ospi-regcheck's direct alif_hal_ospi_initialize() call (both paths run
 * after this init has already executed).
 * ===================================================================
 */

#define DT_DRV_COMPAT snps_designware_ospi

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

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

/* OSPI reg bases, mapped to their CLKCTL_PER_SLV->OSPI_CTRL enable bit per
 * the DFP's enable_ospi_clk()/OSPI_INSTANCE enum (sys_ctrl_ospi.h:32-35,
 * bit = drv_instance); see the header note for the full citation chain and
 * why OSPI1 is currently unreachable (no ospi1 DT node in-tree). */
#define ALIF_OSPI0_BASE 0x83000000u /* rtss_he/soc.h:3778, rtss_hp/soc.h:3784 -- bit 0 */
#define ALIF_OSPI1_BASE 0x83002000u /* rtss_he/soc.h:3780, rtss_hp/soc.h:3786 -- bit 1 */

/*
 * Enable the CLKCTL_PER_SLV->OSPI_CTRL clock-gate bit for the OSPI instance
 * at base_regs, per the DFP's documented enable_ospi_clk()/sys_ctrl_ospi.h
 * sequence: CLKCTL_PER_SLV->OSPI_CTRL |= (1 << drv_instance).  Derived from
 * the reg address rather than hardcoded.  Returns -ENOTSUP on an
 * unrecognized base instead of silently skipping the write: skipping it
 * does not soften anything -- alif_hal_ospi_initialize() then walks
 * straight into the same first register touch that bus-faults without the
 * clock-enable, only now with the one explanatory LOG_WRN likely lost to
 * deferred-logging before the fault.  The caller turns this into a
 * device_is_ready()-visible init failure instead.
 */
static int alif_ospi_clk_enable(uint32_t base_regs)
{
	unsigned int bit;

	if (base_regs == ALIF_OSPI0_BASE) {
		bit = 0;
	} else if (base_regs == ALIF_OSPI1_BASE) {
		bit = 1;
	} else {
		LOG_WRN("ospi clk-enable: unrecognized OSPI base 0x%08x (only OSPI0 0x%08x / "
			"OSPI1 0x%08x are DFP-cited for this fix); refusing to touch OSPI "
			"registers -- they would bus-fault",
			base_regs, ALIF_OSPI0_BASE, ALIF_OSPI1_BASE);
		return -ENOTSUP;
	}

	sys_set_bit(ALIF_CLKCTL_OSPI_CTRL, bit);
	return 0;
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
	int     clk_rc;

	/* Must happen before the first OSPI register touch inside
	 * alif_hal_ospi_initialize() -- see the file-header provenance block. */
	clk_rc = alif_ospi_clk_enable((uint32_t)config->base_regs);
	if (clk_rc != 0) {
		return clk_rc;
	}

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
