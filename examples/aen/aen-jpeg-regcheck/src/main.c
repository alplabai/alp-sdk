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
 *     backend (src/backends/jpeg/alif_hantro.c, priority 100) -- BUILD-ONLY
 *     on this task; the HW encode path is BENCH-PENDING (not yet run on real
 *     AEN801 silicon).  On real silicon, the first thing to check is the
 *     ported driver's jpeg_hw_init(): it reads JPEG_SWREG0 and compares it
 *     against the documented hardware ID (JPEG_HW_ID, 0x90001000), logging
 *     "JPEG hardware not found (ID: 0x%08x)" at LOG_ERR if it doesn't match
 *     -- that register read succeeding is the actual silicon proof the
 *     Hantro block is alive and mapped at 0x49044000, independent of
 *     whether an end-to-end encode ever completes.
 *
 * Same source builds for both platforms: alp_jpeg_open() (backend selection),
 * alp_jpeg_capabilities() (caps query), alp_jpeg_encode() (the actual work)
 * and alp_jpeg_close() are identical portable calls either way.  Only the
 * synthetic frame's memory LAYOUT differs (see the #if block below) --
 * that's a real backend constraint, not example plumbing: the Hantro block
 * latches one raw pointer and reads everything after it as one semi-planar
 * NV12 buffer, while the software encoder wants genuinely separate Y/U/V
 * planes.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/jpeg.h>

#define FRAME_W 64
#define FRAME_H 64
#define OUT_CAP 8192

#if defined(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO)
/*
 * alif_hantro.c latches ONE input pointer (VIDEO_CID_JPEG_INPUT_BUFFER) and
 * hands it straight to the HW as semi-planar 4:2:0 (NV12): the Y plane
 * immediately followed, at stride*height, by an interleaved UV plane.  So on
 * this backend the "frame" is one contiguous buffer -- lay it out that way
 * and pass only y_plane; u_plane/v_plane are ignored by this backend (see
 * its file-header note) so they stay NULL here.
 */
static uint8_t frame_buf[FRAME_W * FRAME_H + (FRAME_W * FRAME_H) / 2];

static void build_frame(alp_jpeg_encode_req_t *req)
{
	uint8_t *y  = frame_buf;
	uint8_t *uv = frame_buf + (FRAME_W * FRAME_H);

	for (int r = 0; r < FRAME_H; ++r) {
		for (int c = 0; c < FRAME_W; ++c) {
			/* Diagonal gradient: gives the encoder real varying
			 * data instead of a flat block it could trivially
			 * collapse to one DCT coefficient. */
			y[r * FRAME_W + c] = (uint8_t)((r + c) * 2);
		}
	}
	/* Neutral (grey) chroma, interleaved U,V pairs -- one pair per 2x2
	 * luma block: (FRAME_W/2)*(FRAME_H/2) pairs = FRAME_W*FRAME_H/2 bytes. */
	memset(uv, 128, (FRAME_W * FRAME_H) / 2);

	req->y_plane  = y;
	req->y_stride = FRAME_W;
	req->u_plane  = NULL;
	req->v_plane  = NULL;
}
#else
/*
 * sw_baseline.c (toojpeg_encode_yuv420) wants genuinely separate planes --
 * true 4:2:0 planar, one U sample and one V sample per 2x2 luma block.
 */
static uint8_t y_buf[FRAME_W * FRAME_H];
static uint8_t u_buf[(FRAME_W / 2) * (FRAME_H / 2)];
static uint8_t v_buf[(FRAME_W / 2) * (FRAME_H / 2)];

static void build_frame(alp_jpeg_encode_req_t *req)
{
	for (int r = 0; r < FRAME_H; ++r) {
		for (int c = 0; c < FRAME_W; ++c) {
			y_buf[r * FRAME_W + c] = (uint8_t)((r + c) * 2);
		}
	}
	memset(u_buf, 128, sizeof(u_buf));
	memset(v_buf, 128, sizeof(v_buf));

	req->y_plane  = y_buf;
	req->y_stride = FRAME_W;
	req->u_plane  = u_buf;
	req->u_stride = FRAME_W / 2;
	req->v_plane  = v_buf;
	req->v_stride = FRAME_W / 2;
}
#endif

static uint8_t out_buf[OUT_CAP];

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
	 * actually got selected for this build. */
	alp_jpeg_caps_t caps;
	alp_status_t    rc = alp_jpeg_capabilities(h, &caps);
	printk("caps  : rc=%d hw_accelerated=%d mjpeg_supported=%d max=%ux%u "
	       "subsample_mask=0x%x\n",
	       (int)rc,
	       (int)caps.hw_accelerated,
	       (int)caps.mjpeg_supported,
	       caps.max_width,
	       caps.max_height,
	       caps.subsample_mask);

	alp_jpeg_encode_req_t req = {
		.width     = FRAME_W,
		.height    = FRAME_H,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
	};
	build_frame(&req);

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
