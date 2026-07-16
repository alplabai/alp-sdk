/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ai-camera-viewer
 * ================
 *
 * Headline edge-AI demo on the E1M-AEN family.
 *
 *   ┌──────────────────┐                ┌──────────────────────┐
 *   │  E1M-AEN801 SoM  │  MIPI / DVP    │ OmniVision OV5640    │
 *   │ + Cortex-M55 HP  │ ◀──── frames ──│ camera (RGB565 / VGA)│
 *   └────────┬─────────┘                └──────────────────────┘
 *            │   <alp/camera.h> hands the frame to the inference
 *            │   pipeline:
 *            ▼
 *      ┌──────────────────────┐
 *      │ TFLM + Ethos-U85      │  ── person_detect.tflite
 *      │ (CONFIG_ALP_TFLM_     │     compiled with Vela for
 *      │  ETHOS_U85=y on E8)   │     ethos-u85-256.
 *      └─────────┬─────────────┘
 *                │  ── bounding-box list + per-class score
 *                ▼
 *           ┌──────────┐
 *           │  LVGL    │  draws the latest preview frame on the
 *           │  UI      │  ST7789 + overlays the boxes + a stats
 *           └──────────┘  strip (latency + class confidence).
 *
 *
 * ── Why this demo matters for customers ───────────────────────
 *
 * Three customer questions get answered side-by-side:
 *   1. "Can I run real inference on the AEN's on-die NPU?"  --
 *      yes, the dispatch path is alp_inference_open(..., backend
 *      = ALP_INFERENCE_BACKEND_ETHOS_U).  The §D.lib.loader picks
 *      the U85 vs U55 vs U65 driver shim from the SKU's
 *      `capabilities:` block, no app-source changes.
 *   2. "How fast?"  --  the per-frame latency strip prints the
 *      microseconds from `alp_inference_invoke()` entry to exit.
 *      Customers compare E8 / E6 / E4 by flipping `som.sku`
 *      in `board.yaml`.
 *   3. "Does it run portably?"  --  the app source uses zero
 *      vendor-specific symbols; the SoC-family routing is in the
 *      loader, not the source.  Re-targeting from AEN to NX9101
 *      (Ethos-U65) is a one-line board.yaml change.
 *
 *
 * ── What's still a placeholder ──────────────────────────────────
 *
 * - The actual `person_detect.tflite` model isn't checked in --
 *   customers drop their own Vela-compiled .tflite into
 *   `models/` and the loader picks it up via the include mount.
 * - The camera capture path falls back to NOSUPPORT on
 *   native_sim (no real OV5640 connected), and the UI shows a
 *   solid-colour placeholder + the inference latency for the
 *   built-in dummy bytes.
 * - Real bounding-box decode is paper-correct only -- the
 *   post-process kernel that turns the model output tensor into
 *   a `viewer_box_t[]` array is still TODO, pending the real
 *   compiled model + AEN HiL bench validation.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/peripheral.h"
#include "alp/camera.h"
#include "alp/inference.h"
#include <alp/display.h>
#include <alp/gui.h>

#include <lvgl.h>

#include "viewer_ui.h"
#include "inference_loop.h"

LOG_MODULE_REGISTER(ai_camera_viewer, LOG_LEVEL_INF);

static viewer_state_t g_state;

K_THREAD_STACK_DEFINE(infer_stack, 8192);
static struct k_thread infer_thread;

static void infer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	inference_loop_run(&g_state);
}

int main(void)
{
	LOG_INF("ai-camera-viewer demo starting");

	/* The camera SCCB + capture path are bring-up duties for the
     * inference loop -- this thread owns the chip handle so it
     * can re-init on failure.  Pass our shared state pointer in
     * via the thread arg slot (NULL above; the loop fetches it
     * from main's stack via g_state directly). */

	/* Display + LVGL bring-up.  Open the panel through the portable
     * <alp/display.h> surface, then bind LVGL to it via <alp/gui.h>
     * -- see examples/display/lvgl-widgets-demo/src/main.c for the
     * full rationale on this open -> lv_init -> tick -> attach
     * sequence, and why CONFIG_LV_Z_AUTO_INIT=n (prj.conf) is
     * required alongside it. */
	alp_display_config_t display_cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);
	alp_display_t       *display     = alp_display_open(&display_cfg);
	if (display == NULL) {
		LOG_ERR("display open failed (err=%d)", (int)alp_last_error());
		return 1;
	}
	lv_init();
	lv_tick_set_cb(k_uptime_get_32);

	alp_status_t attach_status = alp_gui_lvgl_attach(display);
	if (attach_status != ALP_OK) {
		LOG_ERR("alp_gui_lvgl_attach failed (err=%d)", (int)attach_status);
		return 1;
	}

	viewer_ui_build();

	/* Spawn the inference thread.  Higher priority than main so
     * the model gets timely CPU even when LVGL is mid-blit. */
	k_thread_create(&infer_thread,
	                infer_stack,
	                K_THREAD_STACK_SIZEOF(infer_stack),
	                infer_entry,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(3),
	                0,
	                K_NO_WAIT);
	k_thread_name_set(&infer_thread, "infer_loop");

	/* Render loop: 30 fps, sample the shared state once per
     * frame to avoid tearing the bounding-box overlay mid-update. */
	while (1) {
		viewer_state_t snap = g_state;
		viewer_ui_apply(&snap);

		const uint32_t sleep_ms = lv_timer_handler();
		k_msleep(MIN(sleep_ms, 10u));
	}

	return 0;
}
