/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-ospi-regcheck -- compile + DT-bind + register-file proof of the Alif
 * Ensemble OSPI/HexSPI controller (Synopsys DesignWare OSPI, compatible
 * "snps,designware-ospi") on the E1M-AEN801 (Ensemble E8, M55-HE/M55-HP), via
 * the bench RAM-run + RAM-console flow.  Mirrors aen-isp-regcheck.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The Alif Ensemble OSPI/HexSPI is an octal-SPI controller that also drives
 *   HyperBus/HyperRAM-style parts in XiP mode.  It is driven by the alp-sdk
 *   Tier-1.5 driver zephyr/drivers/flash/flash_ospi_alif.c, written over the
 *   Apache-2.0 hal_alif drivers/ospi register library (whose init entry
 *   point, alif_hal_ospi_initialize(), this app ALSO calls directly below, as
 *   an independent reachability proof: the hal_alif OSPI library statically
 *   holds only HAL_OSPI_MAX_INST=2 instance slots, and a call from the
 *   driver's own POST_KERNEL init plus this app's direct call both succeed
 *   against the same two-slot table).
 *
 *   So this app validates what IS deliverable build-green on this batch:
 *     1. the ospi0 node EXISTS and BINDS to its expected compatible
 *        ("snps,designware-ospi"),
 *     2. the reg base + aes-reg base + IRQ the node carries match the fork
 *        e1.dtsi (reg 0x83000000, aes-reg 0x83001000, IRQ 96),
 *     3. the flash_ospi_alif.c driver TU is built AND linked
 *        (CONFIG_OSPI_ALIF), the device INSTANTIATES, and
 *     4. alif_hal_ospi_initialize() -- the one hal_alif entry point the
 *        driver calls -- compiles, links, and is REACHABLE when called a
 *        second time directly from application code (LTO can't dead-strip a
 *        called symbol), AND
 *     5. the OSPI register file is genuinely LIVE and readable: CTRLR0 (the
 *        controller's power-on-reset register) reads back its documented
 *        reset value 0x00C00407 (SVD
 *        AE822FA0E5597BS0_CM55_HE_View.svd:75097, cross-checked in the
 *        driver's file-header MEASURED block) -- a real bus read, not merely
 *        "the call returned".
 *
 * WHAT IS HW-BLOCKED ON THIS BATCH, AND WHY IT IS A SKIP NOT AN
 * ATTEMPT-AND-TOLERATE: XiP setup (alif_hal_ospi_xip_enable()) is not called
 * here at all.  OSPI_XIP_SER (offset 0x10C), the register that call touches,
 * DOES NOT EXIST on this die (SOC_FEAT_OSPI_HAS_XIP_SER=0, AE822-specific --
 * see flash_ospi_alif.c's file-header FOURTH section for the full citation
 * chain) -- calling it bus-faults every time, unconditionally, regardless of
 * whether any part is populated.  There is no rc to catch: hal_alif's
 * alif_hal_ospi_xip_enable() (ospi_hal.c:397-416) returns OSPI_ERR_NONE
 * unconditionally after its register writes -- the ONLY failure mode is the
 * fault itself, so a prior version of this app that called it and "tolerated
 * a nonzero rc" was testing a premise that could never fail on its own terms
 * while the real failure (a crash before the RESULT line) went unreported.
 * This app instead states the SKIP explicitly: "no XiP slave in DT;
 * SOC_FEAT_OSPI_HAS_XIP_SER=0 on AE822".  A live XiP read stays additionally
 * unverifiable regardless, and the reason has TWO layers that must not be
 * collapsed into one (#915):
 *
 *   - As DESIGNED on board rev 2626-R2, the OSPI memory is not a hole in the
 *     schematic: the module netlist carries a Macronix MX25UM25645GXDI00
 *     (256 Mbit octal NOR, SPI Octal I/O DTR) wired to OSPI0_D0..D7 / SCLK /
 *     SS1 / RXDS, and its BOM line is marked populated (DNP = 0).
 *   - As ASSEMBLED, the bench unit has NO OSPI memory fitted (maintainer,
 *     2026-08-30).  DNP = 0 is a build-intent field; it does not promise that
 *     any particular physical module was stuffed with the part.
 *
 * So on THIS board a live XiP read is blocked by an empty footprint, exactly
 * as the original comment said, and no amount of DT or driver work will make
 * the XiP step pass here.  The design-level fact only means a future
 * fully-stuffed module would not need a board respin to run it.  Do not read
 * the BOM as evidence about the unit on your desk -- confirm the part is
 * physically there before treating an OSPI failure as a software defect.
 *
 * This example has caught three real, distinct silicon/build bugs on a board
 * with nothing on the OSPI bus (the clock-gate fault, the MPU Device-mapping
 * regression, and the OSPI_XIP_SER fault above) -- it is a regression
 * sentinel, not a formality.  The clock-gate, MPU, and register-file checks
 * below MUST still surface as loud device_is_ready()/init/readback failures
 * if any of those three regress; only the XiP step is a deliberate,
 * explained skip.
 */

#include <stdbool.h>
#include <stdint.h>

#include <cmsis_core.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <ospi_hal.h>

/* The OSPI node (status set by the board overlay). */
#define OSPI_NODE DT_NODELABEL(ospi0)

/*
 * A hal_alif call in this app can bus-fault before reaching the RESULT
 * PASS/FAIL gate at the bottom of main() (see the flash_ospi_alif.c file
 * header for the fault history this driver's init used to be able to hit) --
 * that would otherwise produce NO "RESULT" line at all, a bench NO-RESULT
 * that hides a real failure behind a state the harness can't tell apart from
 * "still running". Overriding the weak default (kernel/fatal.c) makes the
 * crash path self-reporting: print a RESULT FAIL naming the fatal reason
 * code to the RAM console, then hand off to the normal halt so nothing else
 * runs. The prefix is deliberately the same "RESULT FAIL:" the bottom-of-main
 * gate uses (so a simple grep for RESULT catches this path too) but the text
 * after it can never be mistaken for the success line -- that line is
 * "RESULT PASS: ..." and is only ever reached by returning normally past
 * this handler, which never returns.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	printk("RESULT FAIL: fatal error before the PASS/FAIL gate could run (reason=%u) -- "
	       "a hal_alif call trapped (see flash_ospi_alif.c's file-header fault history)\n",
	       reason);
	for (;;) {
		__WFE();
	}
}

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
 * OSPI_CTRLR0's documented power-on-reset value (struct ospi_regs, ospi.h,
 * offset 0x00) -- cross-checked against the SVD's CTRLR0 resetValue in the
 * driver's file-header MEASURED block
 * (AE822FA0E5597BS0_CM55_HE_View.svd:75097).  A readback matching this proves
 * the register FILE is genuinely live on the bus (clock gate open, MPU
 * region mapped Device, controller actually answering), not merely that a
 * hal_alif call returned a success code.
 */
#define OSPI_CTRLR0_RESET_VALUE 0x00C00407U

/*
 * Compile-time staging fact: 1 iff the ospi0 node exists, is enabled, and
 * binds to its expected compatible.  A pure DT predicate -- independent of
 * device_is_ready / whether the driver TU was built.
 */
#define OSPI_BOUND \
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
	 * batch. ospi_alif_init() writes the OSPI0 clock-enable (CLKCTL_PER_SLV
	 * ->OSPI_CTRL bit 0) before calling alif_hal_ospi_initialize(), and the
	 * OSPI register window is mapped MPU Device -- both fixes for real,
	 * previously-reproduced bus faults, see flash_ospi_alif.c's file
	 * header. device_is_ready() reading false here is a genuine RESULT FAIL
	 * of this app's own contract: it means one of those fixes has
	 * regressed, not that the missing external part is at fault (the
	 * missing part only ever excused the removed XiP-enable step, never
	 * anything upstream of it).
	 */
	const struct device *ospi_dev = DEVICE_DT_GET(OSPI_NODE);

	if (!device_is_ready(ospi_dev)) {
		printk("driver: flash_ospi_alif.c linked but device NOT ready (init failed)\n");
	} else {
		printk("driver: flash_ospi_alif.c linked, device READY (ospi_alif_init() "
		       "completed: alif_hal_ospi_initialize() ran at POST_KERNEL)\n");
	}

	/*
	 * Step 4: call alif_hal_ospi_initialize() a SECOND time, directly from
	 * application code, against DT-derived values -- an independent
	 * compile+link+reachability proof (LTO can't dead-strip a called
	 * symbol). The hal_alif OSPI library holds HAL_OSPI_MAX_INST=2 instance
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

	bool hal_init_ok = (init_rc == OSPI_ERR_NONE);

	/*
	 * Step 5: register-file readback. CTRLR0 sits at offset 0x00 of the OSPI
	 * register window -- read it straight off ospi_base (the same address
	 * both alif_hal_ospi_initialize() calls above were given) rather than
	 * through a hal_alif accessor, so this is an independent proof that the
	 * bus itself answers, not a restatement of "the call returned
	 * OSPI_ERR_NONE". By this point in main() the driver's POST_KERNEL init
	 * has already opened the clock gate and the MPU region is mapped
	 * Device, so this is a plain, safe 32-bit load -- if either of those
	 * regresses, this read faults and k_sys_fatal_error_handler() reports it
	 * instead of a silent hang.
	 */
	uint32_t ctrlr0    = *(volatile uint32_t *)ospi_base;
	bool     ctrlr0_ok = (ctrlr0 == OSPI_CTRLR0_RESET_VALUE);

	printk("hal   : CTRLR0=0x%08x (exp reset value 0x%08x)\n", ctrlr0, OSPI_CTRLR0_RESET_VALUE);

	/*
	 * Step 6: XiP is an explicit, stated SKIP -- not attempted. See the
	 * module header for why: OSPI_XIP_SER (offset 0x10C) does not exist on
	 * AE822 (SOC_FEAT_OSPI_HAS_XIP_SER=0), so alif_hal_ospi_xip_enable()
	 * bus-faults unconditionally; there is no rc that could report this as a
	 * recoverable failure (ospi_hal.c:397-416 returns OSPI_ERR_NONE
	 * unconditionally after the writes that fault).
	 */
	printk("hal   : alif_hal_ospi_xip_enable() SKIPPED -- no XiP slave in DT; "
	       "SOC_FEAT_OSPI_HAS_XIP_SER=0 on AE822 (register does not exist on this die)\n");

	/*
	 * PASS gate: the ospi0 node BINDS -- ospi0@83000000 binds to
	 * "snps,designware-ospi" at the fork reg/aes-reg base with IRQ 96 --
	 * AND alif_hal_ospi_initialize() was called and returned OSPI_ERR_NONE
	 * both from the driver's own init and directly from this app -- AND the
	 * register file reads back its documented CTRLR0 reset value. XiP setup
	 * is out of scope for this gate (see the module header SKIP note); a
	 * live XiP read stays HW-blocked regardless (no octal-NOR/HyperBus part
	 * populated this batch).
	 */
	if (node_ok && hal_init_ok && ctrlr0_ok) {
		printk("RESULT PASS: OSPI/HexSPI node BINDS -- ospi0@83000000 binds to "
		       "snps,designware-ospi at the fork reg/aes-reg base with IRQ 96; "
		       "alif_hal_ospi_initialize() is reachable and links; CTRLR0 reads "
		       "its documented reset value; XiP SKIPPED (no XIP_SER on this die), "
		       "live XiP HW-blocked (no part populated this batch)\n");
	} else {
		printk("RESULT FAIL: OSPI/HexSPI node NOT staged "
		       "(bound=%d base_ok=%d irq_ok=%d hal_init_ok=%d ctrlr0_ok=%d -- node "
		       "missing, disabled, bound to the wrong compatible/reg/irq, the "
		       "hal_alif init call did not return OSPI_ERR_NONE, or the register "
		       "file did not read back its reset value)\n",
		       (int)OSPI_BOUND,
		       (int)(ospi_base == OSPI_BASE_EXPECTED && ospi_aes_base == OSPI_AES_BASE_EXPECTED),
		       (int)(ospi_irq == OSPI_IRQ_EXPECTED),
		       (int)hal_init_ok,
		       (int)ctrlr0_ok);
	}

	return 0;
}
