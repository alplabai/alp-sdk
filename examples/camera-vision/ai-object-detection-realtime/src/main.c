/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ai-object-detection-realtime
 * ============================
 *
 * Realtime YOLOv8-tiny object detection on a DEEPX NPU.
 *
 *     +----------------+    MIPI CSI-2    +----------------+
 *     | E1M-V2M101 SoM | <--- frames ---- | OV5640 camera  |
 *     | (V2N + DX-M1)  |   (RGB565 240x)  | on E1M-EVK     |
 *     +-------+--------+                  +----------------+
 *             |  <alp/camera.h> hands the frame off to the
 *             |  inference pipeline (DEEPX backend resolved
 *             |  from the SKU's preferred_backend by the
 *             |  S-D.lib loader; no vendor symbols in app code).
 *             v
 *     +----------------------+
 *     | <alp/inference.h>    |  YOLOv8-tiny.dxnn on DX-M1
 *     |   AUTO backend       |  (29 TOPS on V2M101;
 *     |                      |   113 TOPS on V2M201 / DX-M2).
 *     +-----------+----------+
 *                 |   bbox list + per-class score
 *                 v
 *            +---------+
 *            | display |  ST7789 framebuffer push via the
 *            +---------+  portable <alp/display.h> blit -- the
 *                         demo composites the preview, the
 *                         bounding-box overlay, and a live FPS
 *                         counter strip.
 *
 *
 * == Why this demo matters for customers ==
 *
 * Three questions answered side-by-side, same source code:
 *
 *   1. "Can the SDK drive a real production-class NPU?"  Yes --
 *      alp_inference_open() dispatches to the DEEPX DX-M1 on
 *      V2M101; the vendor SDK shim sits behind the surface, app
 *      code stays portable.
 *   2. "How fast?"  The FPS counter prints the rolling 1 s frame
 *      rate; per-invoke latency is exposed via k_cycle_get_32()
 *      around alp_inference_invoke().  Customers compare V2M101
 *      (29 TOPS) vs V2M201 (113 TOPS) by flipping `som.sku` in
 *      board.yaml.
 *   3. "Is the pipeline portable?"  Same C source compiles for
 *      AEN (Ethos-U), V2N (DRP-AI3), V2N-M1 (DEEPX DX-M1).  The
 *      board.yaml resolves the NPU; the application is unaware.
 *
 *
 * == What's still a placeholder ==
 *
 * - The Vela-/DXNN-compiled YOLOv8-tiny model isn't checked in;
 *   the README explains the convert-and-include workflow.  This
 *   skeleton wires the surfaces so the build passes, the demo
 *   prints `[obj-det] done` once it has cycled through one
 *   complete capture/infer/display pass.
 * - The bounding-box decode kernel is paper-correct only; the
 *   real YOLOv8 head post-process (objectness threshold + NMS)
 *   that turns the raw output tensor into a list of bbox_t
 *   entries is still TODO, pending the real compiled model (see
 *   the README's "Adding the model" section).
 * - On native_sim the camera + NPU paths return ALP_ERR_NOSUPPORT;
 *   the loop tolerates that and exits after a single pass so the
 *   harness regex sees the `[obj-det] done` marker.
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/peripheral.h"
#include "alp/camera.h"
#include "alp/inference.h"
/* <alp/display.h> ships the full surface today -- open/clear/close
 * plus `alp_display_blit` and `alp_display_get_caps` are all
 * available (see include/alp/display.h).  This skeleton doesn't
 * wire the ST7789 blit path up yet; it prints the bounding-box
 * list to stdout each frame instead -- enough to exercise the
 * capture/infer pipeline end-to-end and let the twister harness
 * pick up the `[obj-det] done` marker. */

LOG_MODULE_REGISTER(obj_det, LOG_LEVEL_INF);

/* Capture / display geometry.  240x240 fits comfortably inside
 * the ST7789's 240x320 panel and keeps the YOLOv8-tiny input
 * tensor on a 32 B alignment without padding.  Bump to 320x320
 * once this example moves past the placeholder model onto a real
 * bench -- the <alp/camera.h> MIPI CSI-2 wrapper already supports
 * higher resolutions. */
#define FRAME_W   240u
#define FRAME_H   240u
#define FRAME_FPS 30u

/* Maximum bounding boxes the display path renders per frame.
 * YOLOv8-tiny on a 240x240 input typically emits <=8 high-
 * confidence detections; everything else gets pruned by the
 * post-process NMS step (still TODO -- see decode_boxes() below). */
#define MAX_BOXES 8u

typedef struct {
	uint16_t x, y, w, h;
	uint16_t class_id;
	float    score;
} bbox_t;

/* Placeholder model bytes.  Customers replace with the actual
 * Vela-/DXNN-compiled `yolov8n.tflite` (Ethos-U / DRPAI) or
 * `yolov8n.dxnn` (DEEPX) -- see README "Adding the model" for
 * the convert-and-include workflow.  Sized to one byte so the
 * inference_open path returns NOSUPPORT cleanly on skeleton
 * builds instead of segfaulting on an empty pointer. */
static const uint8_t s_model[] = { 0x00 };

/* Tensor arena for the on-chip backend.  Real YOLOv8-tiny needs
 * ~512 kB on Ethos-U; the DEEPX backend manages its own arena
 * inside the DX-M1 (host arena is small bookkeeping only). */
static uint8_t s_arena[64 * 1024] __aligned(16);

/* Decode the model's output tensor into a bbox_t array.  This
 * skeleton emits a single synthetic box so the display overlay
 * path has something to render; wiring the real YOLOv8 head
 * post-process (sigmoid + NMS) is still TODO, pending a real
 * compiled model. */
static size_t decode_boxes(const alp_inference_tensor_t *out, bbox_t *boxes, size_t max)
{
	(void)out;
	if (max == 0) {
		return 0;
	}
	boxes[0] = (bbox_t){
		.x        = 60,
		.y        = 60,
		.w        = 120,
		.h        = 120,
		.class_id = 0,
		.score    = 0.92f,
	};
	return 1;
}

int main(void)
{
	LOG_INF("ai-object-detection-realtime starting");

	/* Camera bring-up.  On native_sim alp_camera_open returns
     * NULL with alp_last_error() == ALP_ERR_NOSUPPORT; we
     * tolerate that so the harness can verify the build path. */
	alp_camera_t *cam       = alp_camera_open(&(alp_camera_config_t){
	    .camera_id = 0,
	    .width     = FRAME_W,
	    .height    = FRAME_H,
	    .fps       = FRAME_FPS,
	    .format    = ALP_PIXFMT_RGB565,
	});
	const bool    camera_ok = (cam != NULL);
	if (!camera_ok) {
		LOG_WRN("camera open NOSUPPORT (err=%d) -- skeleton mode", (int)alp_last_error());
	} else {
		/* Tolerate NOSUPPORT on stream-start too; the capture
         * path inside the loop downgrades cleanly. */
		(void)alp_camera_start(cam);
	}

	/* Inference bring-up.  AUTO routes to the SoM's preferred
     * NPU -- DEEPX DX-M1 on V2M101, Ethos-U on AEN, DRPAI on
     * stock V2N, CPU fallback on native_sim. */
	alp_inference_t *inf          = alp_inference_open(&(alp_inference_config_t){
	    .backend = ALP_INFERENCE_BACKEND_AUTO,
	    /* TODO: once a real per-backend model is checked in,
         * switch `.format` based on the active backend (DXNN
         * for DEEPX, VELA for Ethos-U/DRP-AI).  This skeleton
         * leaves DXNN as the placeholder default since the
         * demo's flagship target is the DEEPX path on V2M101. */
	    .format      = ALP_INFERENCE_MODEL_DXNN,
	    .model_data  = s_model,
	    .model_size  = sizeof(s_model),
	    .arena       = s_arena,
	    .arena_bytes = sizeof(s_arena),
	});
	const bool       inference_ok = (inf != NULL);
	if (!inference_ok) {
		LOG_WRN("inference open NOSUPPORT (err=%d) -- skeleton mode", (int)alp_last_error());
	}

	/* TODO: display bring-up via <alp/display.h>.  `alp_display_blit`
     * and `alp_display_get_caps` are available today (see the
     * header comment above); for now the demo streams the
     * bounding-box list + FPS to stdout instead so the pipeline
     * is testable without the framebuffer path.  Swap the printf
     * below for an alp_display_blit() of the rasterised overlay
     * once this skeleton grows a real model to render. */

	/* Pipeline pass counter -- this skeleton runs a single full
     * pass and exits so the harness regex catches the
     * `[obj-det] done` marker.  Real builds (V2N-M1 HiL) flip
     * to a perpetual loop. */
	uint32_t frames          = 0;
	uint32_t window_start_ms = k_uptime_get_32();
	uint32_t fps_x10         = 0;

	/* One full capture -> infer -> display pass.  The
     * surrounding `while (frames < 1)` form keeps the skeleton's
     * runtime bounded for native_sim; promote to `while (1)` on
     * real hardware. */
	while (frames < 1) {
		/* Capture.  When the camera is offline (native_sim) the
         * inference path runs on the zero-initialised input
         * tensor so the timing harness still ticks. */
		if (camera_ok) {
			alp_camera_frame_t frame = { 0 };
			if (alp_camera_capture(cam, &frame, /*timeout_ms=*/100) == ALP_OK) {
				/* TODO: memcpy(frame.data, ...) into the model's
                 * input tensor via alp_inference_get_input() --
                 * that accessor is available today (see
                 * include/alp/inference.h).  This skeleton skips
                 * the copy since the placeholder model has zero
                 * inputs. */
				(void)alp_camera_release(cam, &frame);
			}
		}

		/* Invoke.  Time the call so the on-screen FPS strip can
         * cross-check the per-invoke latency floor. */
		uint32_t t0 = k_cycle_get_32();
		if (inference_ok) {
			(void)alp_inference_invoke(inf);
		}
		uint32_t       t1        = k_cycle_get_32();
		const uint32_t invoke_us = k_cyc_to_us_floor32(t1 - t0);

		/* Decode bounding boxes from the model output tensor. */
		bbox_t boxes[MAX_BOXES];
		size_t n_boxes = 0;
		if (inference_ok && alp_inference_num_outputs(inf) > 0) {
			alp_inference_tensor_t out = { 0 };
			if (alp_inference_get_output(inf, 0, &out) == ALP_OK) {
				n_boxes = decode_boxes(&out, boxes, MAX_BOXES);
			}
		} else {
			/* Skeleton fallback -- emit one synthetic box so the
             * overlay path renders something visible. */
			n_boxes = decode_boxes(NULL, boxes, MAX_BOXES);
		}

		/* TODO: rasterise each bbox_t into the framebuffer here
         * and call alp_display_blit() -- available today -- to
         * push the strip out to the ST7789.  For now this
         * skeleton just walks the list so the unused-variable
         * warning stays quiet. */
		for (size_t i = 0; i < n_boxes; i++) {
			(void)boxes[i];
		}

		/* FPS estimate over a rolling 1 s window. */
		frames++;
		const uint32_t now_ms = k_uptime_get_32();
		if (now_ms - window_start_ms >= 1000) {
			fps_x10         = frames * 10u;
			frames          = 0;
			window_start_ms = now_ms;
		}

		printf("[obj-det] frame invoke=%uus boxes=%zu fps=%u.%u\n",
		       (unsigned)invoke_us,
		       n_boxes,
		       (unsigned)(fps_x10 / 10u),
		       (unsigned)(fps_x10 % 10u));
	}

	/* Tidy up so this skeleton's NOSUPPORT path closes the
     * surfaces in the same order real hardware will: inference,
     * camera.  There's no display handle to close here since
     * this skeleton streams to stdout instead of opening
     * <alp/display.h> (alp_display_close is available once it
     * does). */
	alp_inference_close(inf);
	if (camera_ok) {
		(void)alp_camera_stop(cam);
	}
	alp_camera_close(cam);

	printf("[obj-det] done\n");
	return 0;
}
