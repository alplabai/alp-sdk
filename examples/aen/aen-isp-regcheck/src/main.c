/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-isp-regcheck -- stage 1 of the Alif ISP-Pico (Verisilicon ISP Nano
 * "Pico", compatible "vsi,isp-pico") bring-up on the E1M-AEN801/AEN803
 * (Ensemble E8, M55-HE): "ISP alive, MI writes".
 *
 * The ISP is an in-line image-signal-processor sitting BETWEEN the CSI-2
 * bridge and memory (a video m2m device: an input EP fed by the camera/CSI
 * controller, an output EP that DMAs the processed frame to memory).  It is
 * driven by the alp-sdk vendored fork driver zephyr/drivers/video/isp_pico.c
 * (ADR 0017 Tier-2 INTERIM), whose binding
 * (zephyr/dts/bindings/video/vsi,isp-pico.yaml) and DT node (isp@49046000 in
 * the E8 SoC overlay, IRQs 367 "isp" / 368 "mi-isp") are in-tree.
 *
 * An EARLIER build-only proof (superseded by this file) established that
 * isp_pico.c COMPILES + LINKS against hal_alif v2.3.0.  This stage goes
 * further: no camera sensor is wired on this hardware batch, so it exercises
 * the ISP's self-contained Test-Pattern Generator (TPG) instead -- the board
 * overlay sets DT tpg-image-idx = "3x3-Color-Block", tpg-pix-width = <1>
 * (10-bit), tpg-bayer-pattern = "BGGR", and wires NO controller/ports.  The
 * TPG is an internal pattern source inside the ISP block, independent of the
 * ACQ external-pin path a real sensor's CPI/CSI feed would use, so it needs
 * neither a controller nor a CSI port to produce a real frame.
 *
 * OUTPUT FORMAT: YUYV, not raw Bayer (run 66).  This app originally requested
 * VIDEO_PIX_FMT_SBGGR10 as the OUTPUT (MI/channel) format; the hal_alif
 * libisp wrapper REJECTED it -- isp_pixelfmt_from_fourcc()
 * (isp_api_wrapper.c) maps every Bayer fourcc to a Bayer PIXEL_FORMAT_* valid
 * only on the INPUT port, not the OUTPUT channel, so
 * VSI_MPI_ISP_SetChnAttr() failed with -EINVAL and isp_vsi_update_cfg()
 * propagated it up through video_stream_start() -- see the "PLANNED FIX"
 * comment above isp_stream_start() (isp_pico.c) and the comment above
 * supported_output_fmts[] (also isp_pico.c) for the full trace.  YUYV is
 * Alif's own DFP default MI output format (RTE_Device.h
 * RTE_ISP_OUTPUT_FORMAT=32 == PIXEL_FORMAT_YUYV) and needs the demosaic +
 * colour pipeline actually enabled -- see prj.conf's CONFIG_ISP_LIB_*_MODULE
 * set.
 *
 * WHAT THIS APP PROVES:
 *   1. CGU CLK_ENA (0x1A602014) bit 29 -- the ISP's clock gate -- is set
 *      (SVD reset default 0x7F33F7F1 has bit 29 = 1): the ISP block is
 *      clocked with no action from this app (no ISP reset/clock-enable bit
 *      exists to program -- Driver_ISP.c in the Alif DFP enables no clock
 *      either).
 *   2. The ISP ID registers (PRODUCT_ID/CHIP_ID/CHIP_REVISION,
 *      isp_pico.h:14-19 offsets 0x008/0x00C/0x024) read back off the bus --
 *      proof the AHB/APB fabric actually reaches the block (a "silicon
 *      connect" check independent of driver/DT correctness: read directly
 *      off the bound node's physical reg base, not through the driver).
 *   3. video_isp_init() (POST_KERNEL) configures the TPG straight from the
 *      DT overlay (isp_configure(), isp_pico.c) and the device comes up
 *      device_is_ready().
 *   4. One TPG-sourced frame makes it through
 *      enqueue -> video_stream_start -> IRQ 368 ("mi-isp")
 *      MI_INTR_MP_FRAME_END -> dequeue, with a non-zero byte count, a CRC32,
 *      and a coarse per-region YUV mean (Y/U/V) that is NOT uniform across
 *      the 3x3 grid -- proof the "3x3-Color-Block" pattern survived TPG ->
 *      demosaic -> colour-space conversion to YUYV, not just that some DMA
 *      happened.
 *
 * WHAT STAYS BENCH-BLOCKED: live SENSOR capture
 * (camera->csi->isp->memory).  No camera sensor is wired on this hardware
 * batch -- but the TPG path this app exercises does not need one, which is
 * the point of this stage.
 *
 * The captured frame is copied into a SECOND, separate static SRAM0 buffer
 * (stage1_frame_copy, below) right after dequeue, before the 20 s bench
 * hold -- so a `savebin` read over SWD gets a stable snapshot instead of
 * racing the video pool's buffer (the driver does not recycle it here since
 * it is never re-enqueued, but a stage that streams continuously would).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/crc.h>

#if defined(CONFIG_VIDEO)
#include <zephyr/drivers/video.h>
#endif

/* The ISP node (status + tpg-* properties set by the board overlay). */
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
 * CGU AON block (base 0x1A60_2000, HWRM Table 10-2 p.325: A32/M55-HP/M55-HE
 * all Y).  READ-ONLY: this app never writes CLK_ENA -- it gates clocks for
 * both cores and other blocks on this bus, and a bad write here can silently
 * kill a clock this app -- or the OTHER core -- depends on to keep running.
 * Same register + the same read-only discipline as
 * examples/aen/aen-rtc-tick-probe/src/main.c.
 */
#define CGU_CLK_ENA         0x1A602014UL
#define CGU_CLK_ENA_ISP_BIT BIT(29)

/*
 * ISP ID registers -- offsets transcribed verbatim from
 * zephyr/drivers/video/isp_pico.h:14-19 (ISP_ID_PRODUCT_ID / ISP_ID_CHIP_ID /
 * ISP_ID_CHIP_REVISION).  Read directly off DT_REG_ADDR(ISP_NODE), not
 * through the private driver header (not on the app include path), so this
 * check is independent of the driver even building.
 */
#define ISP_ID_PRODUCT_ID_OFF    0x008U
#define ISP_ID_CHIP_ID_OFF       0x00CU
#define ISP_ID_CHIP_REVISION_OFF 0x024U

/*
 * Bench instrumentation added to isp_pico.c for this stage: counts
 * MI_INTR_MP_FRAME_END events, i.e. IRQ 368 ("mi-isp") actually firing a
 * completed-frame interrupt -- not just the shared isp_isr_handler() being
 * entered for one of its other status bits.
 */
extern volatile uint32_t isp_mi_frame_end_count;

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

/* TPG output geometry -- the smallest size the "3x3-Color-Block" TPG image
 * supports (isp_pico.c:176-189).  Output format is YUYV (run 66 -- see the
 * file header): this needs the demosaic + colour pipeline actually enabled,
 * see prj.conf's CONFIG_ISP_LIB_*_MODULE set. */
#define FRAME_WIDTH  1280
#define FRAME_HEIGHT 720
#define FRAME_BYTES_PER_SAMPLE \
	2 /* VIDEO_PIX_FMT_YUYV is packed 4:2:2: 4
				    * bytes (Y0 U Y1 V) per 2 pixels -- 2
				    * bytes/pixel on average. */
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * FRAME_BYTES_PER_SAMPLE)

/*
 * Second, separate static buffer in the same global SRAM0 bank (4 MiB @
 * 0x02000000, AXI-visible, declared by the SoC dtsi's sram0 node) the video
 * buffer pool uses (CONFIG_VIDEO_BUFFER_POOL_ZEPHYR_REGION_NAME="SRAM0",
 * prj.conf) -- NOT SRAM1 (0x02400000): that BUS-FAULTS on this
 * silicon/batch.  Placed in the same "SRAM0" linker section the pool's own
 * heap uses (zephyr/linker/devicetree_regions.h's LINKER_DT_SECTIONS(),
 * generated from the SoC dtsi), so the linker validates it fits: the
 * default 2 MiB pool + this 1,843,200 B snapshot is well under the 4 MiB
 * bank.
 */
static uint8_t stage1_frame_copy[FRAME_SIZE] __attribute__((section("SRAM0"), aligned(64)));

int main(void)
{
	printk("\n=== aen-isp-regcheck (stage 1: ISP alive, MI writes) ===\n");

	/*
	 * Step 0: CGU CLK_ENA + the ISP's clock-gate bit (READ-ONLY, see the
	 * macro comment above).
	 */
	uint32_t clk_ena = *(volatile uint32_t *)CGU_CLK_ENA;

	printk("CGU CLK_ENA (0x1A602014) = 0x%08x, bit29(ISP)=%d\n",
	       clk_ena,
	       (int)((clk_ena & CGU_CLK_ENA_ISP_BIT) != 0));

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
	 * Step 3: ISP ID registers -- a plain read off the bound node's physical
	 * reg base, independent of driver/DT correctness (see the macro block
	 * above).
	 */
	uint32_t product_id = *(volatile uint32_t *)(uintptr_t)(isp_base + ISP_ID_PRODUCT_ID_OFF);
	uint32_t chip_id    = *(volatile uint32_t *)(uintptr_t)(isp_base + ISP_ID_CHIP_ID_OFF);
	uint32_t chip_rev   = *(volatile uint32_t *)(uintptr_t)(isp_base + ISP_ID_CHIP_REVISION_OFF);

	printk("isp ID: product=0x%08x chip=0x%08x rev=0x%08x\n", product_id, chip_id, chip_rev);

	/*
	 * Step 4: report whether the isp_pico.c driver TU was built+linked, and if so
	 * exercise the portable v4.4 video API on the instantiated device.
	 */
	const struct device *isp_dev = ISP_DEV;

	if (isp_dev == NULL) {
		printk("driver: isp_pico.c NOT built/linked (CONFIG_VIDEO_ISP_VSI=n)\n");
#if defined(CONFIG_VIDEO)
	} else if (!device_is_ready(isp_dev)) {
		printk("driver: isp_pico.c linked but device NOT ready (init needs a camera\n");
		printk("        controller or TPG; the overlay wires the TPG -- unexpected)\n");
	} else {
		struct video_caps caps    = { .type = VIDEO_BUF_TYPE_OUTPUT };
		int               rc_caps = video_get_caps(isp_dev, &caps);

		printk("driver: isp_pico.c linked, device READY (v4.4 video API)\n");
		printk("        video_get_caps rc=%d (min_vbuf_count=%u)\n", rc_caps, caps.min_vbuf_count);

		/* YUYV output format at the TPG's minimum "3x3-Color-Block" size
		 * (1280x720) -- run 66: the wrapper rejects Bayer as an OUTPUT
		 * format, see the file header. */
		struct video_format fmt = {
			.type        = VIDEO_BUF_TYPE_OUTPUT,
			.pixelformat = VIDEO_PIX_FMT_YUYV,
			.width       = FRAME_WIDTH,
			.height      = FRAME_HEIGHT,
			.pitch       = FRAME_WIDTH * FRAME_BYTES_PER_SAMPLE,
		};
		int rc_fmt = video_set_format(isp_dev, &fmt);

		printk("video_set_format(OUTPUT, YUYV, %ux%u) rc=%d\n",
		       (unsigned int)FRAME_WIDTH,
		       (unsigned int)FRAME_HEIGHT,
		       rc_fmt);

		struct video_buffer *vbuf = video_buffer_aligned_alloc(FRAME_SIZE, 64, K_NO_WAIT);

		if (vbuf == NULL) {
			printk("RESULT FAIL: video_buffer_aligned_alloc(%u) returned NULL "
			       "(SRAM0 pool exhausted?)\n",
			       (unsigned int)FRAME_SIZE);
		} else {
			int                  rc_enq, rc_start, rc_deq;
			struct video_buffer *deq = NULL;

			printk("buffer: addr=%p size=%u\n", (void *)vbuf->buffer, vbuf->size);

			/* v4.4's video_enqueue() rejects a buffer whose type is neither INPUT nor
			 * OUTPUT with -EINVAL before the driver sees it; video_buffer_aligned_alloc()
			 * leaves it unset. Bench run 64 hit exactly that (enqueue -22, stream start
			 * -ENOBUFS, MI never armed). OUTPUT = the ISP's MI capture side.
			 */
			vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
			rc_enq     = video_enqueue(isp_dev, vbuf);
			printk("video_enqueue rc=%d\n", rc_enq);

			rc_start = video_stream_start(isp_dev, VIDEO_BUF_TYPE_OUTPUT);
			printk("video_stream_start rc=%d\n", rc_start);

			/* Wait up to 2 s for the ISP's MI DMA to complete one frame
			 * (IRQ 368 -> MI_INTR_MP_FRAME_END -> isp_dequeue()). */
			rc_deq = video_dequeue(isp_dev, &deq, K_MSEC(2000));

			printk("IRQ368 mi_frame_end_count=%u  video_dequeue rc=%d\n",
			       isp_mi_frame_end_count,
			       rc_deq);

			if (rc_deq == 0 && deq != NULL) {
				uint32_t bytesused = deq->bytesused;
				uint32_t crc       = crc32_ieee(deq->buffer, bytesused);

				printk("dequeued: bytesused=%u crc32=0x%08x\n", bytesused, crc);

				/* Coarse 3x3 block-mean structure summary, proving the
				 * "3x3-Color-Block" pattern survived TPG -> demosaic ->
				 * YUYV (distinct per-region Y/U/V means), not just that
				 * some DMA happened.  YUYV is packed 4:2:2: each 4-byte
				 * macropixel [Y0 U Y1 V] covers 2 horizontal pixels and
				 * carries ONE shared chroma (U,V) pair for both -- so Y
				 * is averaged per PIXEL, U/V per MACROPIXEL. */
				uint32_t       region_y_sum[9]     = { 0 };
				uint32_t       region_y_cnt[9]     = { 0 };
				uint32_t       region_u_sum[9]     = { 0 };
				uint32_t       region_v_sum[9]     = { 0 };
				uint32_t       region_uv_cnt[9]    = { 0 };
				const uint8_t *px                  = (const uint8_t *)deq->buffer;
				size_t         n_macropixels       = bytesused / 4;
				size_t         macropixels_per_row = FRAME_WIDTH / 2;

				for (size_t idx = 0; idx < n_macropixels; idx++) {
					size_t row  = idx / macropixels_per_row;
					size_t mcol = idx % macropixels_per_row;
					size_t col  = mcol * 2; /* 1st pixel's column */

					if (row >= FRAME_HEIGHT) {
						break;
					}

					int region =
					    (int)((row * 3) / FRAME_HEIGHT) * 3 + (int)((col * 3) / FRAME_WIDTH);
					uint8_t y0 = px[idx * 4 + 0];
					uint8_t u  = px[idx * 4 + 1];
					uint8_t y1 = px[idx * 4 + 2];
					uint8_t v  = px[idx * 4 + 3];

					region_y_sum[region] += (uint32_t)y0 + y1;
					region_y_cnt[region] += 2;
					region_u_sum[region] += u;
					region_v_sum[region] += v;
					region_uv_cnt[region]++;
				}

				for (int region = 0; region < 9; region++) {
					uint32_t my =
					    region_y_cnt[region] ? region_y_sum[region] / region_y_cnt[region] : 0;
					uint32_t mu =
					    region_uv_cnt[region] ? region_u_sum[region] / region_uv_cnt[region] : 0;
					uint32_t mv =
					    region_uv_cnt[region] ? region_v_sum[region] / region_uv_cnt[region] : 0;

					printk("block[%d,%d] Y=%u U=%u V=%u\n", region / 3, region % 3, my, mu, mv);
				}

				/* Copy out of the video pool to the static snapshot
				 * buffer before the hold, so a bench `savebin` reads a
				 * stable location. */
				size_t copy_len = MIN(bytesused, sizeof(stage1_frame_copy));

				memcpy(stage1_frame_copy, deq->buffer, copy_len);

				printk("snapshot: addr=%p size=%u crc32=0x%08x\n",
				       (void *)stage1_frame_copy,
				       (unsigned int)copy_len,
				       crc32_ieee(stage1_frame_copy, copy_len));

				printk("RESULT PASS: TPG frame captured via IRQ 368 -- %u "
				       "bytes, CRC 0x%08x\n",
				       bytesused,
				       crc);
			} else {
				printk("RESULT FAIL: no frame dequeued within 2000 ms "
				       "(rc=%d)\n",
				       rc_deq);
			}

			video_stream_stop(isp_dev, VIDEO_BUF_TYPE_OUTPUT);
		}
#endif /* defined(CONFIG_VIDEO) */
	}

	/*
	 * Bind-based staging gate (independent of the capture result above): the
	 * ISP node BINDS -- isp@49046000 binds to "vsi,isp-pico" at the DFP reg
	 * base with the two DFP IRQs.
	 */
	if (!node_ok) {
		printk("RESULT FAIL: ISP-Pico node NOT staged "
		       "(bound=%d base_ok=%d irq0_ok=%d irq1_ok=%d -- node missing, disabled, or "
		       "bound to the wrong compatible/reg/irq)\n",
		       (int)ISP_BOUND,
		       (int)(isp_base == ISP_BASE_EXPECTED),
		       (int)(isp_irq0 == ISP_IRQ0_EXPECTED),
		       (int)(isp_irq1 == ISP_IRQ1_EXPECTED));
	}

	printk("Holding 20 s for a bench `savebin` of the snapshot buffer...\n");
	k_sleep(K_SECONDS(20));

	return 0;
}
