/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dsi-regcheck -- scopeless on-silicon staging check of the Alif C2-MIPI-DSI
 * DISPLAY stack on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run +
 * RAM-console flow.  Mirrors aen-camera-regcheck (the CSI/RX twin of this path).
 *
 * THE DISPLAY CHAIN (C2-MIPI-DSI):
 *
 *   cdc200@49031000  (tes,cdc-2.1)          -- the DPI/RGB pixel pump (CDC200)
 *       |  DPI
 *   mipi_dsi@49032000 (snps,designware-dsi) -- the MIPI-DSI host (DSI-TX bridge)
 *       |  phy-if <&dphy 1>
 *   dphy@49033000    (snps,designware-dphy) -- the shared DesignWare MIPI D-PHY
 *
 * The shared D-PHY is the SAME block the camera path uses for CSI-2 RX; for the
 * display path it runs in the TX role.  That TX role is ALREADY implemented in
 * the in-tree driver zephyr/drivers/mipi_dphy/dphy_dw.c (dphy_dw_master_setup(),
 * which programs the PHY via the node's "dsi_reg" 0x49032000 view) -- so the
 * D-PHY does NOT need a new driver for DSI.  What is missing is a display-class
 * driver for cdc200 (tes,cdc-2.1) and a mipi_dsi-class host driver for
 * mipi_dsi (snps,designware-dsi); neither upstream Zephyr v4.4 nor hal_alif
 * ships either.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   This is a BIND/instantiation check, not a pixels-on-glass check.  No display
 *   driver is authored yet, so there is no device to render through; this app
 *   proves the DT plumbing is correct and ready for a driver:
 *     1. the cdc200 + mipi_dsi + dphy nodes exist and BIND to their expected
 *        compatibles (tes,cdc-2.1 / snps,designware-dsi / snps,designware-dphy),
 *     2. the reg base each node carries matches the Alif CMSIS DFP / fork e1.dtsi
 *        addresses (CDC 0x49031000, DSI 0x49032000, D-PHY 0x49033000),
 *     3. the DSI host's cdc-if phandle resolves to the cdc200 node and its
 *        phy-if phandle resolves to the dphy node (the chain is wired),
 *     4. the dphy device (already a real in-tree driver) reports device_is_ready.
 *
 * WHAT IS RUNTIME-BLOCKED ON THIS BATCH: actual display output.  (a) No display-
 * class driver exists yet for cdc200/mipi_dsi (the port is assessed in the
 * deliverable -- the DFP register maps make it tractable).  (b) The AEN801 panel
 * part is TBD: the fork default is focuslcds,mw405 (the Alif AppKit panel), NOT
 * confirmed for the AEN801 SoM -- so no panel child node is asserted here, and
 * the panel reset/backlight GPIOs are bench-unknown (omitted, not invented).
 *
 * The PASS gate is BIND-based: the cdc200 -> mipi_dsi -> dphy chain binds to its
 * expected compatibles at its expected reg bases, with the DSI host's cdc-if and
 * phy-if phandles resolving to the right nodes.  device_is_ready is reported for
 * the dphy (the one node that already has a real driver); the cdc200/mipi_dsi
 * "ready" state is not gated (no driver instantiates them yet).
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* The three display-chain nodes (enabled by the board overlay). */
#define CDC_NODE  DT_NODELABEL(cdc200)
#define DSI_NODE  DT_NODELABEL(mipi_dsi)
#define DPHY_NODE DT_NODELABEL(dphy)

/*
 * Expected reg bases, transcribed from the Alif CMSIS DFP SoC header
 * (Device/soc/AE822FA0E5597/include/rtss_he/soc.h: CDC_BASE 0x49031000,
 * DSI_BASE 0x49032000) and the fork e1.dtsi (d-phy@49033000).  We read the LIVE
 * value from devicetree and compare -- so a binding that resolved to the wrong
 * node is caught.
 */
#define CDC_BASE_EXPECTED  0x49031000U
#define DSI_BASE_EXPECTED  0x49032000U
#define DPHY_BASE_EXPECTED 0x49033000U /* dphy reg[0] "csi_reg" is the shared base */

/*
 * Compile-time staging facts -- each is 1 iff the node exists, is enabled, and
 * binds to its expected compatible.  Pure DT predicates, independent of
 * device_is_ready (no display driver instantiates cdc200/mipi_dsi yet).
 */
#define CDC_BOUND (DT_NODE_HAS_STATUS(CDC_NODE, okay) && DT_NODE_HAS_COMPAT(CDC_NODE, tes_cdc_2_1))
#define DSI_BOUND                                                                                  \
	(DT_NODE_HAS_STATUS(DSI_NODE, okay) && DT_NODE_HAS_COMPAT(DSI_NODE, snps_designware_dsi))
#define DPHY_BOUND                                                                                 \
	(DT_NODE_HAS_STATUS(DPHY_NODE, okay) && DT_NODE_HAS_COMPAT(DPHY_NODE, snps_designware_dphy))

/*
 * Chain-wiring facts: the DSI host's cdc-if phandle must resolve to the cdc200
 * node, and its phy-if phandle must resolve to the dphy node.  DT_SAME_NODE on
 * the resolved phandle is a build-time constant.
 */
#define DSI_CDC_IF_OK DT_SAME_NODE(DT_PHANDLE(DSI_NODE, cdc_if), CDC_NODE)
#define DSI_PHY_IF_OK DT_SAME_NODE(DT_PHANDLE_BY_IDX(DSI_NODE, phy_if, 0), DPHY_NODE)

static void report_ready(const char *name, const struct device *dev)
{
	if (dev == NULL) {
		printk("%-7s device : <none> (node disabled or no driver built)\n", name);
		return;
	}
	if (!device_is_ready(dev)) {
		printk("%-7s device : present but NOT ready\n", name);
		return;
	}
	printk("%-7s device : READY\n", name);
}

int main(void)
{
	printk("\n=== aen-dsi-regcheck ===\n");

	/*
	 * Step 1+2: report each node's binding + reg base.  DT_REG_ADDR is a
	 * build-time constant from the bound node; a mismatch vs the DFP/fork
	 * address means the binding resolved to the wrong node.
	 */
	uint32_t cdc_base  = (uint32_t)DT_REG_ADDR(CDC_NODE);
	uint32_t dsi_base  = (uint32_t)DT_REG_ADDR(DSI_NODE);
	uint32_t dphy_base = (uint32_t)DT_REG_ADDR(DPHY_NODE);

	printk("cdc200 : %s\n", DT_NODE_FULL_NAME(CDC_NODE));
	printk("         bound=%d compat=tes,cdc-2.1 base=0x%08x (exp 0x%08x)\n",
	       (int)CDC_BOUND,
	       cdc_base,
	       CDC_BASE_EXPECTED);
	printk("mipidsi: %s\n", DT_NODE_FULL_NAME(DSI_NODE));
	printk("         bound=%d compat=snps,designware-dsi base=0x%08x (exp 0x%08x)\n",
	       (int)DSI_BOUND,
	       dsi_base,
	       DSI_BASE_EXPECTED);
	printk("dphy   : %s\n", DT_NODE_FULL_NAME(DPHY_NODE));
	printk("         bound=%d compat=snps,designware-dphy base=0x%08x (exp 0x%08x)\n",
	       (int)DPHY_BOUND,
	       dphy_base,
	       DPHY_BASE_EXPECTED);

	/* Step 3: the chain is wired (DSI -> CDC source, DSI -> D-PHY TX). */
	printk("chain  : dsi.cdc-if->cdc200=%d  dsi.phy-if->dphy=%d\n",
	       (int)DSI_CDC_IF_OK,
	       (int)DSI_PHY_IF_OK);

	/*
	 * Step 4: the dphy already has a real in-tree driver (its DSI-TX role is
	 * dphy_dw_master_setup), so it has a real device -- probe device_is_ready.
	 *
	 * cdc200/mipi_dsi have NO display/mipi_dsi-class driver yet: no driver TU
	 * builds a struct device for them, so DEVICE_DT_GET_OR_NULL would emit an
	 * extern reference to a device object that is never defined (a link error).
	 * We therefore do NOT call DEVICE_DT_GET on those two nodes -- their bind is
	 * a pure DT fact (above), and "ready" is meaningless until a driver exists.
	 */
	printk("cdc200  device : <no driver yet> (display-class port pending -- DT-bind only)\n");
	printk("mipidsi device : <no driver yet> (mipi_dsi-class port pending -- DT-bind only)\n");
	report_ready("dphy", DEVICE_DT_GET_OR_NULL(DPHY_NODE));

	bool nodes_ok = CDC_BOUND && (cdc_base == CDC_BASE_EXPECTED) && DSI_BOUND &&
	                (dsi_base == DSI_BASE_EXPECTED) && DPHY_BOUND &&
	                (dphy_base == DPHY_BASE_EXPECTED) && DSI_CDC_IF_OK && DSI_PHY_IF_OK;

	if (nodes_ok) {
		printk("RESULT PASS: C2-MIPI-DSI display chain BINDS -- cdc200/mipi_dsi/dphy bind to "
		       "tes,cdc-2.1/snps,designware-dsi/snps,designware-dphy at the DFP reg bases "
		       "(CDC 0x49031000, DSI 0x49032000, D-PHY 0x49033000) with the dsi->cdc-if and "
		       "dsi->phy-if phandles wired; display-class drivers for cdc200/mipi_dsi not yet "
		       "authored, panel part TBD -- live display HW-blocked on this batch\n");
	} else {
		printk("RESULT FAIL: C2-MIPI-DSI display chain NOT fully staged "
		       "(cdc=%d dsi=%d dphy=%d cdc-if=%d phy-if=%d -- a node is missing, disabled, "
		       "bound to the wrong compatible/reg base, or the chain phandles are unwired)\n",
		       (int)(CDC_BOUND && cdc_base == CDC_BASE_EXPECTED),
		       (int)(DSI_BOUND && dsi_base == DSI_BASE_EXPECTED),
		       (int)(DPHY_BOUND && dphy_base == DPHY_BASE_EXPECTED),
		       (int)DSI_CDC_IF_OK,
		       (int)DSI_PHY_IF_OK);
	}

	return 0;
}
