/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-i3c-regcheck -- scopeless staging check of the Synopsys DesignWare I3C
 * controller on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run +
 * RAM-console flow.  Mirrors aen-can-regcheck.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The Alif I3C block is a Synopsys DesignWare I3C controller, for which
 *   UPSTREAM Zephyr already ships a full driver (drivers/i3c/i3c_dw.c,
 *   "snps,designware-i3c") -- pure ADR 0017 Tier-1 (upstream-native): no
 *   vendored or forked driver code, only the DT node (SoC overlay
 *   zephyr/dts/alif/ensemble_e8_peripherals.dtsi) + this board overlay.
 *
 *   The E1M-AEN801 SoM wires the LP I3C instance (lpi3c0@0x43006000, the
 *   M55-HE local-domain controller) rather than the main i3c0 -- both share
 *   pads P7_6/P7_7 on a different mux selector, and LP I3C is the one the
 *   vendor netlist actually connects (see the board overlay).  So this app
 *   validates what IS deliverable build-green on this batch:
 *     1. the lpi3c0 DT node exists and BINDS to "snps,designware-i3c" at its
 *        expected reg base (0x43006000),
 *     2. the alp_i3c0 alias resolves to that same node (mirrors the alp_can0
 *        alias contract on the CAN-FD sibling -- no portable <alp/i3c.h>
 *        dispatcher exists yet, so this is a plain DT_ALIAS() read, not a
 *        backend binding),
 *     3. the i3c_dw driver INSTANTIATES (DEVICE_DT_GET_OR_NULL resolves a
 *        real device and device_is_ready() reports its init result),
 *     4. the I3C/I2C timing DT facts are wired: i3c-scl-hz = 12.5 MHz,
 *        od-thigh-min-ns = 41 (the DesignWare Open-Drain-high-time floor
 *        the fork sets for this instance).
 *
 * WHAT IS UNTESTED ON THIS BATCH: everything past bind.  This app is
 * COMPILE-PROOF ONLY -- built + linked for the AEN board target, never run on
 * real E1M-AEN801 silicon (I3C is not silicon-verifiable this batch: no I3C
 * target device is wired on the bench carrier).  Unlike aen-dma-regcheck
 * (whose PL330 copy is proven end-to-end on silicon) this app's PASS gate is
 * BIND-based only, same posture as aen-can-regcheck: the controller binds,
 * the alias resolves, the DT facts match the fork transcription.  The
 * DEVICE_CTRL register readback below is an MMIO-reachability probe only --
 * printed for diagnostic value, NOT gated (its expected reset value is a
 * clean-room read of the DesignWare IP spec, not bench-confirmed against
 * this silicon).
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

/* The LP I3C controller node + the alp portable-style alias that must point
 * at it (see the board overlay -- no <alp/i3c.h> dispatcher exists yet). */
#define I3C_NODE  DT_NODELABEL(lpi3c0)
#define I3C_ALIAS DT_ALIAS(alp_i3c0)

/*
 * Expected facts, transcribed from the SoC dtsi (which carries the fork's
 * e4_e6_e8.dtsi address VERBATIM).  We read the LIVE value from devicetree
 * and compare -- so this stays correct if a node ever moves, and catches a
 * binding that resolved to the wrong node.
 *
 *   reg          0x43006000 -- LPI3C0 register block (M55-HE local domain)
 *   i3c-scl-hz   12500000   -- fork e4_e6_e8.dtsi lpi3c0 node
 *   od-thigh-min 41 ns      -- fork e4_e6_e8.dtsi lpi3c0 node
 */
#define I3C_BASE_EXPECTED         0x43006000U
#define I3C_SCL_HZ_EXPECTED       12500000U
#define I3C_OD_THIGH_MIN_EXPECTED 41U

/* DEVICE_CTRL, offset 0x0 in the DesignWare I3C register map (drivers/i3c/
 * i3c_dw.c DEVICE_CTRL) -- probed as a raw MMIO read only (see file header:
 * NOT gated, no expected-value comparison, this silicon is unbenched). */
#define I3C_DEVICE_CTRL_OFFSET 0x0U

/*
 * Compile-time staging facts -- each is 1 iff the node exists, is enabled,
 * and binds to its expected compatible.  Pure DT predicates -- a bound node
 * at the right compatible, independent of device_is_ready (the controller's
 * runtime readiness is reported separately, since this silicon is unbenched).
 */
#define I3C_BOUND \
	(DT_NODE_HAS_STATUS(I3C_NODE, okay) && DT_NODE_HAS_COMPAT(I3C_NODE, snps_designware_i3c))

/* The alp_i3c0 alias must resolve to the SAME node the i3c_dw driver binds. */
#define ALIAS_OK (DT_NODE_EXISTS(I3C_ALIAS) && (DT_DEP_ORD(I3C_ALIAS) == DT_DEP_ORD(I3C_NODE)))

int main(void)
{
	printk("\n=== aen-i3c-regcheck ===\n");

	/*
	 * Step 1+2: report the node's binding + reg base + timing DT facts.
	 * DT_REG_ADDR / DT_PROP are build-time constants pulled from the bound
	 * node; a mismatch vs the fork address means the binding resolved to the
	 * wrong node.
	 */
	uint32_t i3c_base     = (uint32_t)DT_REG_ADDR(I3C_NODE);
	uint32_t i3c_scl_hz   = (uint32_t)DT_PROP(I3C_NODE, i3c_scl_hz);
	uint32_t od_thigh_min = (uint32_t)DT_PROP(I3C_NODE, od_thigh_min_ns);

	printk("lpi3c0: %s\n", DT_NODE_FULL_NAME(I3C_NODE));
	printk("        bound=%d compat=snps,designware-i3c base=0x%08x (exp 0x%08x)\n",
	       (int)I3C_BOUND,
	       i3c_base,
	       I3C_BASE_EXPECTED);
	printk("        i3c-scl-hz=%u (exp %u) od-thigh-min-ns=%u (exp %u)\n",
	       i3c_scl_hz,
	       I3C_SCL_HZ_EXPECTED,
	       od_thigh_min,
	       I3C_OD_THIGH_MIN_EXPECTED);
	printk("alias : alp_i3c0 -> %s (resolves_to_lpi3c0=%d)\n",
	       DT_NODE_FULL_NAME(I3C_ALIAS),
	       (int)ALIAS_OK);

	/*
	 * Step 3: probe the instantiated driver.  DEVICE_DT_GET_OR_NULL is NULL
	 * only if the node is disabled or the driver TU was not built, so this
	 * links cleanly either way.  device_is_ready() reports the i3c_dw init
	 * result.
	 */
	const struct device *dev = DEVICE_DT_GET_OR_NULL(I3C_NODE);

	if (dev == NULL) {
		printk("lpi3c0 device : <none> (node disabled or driver not built)\n");
	} else if (!device_is_ready(dev)) {
		printk("lpi3c0 device : present but NOT ready (init/clock/pinctrl failed)\n");
	} else {
		printk("lpi3c0 device : READY (snps,designware-i3c driver instantiated)\n");
	}

	/*
	 * Diagnostic only (NOT gated -- see file header): a raw MMIO read of
	 * DEVICE_CTRL, proving the register window is reachable.  No expected
	 * value comparison -- the DesignWare reset value for this field is not
	 * bench-confirmed on this silicon.
	 */
	printk("lpi3c0 DEVICE_CTRL (0x%08x) = 0x%08x (reachability probe, not gated)\n",
	       i3c_base + I3C_DEVICE_CTRL_OFFSET,
	       sys_read32(i3c_base + I3C_DEVICE_CTRL_OFFSET));

	/*
	 * PASS gate: the LP I3C controller BINDS -- lpi3c0 binds to
	 * snps,designware-i3c at the fork reg base, the alp_i3c0 alias points at
	 * it, and the DT timing facts (i3c-scl-hz/od-thigh-min-ns) match the
	 * fork transcription.  This is a bind/instantiation check ONLY;
	 * device_is_ready and the DEVICE_CTRL readback are reported above but
	 * NOT gated -- I3C is not silicon-verifiable on this batch (no I3C
	 * target device wired).
	 */
	bool nodes_ok = I3C_BOUND && (i3c_base == I3C_BASE_EXPECTED) &&
	                (i3c_scl_hz == I3C_SCL_HZ_EXPECTED) &&
	                (od_thigh_min == I3C_OD_THIGH_MIN_EXPECTED) && ALIAS_OK;

	if (nodes_ok) {
		printk("RESULT PASS: LP I3C controller BINDS -- lpi3c0 binds to "
		       "snps,designware-i3c at 0x%08x, alp_i3c0 alias points at it, timing DT "
		       "facts wired (i3c-scl-hz=%u od-thigh-min-ns=%u); live bus traffic "
		       "UNTESTED this batch (I3C not silicon-verifiable, no target wired)\n",
		       I3C_BASE_EXPECTED,
		       I3C_SCL_HZ_EXPECTED,
		       I3C_OD_THIGH_MIN_EXPECTED);
	} else {
		printk("RESULT FAIL: LP I3C controller NOT fully staged "
		       "(bound=%d base=%d scl_hz=%d od_thigh=%d alias=%d -- a fact is missing, "
		       "the node is disabled, or it bound to the wrong compatible/reg base)\n",
		       (int)I3C_BOUND,
		       (int)(i3c_base == I3C_BASE_EXPECTED),
		       (int)(i3c_scl_hz == I3C_SCL_HZ_EXPECTED),
		       (int)(od_thigh_min == I3C_OD_THIGH_MIN_EXPECTED),
		       (int)ALIAS_OK);
	}

	return 0;
}
