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
