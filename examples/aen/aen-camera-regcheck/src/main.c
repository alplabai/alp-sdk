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
 *   The fork video drivers, however, target the OLDER Zephyr video API whose
 *   callbacks take an `enum video_endpoint_id ep` argument -- a type upstream
 *   Zephyr v4.4 REMOVED in the video-API rework (v4.4 callbacks are (dev, fmt) /
 *   (dev, caps) / (dev, enable, type) / (dev, cid)).  Porting all three drivers
 *   to v4.4 is a driver rewrite, not a mechanical fix, so the driver TUs are
 *   GATED OFF (CONFIG_ALP_VIDEO_ALIF_BROKEN=n) -- carried in-tree but not yet
 *   compiled.  This is deferred to the fork-repoint (task #21).
 *
 *   So this app validates the STAGING that IS deliverable build-green:
 *     1. the DT nodes exist and BIND to the right compatibles
 *        (alif,cam / snps,designware-csi / snps,designware-dphy / onnn,arx3a0),
 *     2. the reg base each node carries matches the fork e1.dtsi / e3_e5_e7.dtsi,
 *     3. whether a bound video DEVICE was instantiated (it is NOT, while the
 *        driver TU stays gated off -- DEVICE_DT_GET_OR_NULL returns NULL),
 *     4. IF (and only if) a device IS present (after the v4.4 port flips the
 *        gate on), it exercises the portable video API (video_get_caps +
 *        video_set_format) and reports the negotiated format.
 *
 * WHAT IS RUNTIME-BLOCKED ON THIS BATCH: actual frame CAPTURE.  No camera
 * sensor is wired on this hardware batch, and the ARX3A0 I2C routing / address /
 * reset+power GPIOs are bench unknowns (see the dtsi FLAGs).  Even once the
 * driver is ported, a live frame needs the sensor populated.  This app NEVER
 * attempts a capture -- it would have nothing to capture from.
 *
 * The PASS gate here is: the camera-capture stack is STAGED -- every node binds
 * to its expected compatible at its expected reg base.  Whether a bound device
 * exists is REPORTED (it tells you if the driver-build gate is on), not part of
 * the gate, because the driver is intentionally gated off pending the v4.4 port.
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
 * binds to its expected compatible.  These are pure DT predicates: they hold
 * regardless of whether the driver TU was compiled, which is exactly the
 * "bindings bind" property we want to assert here.
 */
#define CAM_BOUND (DT_NODE_HAS_STATUS(CAM_NODE, okay) && DT_NODE_HAS_COMPAT(CAM_NODE, alif_cam))
#define CSI_BOUND                                                                                  \
	(DT_NODE_HAS_STATUS(CSI_NODE, okay) && DT_NODE_HAS_COMPAT(CSI_NODE, snps_designware_csi))
#define DPHY_BOUND                                                                                 \
	(DT_NODE_HAS_STATUS(DPHY_NODE, okay) && DT_NODE_HAS_COMPAT(DPHY_NODE, snps_designware_dphy))
#define ARX_BOUND (DT_NODE_HAS_STATUS(ARX_NODE, okay) && DT_NODE_HAS_COMPAT(ARX_NODE, onnn_arx3a0))

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
	 * Step 3+4: is a bound video DEVICE present, and does the portable video API
	 * work against it?  This is guarded by CONFIG_VIDEO_ALIF_CAM -- the symbol is
	 * only y once the driver TU is built (which today requires flipping
	 * CONFIG_ALP_VIDEO_ALIF_BROKEN=y AFTER the fork driver is ported to the v4.4
	 * video API).  While the driver is gated OFF, referencing the cam device at
	 * all (even via DEVICE_DT_GET_OR_NULL) would be an undefined link symbol --
	 * an enabled node with no driver instance has no __device ordinal -- so we
	 * compile out the device path entirely and just report the gated-off state.
	 */
#if defined(CONFIG_VIDEO_ALIF_CAM)
	const struct device *cam = DEVICE_DT_GET(CAM_NODE);

	if (!device_is_ready(cam)) {
		printk("cam device : present but NOT ready (init/clock failed)\n");
	} else {
		/*
		 * Exercise the portable v4.4 video API: query caps, then read back the
		 * format.  We do NOT enqueue buffers or start a stream -- that is the
		 * live CAPTURE path, which is HW-blocked (no sensor wired on this batch).
		 */
		struct video_caps   caps = { 0 };
		struct video_format fmt  = { 0 };

		int rc_caps = video_get_caps(cam, &caps);
		int rc_fmt  = video_get_format(cam, &fmt);

		printk("cam device : READY\n");
		printk("video_get_caps   rc = %d (min_vbuf_count=%u)\n", rc_caps, caps.min_vbuf_count);
		printk("video_get_format rc = %d  %ux%u pitch=%u fourcc=0x%08x\n",
		       rc_fmt,
		       fmt.width,
		       fmt.height,
		       fmt.pitch,
		       fmt.pixelformat);
	}
#else
	printk("cam device : <none> (driver TU gated off -- CONFIG_ALP_VIDEO_ALIF_BROKEN=n,\n");
	printk("             fork driver not yet ported to the v4.4 video API; task #21)\n");
#endif

	/*
	 * PASS gate: the camera-capture stack is STAGED -- every node binds to its
	 * expected compatible at its expected reg base.  The "cam device" presence
	 * line above is REPORTED, not gated, because the driver is intentionally
	 * gated off pending the v4.4 video-API port (task #21).  Live frame CAPTURE
	 * is HW-blocked on this batch (no camera sensor wired).
	 */
	if (nodes_ok) {
		printk("RESULT PASS: camera-capture stack STAGED -- cam/csi/dphy/arx3a0 nodes bind "
		       "to alif,cam/snps,designware-csi/snps,designware-dphy/onnn,arx3a0 at the "
		       "fork reg bases; driver TU pending v4.4 video-API port (task #21); live "
		       "CAPTURE HW-blocked (no sensor wired)\n");
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
