/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-can-regcheck -- scopeless on-silicon staging check of the Alif CAN-FD
 * controller on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run +
 * RAM-console flow.  Mirrors aen-camera-regcheck / aen-adc-regcheck.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The Alif CAN-FD block uses the CAST CAN-CTRL IP (NOT the Bosch M_CAN that
 *   upstream Zephyr's can_mcan.c targets -- the register map differs), so the
 *   Zephyr can_* class driver for it is the fork's can_cast.c ("cast,can"),
 *   brought IN-TREE as an ADR 0017 Tier-2 fork copy (zephyr/drivers/can/
 *   can_cast.c + can_cast.h, Apache-2.0), with its binding under
 *   zephyr/dts/bindings/can/cast,can.yaml and the can0 node in the E8 SoC
 *   overlay zephyr/dts/alif/ensemble_e8_peripherals.dtsi.
 *
 *   The alp-sdk portable CAN surface (<alp/can.h> + src/can_dispatch.c +
 *   src/backends/can/zephyr_drv.c) binds whatever standard Zephyr can_* device
 *   the alp_can0 DT alias points at -- here, the cast,can controller.  So this
 *   app validates what IS deliverable build-green on this batch:
 *     1. the can0 DT node exists and BINDS to "cast,can" at its expected reg
 *        base (0x49036000) with the CANFD0 counter window (0x49037000),
 *     2. the alp_can0 alias resolves to that same node (the portable backend's
 *        contract),
 *     3. the cast,can driver INSTANTIATES (DEVICE_DT_GET_OR_NULL resolves a real
 *        device and device_is_ready() reports its init result),
 *     4. the CAN-FD enable plumbing is wired: can-fd present, can-fd-ctrl-reg =
 *        0x4902F00C (CLKCTL_PER_SLV + CANFD_CTRL offset 0xC), can-fd-bit = 20
 *        (CANFD0_CTRL_FD_ENA on the single-CANFD E8 part).
 *
 * WHAT IS RUNTIME-BLOCKED ON THIS BATCH: actual bus traffic.  No CAN
 * transceiver/bus is wired on this hardware batch, and the CAN_STBY routing is
 * a documented pad/mux conflict (the SoM routes STBY on CAN_STBY_A=P7_3 while
 * RXD/TXD use the _B mux on P0_4/P0_5 -- see the board overlay).  So the app
 * NEVER starts the controller or sends a frame; it reports bind-vs-ready and
 * the DT-encoded FD facts, and gates PASS on the binding.
 *
 * The PASS gate here is BIND-based: the CAN-FD controller binds to "cast,can"
 * at the fork reg base, the alp_can0 alias points at it, and the FD-enable DT
 * facts match the DFP.  device_is_ready is reported but not gated by bus
 * presence.  Live bus communication stays HW-blocked on this batch (no
 * transceiver wired).
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* The CAN-FD controller node + the alp portable alias that must point at it. */
#define CAN_NODE  DT_NODELABEL(can0)
#define CAN_ALIAS DT_ALIAS(alp_can0)

/*
 * Expected facts, transcribed from the SoC dtsi (which carries the fork's
 * e1.dtsi addresses VERBATIM) + the Alif DFP CMSIS headers.  We read the LIVE
 * value from devicetree and compare -- so this stays correct if a node ever
 * moves, and catches a binding that resolved to the wrong node.
 *
 *   reg[0]  0x49036000 -- CANFD0 register block   (soc.h CANFD0_BASE)
 *   reg[1]  0x49037000 -- CANFD0 counter window   (e1.dtsi "can_cnt_reg")
 *   FD ctrl 0x4902F00C -- CLKCTL_PER_SLV(0x4902F000) + CANFD_CTRL(0xC)
 *   FD bit  20         -- CANFD0_CTRL_FD_ENA (sys_ctrl_canfd.h, single-CANFD E8)
 */
#define CAN_BASE_EXPECTED     0x49036000U
#define CAN_CNT_BASE_EXPECTED 0x49037000U
#define CAN_FD_CTRL_EXPECTED  0x4902F00CU
#define CAN_FD_BIT_EXPECTED   20U

/*
 * Compile-time staging facts -- each is 1 iff the node exists, is enabled, and
 * binds to its expected compatible.  Pure DT predicates -- a bound node at the
 * right compatible, independent of device_is_ready (the controller's runtime
 * readiness is reported separately, since no transceiver is wired).
 */
#define CAN_BOUND (DT_NODE_HAS_STATUS(CAN_NODE, okay) && DT_NODE_HAS_COMPAT(CAN_NODE, cast_can))

/* The alp_can0 alias must resolve to the SAME node the cast,can driver binds:
 * that is the contract src/backends/can/zephyr_drv.c relies on
 * (DEVICE_DT_GET(DT_ALIAS(alp_can0))). */
#define ALIAS_OK (DT_NODE_EXISTS(CAN_ALIAS) && (DT_DEP_ORD(CAN_ALIAS) == DT_DEP_ORD(CAN_NODE)))

int main(void)
{
	printk("\n=== aen-can-regcheck ===\n");

	/*
	 * Step 1+2: report the node's binding + reg bases.  DT_REG_ADDR_BY_IDX is a
	 * build-time constant pulled from the bound node; a mismatch vs the fork
	 * address means the binding resolved to the wrong node.
	 */
	uint32_t can_base     = (uint32_t)DT_REG_ADDR_BY_IDX(CAN_NODE, 0);
	uint32_t can_cnt_base = (uint32_t)DT_REG_ADDR_BY_IDX(CAN_NODE, 1);
	uint32_t fd_ctrl      = (uint32_t)DT_PROP(CAN_NODE, can_fd_ctrl_reg);
	uint32_t fd_bit       = (uint32_t)DT_PROP(CAN_NODE, can_fd_bit);
	bool     fd_present   = DT_PROP(CAN_NODE, can_fd);

	printk("can0  : %s\n", DT_NODE_FULL_NAME(CAN_NODE));
	printk("        bound=%d compat=cast,can base=0x%08x (exp 0x%08x)\n",
	       (int)CAN_BOUND,
	       can_base,
	       CAN_BASE_EXPECTED);
	printk("        cnt window base=0x%08x (exp 0x%08x)\n", can_cnt_base, CAN_CNT_BASE_EXPECTED);
	printk("        can-fd=%d can-fd-ctrl-reg=0x%08x (exp 0x%08x) can-fd-bit=%u (exp %u)\n",
	       (int)fd_present,
	       fd_ctrl,
	       CAN_FD_CTRL_EXPECTED,
	       fd_bit,
	       CAN_FD_BIT_EXPECTED);
	printk("alias : alp_can0 -> %s (resolves_to_can0=%d)\n",
	       DT_NODE_FULL_NAME(CAN_ALIAS),
	       (int)ALIAS_OK);

	/*
	 * Step 3: probe the instantiated driver.  DEVICE_DT_GET_OR_NULL is NULL only
	 * if the node is disabled or the driver TU was not built, so this links
	 * cleanly either way.  device_is_ready() reports the cast,can init result;
	 * with no transceiver wired the bus-off / standby exit can still report ready
	 * (init is pin/clock/reset only -- no bus arbitration at init), so readiness
	 * is reported but NOT gated.
	 */
	const struct device *dev = DEVICE_DT_GET_OR_NULL(CAN_NODE);
	if (dev == NULL) {
		printk("can0  device : <none> (node disabled or driver not built)\n");
	} else if (!device_is_ready(dev)) {
		printk("can0  device : present but NOT ready (init/clock/pinctrl failed)\n");
	} else {
		printk("can0  device : READY (cast,can driver instantiated)\n");
	}

	/*
	 * PASS gate: the CAN-FD controller BINDS -- the can0 node binds to "cast,can"
	 * at the fork reg base + counter window, the alp_can0 alias points at it, and
	 * the FD-enable DT facts (ctrl-reg/bit) match the DFP.  This is a
	 * bind/instantiation check; device_is_ready is reported above but NOT gated
	 * (no transceiver wired).  Live bus traffic stays HW-blocked on this batch.
	 */
	bool nodes_ok = CAN_BOUND && (can_base == CAN_BASE_EXPECTED) &&
	                (can_cnt_base == CAN_CNT_BASE_EXPECTED) && fd_present &&
	                (fd_ctrl == CAN_FD_CTRL_EXPECTED) && (fd_bit == CAN_FD_BIT_EXPECTED) &&
	                ALIAS_OK;

	if (nodes_ok) {
		printk("RESULT PASS: CAN-FD controller BINDS -- can0 binds to cast,can at 0x%08x "
		       "(+0x%08x cnt window), alp_can0 alias points at it, CAN-FD enable wired "
		       "(ctrl-reg 0x%08x bit %u); live bus traffic HW-blocked (no transceiver wired)\n",
		       CAN_BASE_EXPECTED,
		       CAN_CNT_BASE_EXPECTED,
		       CAN_FD_CTRL_EXPECTED,
		       CAN_FD_BIT_EXPECTED);
	} else {
		printk("RESULT FAIL: CAN-FD controller NOT fully staged "
		       "(bound=%d base=%d cnt=%d fd=%d ctrl=%d bit=%d alias=%d -- a fact is missing, "
		       "the node is disabled, or it bound to the wrong compatible/reg base)\n",
		       (int)CAN_BOUND,
		       (int)(can_base == CAN_BASE_EXPECTED),
		       (int)(can_cnt_base == CAN_CNT_BASE_EXPECTED),
		       (int)fd_present,
		       (int)(fd_ctrl == CAN_FD_CTRL_EXPECTED),
		       (int)(fd_bit == CAN_FD_BIT_EXPECTED),
		       (int)ALIAS_OK);
	}

	return 0;
}
