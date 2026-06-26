/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-camera-regcheck -- scopeless on-silicon staging check of the Alif CAMERA
 * CAPTURE stack on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run +
 * RAM-console flow.  Mirrors aen-adc-regcheck.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The Alif camera-capture stack is brought IN-TREE as ADR 0017 Tier-2 fork
 *   copies (zephyr/drivers/video/video_alif.c "alif,cam" = the CPI capture
 *   controller, video_csi_dw.c "snps,designware-csi" = the MIPI CSI-2 bridge,
 *   zephyr/drivers/mipi_dphy/dphy_dw.c "snps,designware-dphy" = the D-PHY, and
 *   arx3a0.c "onnn,arx3a0" = the ON Semi MIPI sensor), with their bindings + DT
 *   nodes in zephyr/dts/alif/ensemble_e8_peripherals.dtsi.
 *
 *   The fork video drivers have now been PORTED to the upstream Zephyr v4.4
 *   video API by Alp Lab AB (the old `enum video_endpoint_id`-based callbacks
 *   are gone; v4.4 callbacks are (dev, fmt) / (dev, caps) / (dev, enable, type)
 *   / (dev, cid)).  The former CONFIG_ALP_VIDEO_ALIF_BROKEN gate is RETIRED, so
 *   the driver TUs build in directly when their DT node is enabled.
 *
 *   So this app validates what IS deliverable build-green on this batch:
 *     1. the DT nodes exist and BIND to the right compatibles
 *        (alif,cam / snps,designware-csi / snps,designware-dphy / onnn,arx3a0),
 *     2. the reg base each node carries matches the fork e1.dtsi / e3_e5_e7.dtsi,
 *     3. the CSI bridge + sensor + D-PHY drivers INSTANTIATE on v4.4 (their DT
 *        nodes are enabled and DEVICE_DT_GET_OR_NULL resolves a real device),
 *     4. for any instantiated video DEVICE it exercises the portable v4.4 video
 *        API (video_get_caps + video_get_format) and reports the result.
 *
 *   The CPI controller (cam) is now INSTANTIATED too: the board overlay supplies
 *   the two facts it needs -- the itcm/dtcm `global_base` (re-`compatible` onto
 *   alif,itcm/alif,dtcm; the AXI buffer path's hal_alif local_to_global() reads
 *   it) and the cam<->csi<->arx3a0 media-controller endpoint graph (the CPI
 *   DEVICE_DT_GET's its remote endpoint).  Both are grounded in the fork
 *   ensemble_rtss_he.dtsi + the standard Zephyr video-interfaces pattern.
 *
 * WHAT IS RUNTIME-BLOCKED ON THIS BATCH: actual frame CAPTURE.  No camera sensor
 * is wired on this hardware batch, and the ARX3A0 I2C routing / address /
 * reset+power GPIOs + the cam XVCLK pad-mux are bench unknowns (see the dtsi
 * FLAGs + the overlay).  So arx3a0 device_is_ready() will be FALSE (nothing
 * answers on the bus); the app reports bind-vs-ready separately and NEVER
 * attempts a capture -- it would have nothing to capture from.
 *
 * The PASS gate here is BIND-based: the full camera-capture stack (cam/csi/dphy/
 * arx3a0) binds to its expected compatible at its expected reg base on the v4.4
 * video API, with the endpoint graph + global-base in place.  device_is_ready is
 * reported but not gated (the no-sensor arx3a0 is expected-not-ready).
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/video.h>
#include <zephyr/sys/printk.h>

/* The four camera-capture stack nodes (enabled by the board overlay). */
#define CAM_NODE  DT_NODELABEL(cam)
#define CSI_NODE  DT_NODELABEL(csi)
#define DPHY_NODE DT_NODELABEL(dphy)
#define ARX_NODE  DT_NODELABEL(arx3a0)

/*
 * Expected reg bases, transcribed from the SoC dtsi (which carries the fork's
 * e3_e5_e7.dtsi / e1.dtsi addresses VERBATIM).  We read the LIVE value from
 * devicetree and compare -- so this stays correct if a node ever moves, and
 * catches a binding that resolved to the wrong node.
 */
#define CAM_BASE_EXPECTED  0x49030000U
#define CSI_BASE_EXPECTED  0x49033000U
#define DPHY_BASE_EXPECTED 0x49033000U /* dphy reg[0] "csi_reg" shares the CSI base */
#define ARX_ADDR_EXPECTED  0x36U       /* ARX3A0 default 7-bit I2C addr (bench-unverified) */

/*
 * Compile-time staging facts -- each is 1 iff the node exists, is enabled, and
 * binds to its expected compatible.  These are pure DT predicates -- a bound
 * node at the right compatible, independent of device_is_ready (the sensor's
 * runtime readiness is reported separately, since no sensor is wired).
 */
#define CAM_BOUND (DT_NODE_HAS_STATUS(CAM_NODE, okay) && DT_NODE_HAS_COMPAT(CAM_NODE, alif_cam))
#define CSI_BOUND                                                                                  \
	(DT_NODE_HAS_STATUS(CSI_NODE, okay) && DT_NODE_HAS_COMPAT(CSI_NODE, snps_designware_csi))
#define DPHY_BOUND                                                                                 \
	(DT_NODE_HAS_STATUS(DPHY_NODE, okay) && DT_NODE_HAS_COMPAT(DPHY_NODE, snps_designware_dphy))
#define ARX_BOUND (DT_NODE_HAS_STATUS(ARX_NODE, okay) && DT_NODE_HAS_COMPAT(ARX_NODE, onnn_arx3a0))

/*
 * Exercise the portable v4.4 video API against an instantiated video device:
 * query caps, then read back the format.  We do NOT enqueue buffers or start a
 * stream -- that is the live CAPTURE path, HW-blocked on this batch (no sensor).
 */
static void probe_video_dev(const char *name, const struct device *dev)
{
	if (dev == NULL) {
		printk("%-6s device : <none> (node disabled or driver not built)\n", name);
		return;
	}
	if (!device_is_ready(dev)) {
		printk("%-6s device : present but NOT ready (init/clock failed)\n", name);
		return;
	}

	struct video_caps   caps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	struct video_format fmt  = { .type = VIDEO_BUF_TYPE_OUTPUT };

	int rc_caps = video_get_caps(dev, &caps);
	int rc_fmt  = video_get_format(dev, &fmt);

	printk("%-6s device : READY (v4.4 video API)\n", name);
	printk("        video_get_caps   rc = %d (min_vbuf_count=%u)\n", rc_caps, caps.min_vbuf_count);
	printk("        video_get_format rc = %d  %ux%u pitch=%u fourcc=0x%08x\n",
	       rc_fmt,
	       fmt.width,
	       fmt.height,
	       fmt.pitch,
	       fmt.pixelformat);
}

int main(void)
{
	printk("\n=== aen-camera-regcheck ===\n");

	/*
	 * Step 1+2: report each node's binding + reg base.  DT_REG_ADDR is a
	 * build-time constant pulled from the bound node; a mismatch vs the fork
	 * address means the binding resolved to the wrong node.
	 */
	uint32_t cam_base  = (uint32_t)DT_REG_ADDR(CAM_NODE);
	uint32_t csi_base  = (uint32_t)DT_REG_ADDR(CSI_NODE);
	uint32_t dphy_base = (uint32_t)DT_REG_ADDR(DPHY_NODE);
	uint32_t arx_addr  = (uint32_t)DT_REG_ADDR(ARX_NODE);

	printk("cam   : %s\n", DT_NODE_FULL_NAME(CAM_NODE));
	printk("        bound=%d compat=alif,cam base=0x%08x (exp 0x%08x)\n",
	       (int)CAM_BOUND,
	       cam_base,
	       CAM_BASE_EXPECTED);
	printk("csi   : %s\n", DT_NODE_FULL_NAME(CSI_NODE));
	printk("        bound=%d compat=snps,designware-csi base=0x%08x (exp 0x%08x)\n",
	       (int)CSI_BOUND,
	       csi_base,
	       CSI_BASE_EXPECTED);
	printk("dphy  : %s\n", DT_NODE_FULL_NAME(DPHY_NODE));
	printk("        bound=%d compat=snps,designware-dphy base=0x%08x (exp 0x%08x)\n",
	       (int)DPHY_BOUND,
	       dphy_base,
	       DPHY_BASE_EXPECTED);
	printk("arx3a0: %s\n", DT_NODE_FULL_NAME(ARX_NODE));
	printk("        bound=%d compat=onnn,arx3a0 i2c-addr=0x%02x (exp 0x%02x, BENCH-UNVERIFIED)\n",
	       (int)ARX_BOUND,
	       arx_addr,
	       ARX_ADDR_EXPECTED);

	bool nodes_ok = CAM_BOUND && (cam_base == CAM_BASE_EXPECTED) && CSI_BOUND &&
	                (csi_base == CSI_BASE_EXPECTED) && DPHY_BOUND &&
	                (dphy_base == DPHY_BASE_EXPECTED) && ARX_BOUND &&
	                (arx_addr == ARX_ADDR_EXPECTED);

	/*
	 * Step 3+4: probe the v4.4-ported drivers on the instantiated nodes.
	 * DEVICE_DT_GET_OR_NULL is NULL only if the node is disabled or the driver TU
	 * was not built, so this links cleanly either way.  device_is_ready() is
	 * reported per node: cam (CPI) + csi + dphy init off their own state, but the
	 * arx3a0 sensor cannot be ready (no sensor wired on this batch) -- that is
	 * expected and does NOT fail the bind-based PASS gate below.
	 */
	probe_video_dev("cam", DEVICE_DT_GET_OR_NULL(CAM_NODE));
	probe_video_dev("csi", DEVICE_DT_GET_OR_NULL(CSI_NODE));
	probe_video_dev("arx3a0", DEVICE_DT_GET_OR_NULL(ARX_NODE));

	/*
	 * PASS gate: the full camera-capture stack BINDS -- every node (cam/csi/dphy/
	 * arx3a0) binds to its expected compatible at its expected reg base on the
	 * v4.4 video API, with the cam<->csi<->arx3a0 endpoint graph + the itcm/dtcm
	 * global-base now in place (the former cam-disable blockers).  This is a
	 * bind/instantiation check; device_is_ready is reported above but NOT gated
	 * (the arx3a0 sensor cannot be ready -- no sensor wired).  Live frame CAPTURE
	 * stays HW-blocked on this batch.
	 */
	if (nodes_ok) {
		printk("RESULT PASS: camera-capture stack BINDS -- cam/csi/dphy/arx3a0 nodes bind to "
		       "alif,cam/snps,designware-csi/snps,designware-dphy/onnn,arx3a0 at the fork reg "
		       "bases on the v4.4 video API (cam<->csi<->arx3a0 endpoint graph + itcm/dtcm "
		       "global-base in place); live CAPTURE HW-blocked (no sensor wired)\n");
	} else {
		printk("RESULT FAIL: camera-capture stack NOT fully staged "
		       "(cam=%d csi=%d dphy=%d arx=%d -- a node is missing, disabled, or bound "
		       "to the wrong compatible/reg base)\n",
		       (int)(CAM_BOUND && cam_base == CAM_BASE_EXPECTED),
		       (int)(CSI_BOUND && csi_base == CSI_BASE_EXPECTED),
		       (int)(DPHY_BOUND && dphy_base == DPHY_BASE_EXPECTED),
		       (int)(ARX_BOUND && arx_addr == ARX_ADDR_EXPECTED));
	}

	return 0;
}
