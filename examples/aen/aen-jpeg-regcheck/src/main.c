/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-jpeg-regcheck -- <alp/jpeg.h> backend-selection proof.  Two scenarios
 * (see testcase.yaml) exercise the two backends the JPEG class currently
 * ships:
 *
 *   - native_sim: the portable TooJpeg-derived software fallback
 *     (src/backends/jpeg/sw_baseline.c, priority 50) -- runs for real, in CI.
 *
 *   - E1M-AEN801 (Ensemble E8, M55-HE): the Alif Hantro VC9000E hardware
 *     backend (src/backends/jpeg/alif_hantro.c, priority 100) -- SILICON-
 *     PROVEN on a real AEN801: alp_jpeg_encode() returns a valid 935-byte
 *     64x64 JPEG that round-trips through libjpeg.  Getting there took three
 *     defects a real bench run exposed + this batch fixed: (1) the missing
 *     CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8 select in prj.conf that silently let
 *     sw_baseline win instead of the HW backend; (2) the driver programming
 *     the HW output-size register from buf->bytesused (0 after
 *     video_import_buffer) instead of buf->size; and (3) the DMA buffers
 *     landing in core-local DTCM, unreachable by the Hantro AXI master --
 *     which is why nv12_buf / out_buf below are placed in global SRAM0.
 *     On real silicon, the first thing to check is the ported driver's
 *     jpeg_hw_init(): it reads JPEG_SWREG0 and compares it against the
 *     documented hardware ID (JPEG_HW_ID, 0x90001000), logging "JPEG
 *     hardware not found (ID: 0x%08x)" at LOG_ERR if it doesn't match --
 *     that register read succeeding is the actual silicon proof the Hantro
 *     block is alive and mapped at 0x49044000, independent of whether an
 *     end-to-end encode ever completes.
 *
 * Same source builds for both platforms: alp_jpeg_open() (backend selection),
 * alp_jpeg_capabilities() (caps query), alp_jpeg_encode() (the actual work)
 * and alp_jpeg_close() are identical portable calls either way.  The two
 * backends want genuinely different SOURCE-BUFFER LAYOUTS though (the Hantro
 * block latches one raw pointer and reads everything after it as one
 * semi-planar NV12 buffer, while the software encoder wants real separate
 * Y/U/V planes) -- rather than #ifdef-ing on which backend this BUILD links
 * (the old, now-removed approach), this app asks the WON backend at RUNTIME
 * which layouts it accepts (alp_jpeg_caps_t::pixfmt_mask) and builds
 * whichever one it advertises.  That's not just tidier: it is the only way
 * an app can stay correct if a THIRD backend with a THIRD layout preference
 * ever joins the class -- a compile-time #ifdef ladder would need a new arm
 * for every future backend, a runtime capability query does not.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/jpeg.h>
#include <alp/peripheral.h>

#define FRAME_W 64
#define FRAME_H 64
#define OUT_CAP 8192

/*
 * Backing storage for both layouts this app knows how to build.  Both
 * arrays exist unconditionally (no CONFIG_ALP_SDK_JPEG_ALIF_HANTRO #if
 * gate) -- at ~6 KiB combined for a 64x64 frame this is cheap, and it is
 * what makes the runtime pixfmt_mask branch below possible: whichever
 * layout the won backend wants, its buffer is already there to fill.
 */

/*
 * The Hantro VC9000E is an external AXI bus master: it fetches nv12_buf
 * (input) and writes out_buf (output) over its OWN AXI master, not through
 * the M55 core, so both addresses must be GLOBAL (AXI-visible) -- not a
 * core-local TCM alias.  The generated board's default RAM (`zephyr,sram =
 * &dtcm`) lands .bss in the M55's DTCM at the core-private local address
 * 0x20000000, which the Hantro's AXI master cannot reach -- that mismatch
 * is exactly the JPEG_BUS_ERROR_STATUS (ALP_ERR_IO) seen on the AEN801
 * bench.  This is the identical trap aen-dma-regcheck hit with the PL330
 * DMA AXI master (see that example's file header): the fix there, and here,
 * is to tag the DMA-visible buffers into the "SRAM0" linker region, which
 * resolves to the global on-chip SRAM0 bank @0x02000000 that every AXI
 * master on this SoC can see.  Guarded to the HW build only: native_sim
 * links this same source but has no jpeg0 node and no "SRAM0" memory
 * region, so the attribute must vanish there.
 */
#if defined(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO)
#define JPEG_DMA_MEM __attribute__((section("SRAM0")))
#else
#define JPEG_DMA_MEM
#endif

/* ALP_PIXFMT_NV12: one contiguous buffer, Y plane then an interleaved UV
 * plane at offset (FRAME_W * FRAME_H) -- what alif_hantro.c latches as a
 * single raw pointer (see its file-header note on the y_plane-as-NV12-base
 * constraint).  Hantro AXI INPUT -- must be SRAM0 (see JPEG_DMA_MEM above). */
static uint8_t nv12_buf[FRAME_W * FRAME_H + (FRAME_W * FRAME_H) / 2] JPEG_DMA_MEM;

/* ALP_PIXFMT_YUV420_PLANAR: three independent buffers -- what sw_baseline.c
 * (toojpeg_encode_yuv420) wants, one U and one V sample per 2x2 luma block. */
static uint8_t y_buf[FRAME_W * FRAME_H];
static uint8_t u_buf[(FRAME_W / 2) * (FRAME_H / 2)];
static uint8_t v_buf[(FRAME_W / 2) * (FRAME_H / 2)];

/* Diagonal gradient in Y, neutral (grey) chroma -- real varying luma data
 * the encoder can't trivially collapse to one DCT coefficient, while chroma
 * content is irrelevant to the SOI-marker pass/fail check below. */
static void build_frame_nv12(alp_jpeg_encode_req_t *req)
{
	uint8_t *y  = nv12_buf;
	uint8_t *uv = nv12_buf + (FRAME_W * FRAME_H);

	for (int r = 0; r < FRAME_H; ++r) {
		for (int c = 0; c < FRAME_W; ++c) {
			y[r * FRAME_W + c] = (uint8_t)((r + c) * 2);
		}
	}
	/* One interleaved U,V pair per 2x2 luma block: (FRAME_W/2)*(FRAME_H/2)
	 * pairs = FRAME_W*FRAME_H/2 bytes. */
	memset(uv, 128, (FRAME_W * FRAME_H) / 2);

	req->format   = ALP_PIXFMT_NV12;
	req->y_plane  = y;
	req->y_stride = FRAME_W;
	req->u_plane  = NULL; /* NV12: the UV plane lives inside y_plane -- not consulted. */
	req->v_plane  = NULL;
}

static void build_frame_planar(alp_jpeg_encode_req_t *req)
{
	for (int r = 0; r < FRAME_H; ++r) {
		for (int c = 0; c < FRAME_W; ++c) {
			y_buf[r * FRAME_W + c] = (uint8_t)((r + c) * 2);
		}
	}
	memset(u_buf, 128, sizeof(u_buf));
	memset(v_buf, 128, sizeof(v_buf));

	req->format   = ALP_PIXFMT_YUV420_PLANAR;
	req->y_plane  = y_buf;
	req->y_stride = FRAME_W;
	req->u_plane  = u_buf;
	req->u_stride = FRAME_W / 2;
	req->v_plane  = v_buf;
	req->v_stride = FRAME_W / 2;
}

/* Hantro AXI OUTPUT -- must be SRAM0 too (see JPEG_DMA_MEM above). */
static uint8_t out_buf[OUT_CAP] JPEG_DMA_MEM;

int main(void)
{
	printk("\n=== aen-jpeg-regcheck ===\n");

	/* alp_jpeg_open() walks the backend registry (alp_backend_select on
	 * class "jpeg" + this build's ALP_SOC_REF_STR) and picks whichever
	 * backend wins -- alif_hantro (priority 100) on the AEN801
	 * hardware-path build, sw_baseline (priority 50) everywhere else,
	 * including native_sim. */
	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	if (h == NULL) {
		printk("RESULT FAIL: alp_jpeg_open() returned NULL (err=%d)\n", (int)alp_last_error());
		return 0;
	}

	/* Query the WON backend's capabilities -- hw_accelerated is the one
	 * bit that tells you, at runtime, which of the two backends above
	 * actually got selected for this build; pixfmt_mask tells you which
	 * source-buffer layout(s) it will accept. */
	alp_jpeg_caps_t caps;
	alp_status_t    rc = alp_jpeg_capabilities(h, &caps);
	printk("caps  : rc=%d hw_accelerated=%d mjpeg_supported=%d max=%ux%u "
	       "subsample_mask=0x%x pixfmt_mask=0x%x\n",
	       (int)rc,
	       (int)caps.hw_accelerated,
	       (int)caps.mjpeg_supported,
	       caps.max_width,
	       caps.max_height,
	       caps.subsample_mask,
	       caps.pixfmt_mask);

	alp_jpeg_encode_req_t req = {
		.width     = FRAME_W,
		.height    = FRAME_H,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
	};

	/* Build whichever layout the won backend actually advertises, instead
	 * of assuming one from which CONFIG_* happened to be set at compile
	 * time.  NV12 first: it's the HW-path preference, and no backend this
	 * class ships advertises both bits at once, so the order here only
	 * matters if that ever changes. */
	if (caps.pixfmt_mask & (1u << ALP_PIXFMT_NV12)) {
		build_frame_nv12(&req);
	} else if (caps.pixfmt_mask & (1u << ALP_PIXFMT_YUV420_PLANAR)) {
		build_frame_planar(&req);
	} else {
		printk("RESULT FAIL: won backend advertises no pixfmt this app knows how to build "
		       "(pixfmt_mask=0x%x)\n",
		       caps.pixfmt_mask);
		alp_jpeg_close(h);
		return 0;
	}

	size_t out_len = 0;
	rc             = alp_jpeg_encode(h, &req, out_buf, sizeof(out_buf), &out_len);

	/* A valid JPEG stream always opens with the SOI (Start Of Image)
	 * marker, FF D8 -- checking it is a cheap, backend-agnostic proof
	 * that a real JPEG-shaped byte stream came back, without needing a
	 * full decoder in this example. */
	bool pass = (rc == ALP_OK) && (out_len >= 2) && (out_buf[0] == 0xFFu) && (out_buf[1] == 0xD8u);

	printk("encode: rc=%d out_len=%u first2=%02x%02x (expect ffd8 SOI)\n",
	       (int)rc,
	       (unsigned)out_len,
	       out_buf[0],
	       out_buf[1]);

	alp_jpeg_close(h);

	if (pass) {
		printk("RESULT PASS: %s backend encoded %u bytes starting with the JPEG SOI "
		       "marker (FF D8)\n",
		       caps.hw_accelerated ? "alif_hantro (HW)" : "sw_baseline (SW)",
		       (unsigned)out_len);
	} else {
		printk("RESULT FAIL: encode rc=%d out_len=%u\n", (int)rc, (unsigned)out_len);
	}

	return 0;
}
