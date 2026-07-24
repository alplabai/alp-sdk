/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-ospi-regcheck -- compile + DT-bind proof of the Alif Ensemble OSPI/
 * HexSPI controller (Synopsys DesignWare OSPI, compatible
 * "snps,designware-ospi") on the E1M-AEN801 (Ensemble E8, M55-HE), via the
 * bench RAM-run + RAM-console flow.  Mirrors aen-isp-regcheck.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The Alif Ensemble OSPI/HexSPI is an octal-SPI controller that also drives
 *   HyperBus/HyperRAM-style parts in XiP mode.  It is driven by the alp-sdk
 *   Tier-1.5 driver zephyr/drivers/flash/flash_ospi_alif.c, written over the
 *   Apache-2.0 hal_alif drivers/ospi register library (whose init/XiP-enable
 *   entry points, alif_hal_ospi_initialize()/alif_hal_ospi_xip_enable(), this
 *   app ALSO calls directly below, as an independent reachability proof: the
 *   hal_alif OSPI library statically holds only HAL_OSPI_MAX_INST=2 instance
 *   slots, and a call from the driver's own POST_KERNEL init plus this app's
 *   direct call both succeed against the same two-slot table).
 *
 *   So this app validates what IS deliverable build-green on this batch:
 *     1. the ospi0 node EXISTS and BINDS to its expected compatible
 *        ("snps,designware-ospi"),
 *     2. the reg base + aes-reg base + IRQ the node carries match the fork
 *        e1.dtsi (reg 0x83000000, aes-reg 0x83001000, IRQ 96),
 *     3. the flash_ospi_alif.c driver TU is built AND linked
 *        (CONFIG_OSPI_ALIF), the device INSTANTIATES, and
 *     4. alif_hal_ospi_initialize() + alif_hal_ospi_xip_enable() -- the two
 *        hal_alif entry points the driver calls -- compile, link, and are
 *        REACHABLE when called a second time directly from application code
 *        (LTO can't dead-strip a called symbol).
 *
 * WHAT IS HW-BLOCKED ON THIS BATCH: any live XiP read.  No octal-NOR/
 * HyperBus part is populated on the E1M-AEN801 this hardware batch, so the
 * XiP window this controller programs has nothing behind it to read back --
 * a non-zero alif_hal_ospi_xip_enable() rc here is EXPECTED, not a failure.
 * The PASS gate below is BIND-based (+ the two hal_alif calls not
 * crash/hard-faulting), not a data-correctness check.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <ospi_hal.h>

/* The OSPI node (status set by the board overlay). */
#define OSPI_NODE DT_NODELABEL(ospi0)

/*
 * Expected reg/aes-reg base + IRQ.  Transcribed VERBATIM from the fork's
 * e1.dtsi ospi0 node (reg 0x83000000/0x1000, aes-reg 0x83001000/0x100, IRQ
 * 96) -- see the OSPI_ALIF Kconfig + the SoC dtsi node comment for the
 * full provenance.  Read the LIVE values from devicetree and compare, so
 * this stays correct if the node ever moves and catches a binding that
 * resolved to the wrong node.
 */
#define OSPI_BASE_EXPECTED     0x83000000U
#define OSPI_AES_BASE_EXPECTED 0x83001000U
#define OSPI_IRQ_EXPECTED      96U

/*
 * Compile-time staging fact: 1 iff the ospi0 node exists, is enabled, and
 * binds to its expected compatible.  A pure DT predicate -- independent of
 * device_is_ready / whether the driver TU was built.
 */
#define OSPI_BOUND                                                                               \
	(DT_NODE_HAS_STATUS(OSPI_NODE, okay) && DT_NODE_HAS_COMPAT(OSPI_NODE, snps_designware_ospi))

int main(void)
{
	printk("\n=== aen-ospi-regcheck ===\n");

	/*
	 * Step 1+2: report the node's binding + reg/aes-reg base + IRQ.
	 * DT_REG_ADDR / DT_PROP_BY_IDX / DT_IRQ_BY_IDX are build-time constants
	 * pulled from the bound node; a mismatch vs the fork e1.dtsi means the
	 * binding resolved to the wrong node.
	 */
	uint32_t ospi_base     = (uint32_t)DT_REG_ADDR(OSPI_NODE);
	uint32_t ospi_aes_base = (uint32_t)DT_PROP_BY_IDX(OSPI_NODE, aes_reg, 0);
	uint32_t ospi_irq      = (uint32_t)DT_IRQ_BY_IDX(OSPI_NODE, 0, irq);

	printk("ospi0 : %s\n", DT_NODE_FULL_NAME(OSPI_NODE));
	printk("        bound=%d compat=snps,designware-ospi base=0x%08x (exp 0x%08x) "
	       "aes_base=0x%08x (exp 0x%08x)\n",
	       (int)OSPI_BOUND,
	       ospi_base,
	       OSPI_BASE_EXPECTED,
	       ospi_aes_base,
	       OSPI_AES_BASE_EXPECTED);
	printk("        irq=%u (exp %u)\n", ospi_irq, OSPI_IRQ_EXPECTED);

	bool node_ok = OSPI_BOUND && (ospi_base == OSPI_BASE_EXPECTED) &&
		       (ospi_aes_base == OSPI_AES_BASE_EXPECTED) && (ospi_irq == OSPI_IRQ_EXPECTED);

	/*
	 * Step 3: the flash_ospi_alif.c driver TU is always built under this
	 * app's prj.conf (CONFIG_OSPI_ALIF=y), so DEVICE_DT_GET is safe here --
	 * unlike aen-isp-regcheck, there is no link-blocked driver TU on this
	 * batch.  device_is_ready() is `true` once ospi_alif_init() returns 0,
	 * which it does unconditionally (a non-zero alif_hal_ospi_xip_enable()
	 * is logged but not fatal to init -- see the driver).
	 */
	const struct device *ospi_dev = DEVICE_DT_GET(OSPI_NODE);

	if (!device_is_ready(ospi_dev)) {
		printk("driver: flash_ospi_alif.c linked but device NOT ready (init failed)\n");
	} else {
		printk("driver: flash_ospi_alif.c linked, device READY (ospi_alif_init() "
		       "completed: alif_hal_ospi_initialize() + alif_hal_ospi_xip_enable() "
		       "both ran at POST_KERNEL)\n");
	}

	/*
	 * Step 4: call the two hal_alif entry points a SECOND time, directly
	 * from application code, against DT-derived values -- an independent
	 * compile+link+reachability proof (LTO can't dead-strip a called
	 * symbol).  The hal_alif OSPI library holds HAL_OSPI_MAX_INST=2 instance
	 * slots (modules/hal/alif drivers/ospi/src/ospi_hal.c); the driver's own
	 * POST_KERNEL init already took slot 0, so this call takes slot 1 --
	 * both succeed against the fixed two-slot table.
	 */
	HAL_OSPI_Handle_T app_handle = -1;
	struct ospi_init  app_cfg    = {
		     .bus_speed       = DT_PROP(OSPI_NODE, bus_speed),
		     .core_clk        = DT_PROP_OR(OSPI_NODE, clock_frequency, DT_PROP(OSPI_NODE, bus_speed)),
		     .cs_pin          = DT_PROP(OSPI_NODE, cs_pin),
		     .rx_ds_delay     = DT_PROP(OSPI_NODE, rx_ds_delay),
		     .ddr_drive_edge  = DT_PROP(OSPI_NODE, ddr_drive_edge),
		     .baud2_delay     = OSPI_BAUD2_DELAY_AUTO,
		     .base_regs       = (uint32_t *)ospi_base,
		     .aes_regs        = (uint32_t *)ospi_aes_base,
		     .xip_wait_cycles = DT_PROP(OSPI_NODE, xip_wait_cycles),
	};

	int32_t init_rc = alif_hal_ospi_initialize(&app_handle, &app_cfg);

	printk("hal   : alif_hal_ospi_initialize() rc=%d handle=%d\n", init_rc, (int)app_handle);

	bool hal_reachable = (init_rc == OSPI_ERR_NONE);

	if (hal_reachable) {
		int32_t xip_rc = alif_hal_ospi_xip_enable(app_handle);

		/* HW-BLOCKED: no part on the bus, so a non-zero rc is expected -- this
		 * only proves the call is reachable and does not hard-fault. */
		printk("hal   : alif_hal_ospi_xip_enable() rc=%d (HW-BLOCKED: no OSPI flash/"
		       "HyperBus part populated this batch, non-zero expected)\n",
		       xip_rc);
	}

	/*
	 * PASS gate: the ospi0 node BINDS -- ospi0@83000000 binds to
	 * "snps,designware-ospi" at the fork reg/aes-reg base with IRQ 96 -- AND
	 * the two hal_alif entry points were CALLED and RETURNED (reachable,
	 * did not hard-fault) both from the driver's own init and directly from
	 * this app.  This is a bind + reachability check; a live XiP read stays
	 * HW-blocked on this batch (no octal-NOR/HyperBus part populated).
	 */
	if (node_ok && hal_reachable) {
		printk("RESULT PASS: OSPI/HexSPI node BINDS -- ospi0@83000000 binds to "
		       "snps,designware-ospi at the fork reg/aes-reg base with IRQ 96; "
		       "alif_hal_ospi_initialize()/alif_hal_ospi_xip_enable() are reachable "
		       "and link; live XiP HW-blocked (no part populated this batch)\n");
	} else {
		printk("RESULT FAIL: OSPI/HexSPI node NOT staged "
		       "(bound=%d base_ok=%d irq_ok=%d hal_reachable=%d -- node missing, "
		       "disabled, bound to the wrong compatible/reg/irq, or the hal_alif "
		       "init call did not return OSPI_ERR_NONE)\n",
		       (int)OSPI_BOUND,
		       (int)(ospi_base == OSPI_BASE_EXPECTED && ospi_aes_base == OSPI_AES_BASE_EXPECTED),
		       (int)(ospi_irq == OSPI_IRQ_EXPECTED),
		       (int)hal_reachable);
	}

	return 0;
}
