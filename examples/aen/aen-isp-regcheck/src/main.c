/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-isp-regcheck -- scopeless on-silicon staging check of the Alif ISP-Pico
 * (Verisilicon ISP Nano "Pico", compatible "vsi,isp-pico") on the E1M-AEN801
 * (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.  Mirrors
 * aen-camera-regcheck.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The Alif Ensemble ISP-Pico is an in-line image-signal-processor sitting
 *   BETWEEN the CSI-2 bridge and memory (a video m2m device: it has an input EP
 *   fed by the camera/CSI controller and an output EP that DMAs the processed
 *   frame to memory).  It is driven by the alp-sdk vendored fork driver
 *   zephyr/drivers/video/isp_pico.c (ADR 0017 Tier-2 INTERIM), whose binding
 *   (zephyr/dts/bindings/video/vsi,isp-pico.yaml) and DT node (isp@49046000 in
 *   the E8 SoC overlay, IRQs 367 "isp" / 368 "mi-isp") are in-tree.
 *
 *   So this app validates what IS deliverable build-green on this batch:
 *     1. the isp node EXISTS and BINDS to its expected compatible
 *        ("vsi,isp-pico"),
 *     2. the reg base + the two IRQs the node carries match the DFP / fork
 *        e4_e6_e8.dtsi (reg 0x49046000, ISP_IRQ_IRQn=367, ISP_MI_IRQ_IRQn=368),
 *     3. the isp_pico.c driver TU is built AND LINKED against hal_alif v2.3.0
 *        (CONFIG_VIDEO_ISP_VSI=y, this app's prj.conf) -- the device
 *        INSTANTIATES and the portable v4.4 video API is exercised
 *        (video_get_caps).
 *
 *   ***********************************************************
 *   ** WHY CONFIG_VIDEO_ISP_VSI IS ON HERE (build-only proof) **
 *   ***********************************************************
 *   isp_pico.c links the hal_alif libisp wrapper (the Vivante ISP BLOB, opt-in
 *   via USE_ALIF_ISP_LIB).  west.yml now pins hal_alif v2.3.0, whose
 *   isp_wrapper/src/isp_api_wrapper.c itself #includes
 *   <zephyr/drivers/video/isp-vsi.h> (previously the only missing piece) and
 *   ships a 3-arg isp_vsi_bottom_half(dev, init_cfg, mi_mis) matching this
 *   driver's call site.  isp-vsi.h itself was never published by hal_alif, so
 *   it -- plus its isp_ctrl_params.h dependency -- is vendored VERBATIM
 *   (Apache-2.0) from alifsemi/zephyr_alif @ v2.3.0 into
 *   zephyr/include/zephyr/drivers/video/ (see the driver's file-header note).
 *   The wrapper exports no isp_vsi_set_param / isp_vsi_get_param, but the
 *   v4.4 port dropped their only callers, so that is not a compile blocker.
 *   Do NOT fabricate any hal_alif API.
 *
 * WHAT IS RUNTIME-BLOCKED ON THIS BATCH: actual ISP processing.  No camera
 * sensor is wired on this hardware batch, and the ISP only does useful work in a
 * camera->csi->isp->memory media-controller graph -- so this app NEVER attempts
 * a stream.  This is a COMPILE + LINK proof only; the live path stays
 * BENCH-BLOCKED until a sensor is wired.
 *
 * The PASS gate here is BIND-based: the isp node binds to "vsi,isp-pico" at reg
 * 0x49046000 with IRQs 367/368.  Whether the driver TU was built+linked is
 * reported separately (expected YES on this batch now that CONFIG_VIDEO_ISP_VSI=y
 * compiles+links cleanly), and does NOT gate the bind-based PASS.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_VIDEO)
#include <zephyr/drivers/video.h>
#endif

/* The ISP node (status set by the board overlay). */
#define ISP_NODE DT_NODELABEL(isp)

/*
 * Expected reg base + IRQs.  The two IRQs are DFP-confirmed (AE822FA0E5597
 * rtss_he/soc.h: ISP_IRQ_IRQn=367, ISP_MI_IRQ_IRQn=368).  The reg base
 * 0x49046000 is NOT in the DFP CMSIS header -- that header defines no ISP_BASE
 * to cross-check against -- so its sole source-of-truth is the fork
 * e4_e6_e8.dtsi (isp@49046000).  We read the LIVE values from devicetree and
 * compare -- so this stays correct if the node ever moves, and catches a
 * binding that resolved to the wrong node.
 */
#define ISP_BASE_EXPECTED 0x49046000U
#define ISP_IRQ0_EXPECTED 367U /* "isp"    */
#define ISP_IRQ1_EXPECTED 368U /* "mi-isp" */

/*
 * Compile-time staging fact: 1 iff the isp node exists, is enabled, and binds to
 * its expected compatible.  A pure DT predicate -- a bound node at the right
 * compatible, independent of device_is_ready / whether the driver TU was built.
 */
#define ISP_BOUND (DT_NODE_HAS_STATUS(ISP_NODE, okay) && DT_NODE_HAS_COMPAT(ISP_NODE, vsi_isp_pico))

/*
 * The driver TU (isp_pico.c) is built only under CONFIG_VIDEO_ISP_VSI, which
 * this app's prj.conf sets to y (compiles + links against hal_alif v2.3.0 --
 * see the header).  ISP_DEV stays gated on the Kconfig (not a bare
 * DEVICE_DT_GET_OR_NULL) so this file keeps building cleanly for anyone who
 * copies it with CONFIG_VIDEO_ISP_VSI=n.
 *
 * IMPORTANT: DEVICE_DT_GET_OR_NULL is NOT NULL-safe here.  It expands to
 * DEVICE_DT_GET when the node is merely status="okay" -- INDEPENDENT of whether
 * any driver actually instantiated a device for it (see device.h:382).  If the
 * isp node were enabled (so it BINDS) but isp_pico.c were NOT built, that
 * expansion would emit a dangling reference to a __device_dts_ord_* symbol that
 * no TU defines -> a LINK error.  So we gate the device fetch on
 * CONFIG_VIDEO_ISP_VSI (the Kconfig that controls whether the driver TU
 * exists): when the driver is not built, ISP_DEV is a plain NULL and nothing
 * references the (non-existent) device object.
 */
#if defined(CONFIG_VIDEO_ISP_VSI)
#define ISP_DEV DEVICE_DT_GET_OR_NULL(ISP_NODE)
#else
#define ISP_DEV NULL
#endif

int main(void)
{
	printk("\n=== aen-isp-regcheck ===\n");

	/*
	 * Step 1+2: report the node's binding + reg base + IRQs.  DT_REG_ADDR /
	 * DT_IRQ_BY_IDX are build-time constants pulled from the bound node; a
	 * mismatch vs the DFP address/IRQ means the binding resolved to the wrong
	 * node.
	 */
	uint32_t isp_base = (uint32_t)DT_REG_ADDR(ISP_NODE);
	uint32_t isp_irq0 = (uint32_t)DT_IRQ_BY_IDX(ISP_NODE, 0, irq);
	uint32_t isp_irq1 = (uint32_t)DT_IRQ_BY_IDX(ISP_NODE, 1, irq);

	printk("isp   : %s\n", DT_NODE_FULL_NAME(ISP_NODE));
	printk("        bound=%d compat=vsi,isp-pico base=0x%08x (exp 0x%08x)\n",
	       (int)ISP_BOUND,
	       isp_base,
	       ISP_BASE_EXPECTED);
	printk("        irq[0]=%u (exp %u, \"isp\")  irq[1]=%u (exp %u, \"mi-isp\")\n",
	       isp_irq0,
	       ISP_IRQ0_EXPECTED,
	       isp_irq1,
	       ISP_IRQ1_EXPECTED);

	bool node_ok = ISP_BOUND && (isp_base == ISP_BASE_EXPECTED) &&
	               (isp_irq0 == ISP_IRQ0_EXPECTED) && (isp_irq1 == ISP_IRQ1_EXPECTED);

	/*
	 * Step 3: report whether the isp_pico.c driver TU was built+linked, and if so
	 * exercise the portable v4.4 video API on the instantiated device.  On this
	 * batch the driver IS built (CONFIG_VIDEO_ISP_VSI=y, compiles+links against
	 * hal_alif v2.3.0).  device_is_ready() can still legitimately come back
	 * false here: video_isp_init() requires either a camera controller or the
	 * TPG enabled, and this build-only overlay wires neither (no sensor on this
	 * batch) -- that is expected and does NOT fail the bind-based PASS gate
	 * below.
	 */
	const struct device *isp_dev = ISP_DEV;

	if (isp_dev == NULL) {
		printk("driver: isp_pico.c NOT built/linked (CONFIG_VIDEO_ISP_VSI=n)\n");
	} else if (!device_is_ready(isp_dev)) {
		printk("driver: isp_pico.c linked but device NOT ready (init needs a camera\n");
		printk("        controller or TPG; neither is wired -- BENCH-BLOCKED, expected)\n");
#if defined(CONFIG_VIDEO)
	} else {
		struct video_caps caps    = { .type = VIDEO_BUF_TYPE_OUTPUT };
		int               rc_caps = video_get_caps(isp_dev, &caps);

		printk("driver: isp_pico.c linked, device READY (v4.4 video API)\n");
		printk(
		    "        video_get_caps rc = %d (min_vbuf_count=%u)\n", rc_caps, caps.min_vbuf_count);
#endif
	}

	/*
	 * PASS gate: the ISP node BINDS -- isp@49046000 binds to "vsi,isp-pico" at
	 * the DFP reg base with the two DFP IRQs.  This is a bind/staging check;
	 * the driver TU compile+link status is reported above and does NOT gate
	 * this PASS.  Live ISP processing stays BENCH-BLOCKED on this batch (no
	 * camera sensor wired) -- see the file header.
	 */
	if (node_ok) {
		printk("RESULT PASS: ISP-Pico node BINDS -- isp@49046000 binds to vsi,isp-pico at "
		       "the DFP reg base 0x49046000 with IRQs 367/368; driver TU compiles+links "
		       "against hal_alif v2.3.0 (reported above); live processing BENCH-BLOCKED "
		       "(no sensor wired)\n");
	} else {
		printk("RESULT FAIL: ISP-Pico node NOT staged "
		       "(bound=%d base_ok=%d irq0_ok=%d irq1_ok=%d -- node missing, disabled, or "
		       "bound to the wrong compatible/reg/irq)\n",
		       (int)ISP_BOUND,
		       (int)(isp_base == ISP_BASE_EXPECTED),
		       (int)(isp_irq0 == ISP_IRQ0_EXPECTED),
		       (int)(isp_irq1 == ISP_IRQ1_EXPECTED));
	}

	return 0;
}
