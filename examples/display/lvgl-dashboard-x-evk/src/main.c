/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * lvgl-dashboard-x-evk
 * ====================
 *
 * A minimal LVGL 9 dashboard rendered on the E1M-X V2N MIPI-DSI panel
 * (RK055HDMIPI4MA0, 720 x 1280 @ 60 Hz) via the standard Linux DRM/KMS
 * pipeline.
 *
 *
 * ── Why no alp_display_* API here? ────────────────────────────────────
 *
 *   On the Renesas RZ/V2N family the display pipeline is owned by the
 *   A55 Linux cluster.  At the Linux level the panel is just a DRM/KMS
 *   node exported by Renesas' `rz-du` DRM driver + the DSI bridge +
 *   the panel driver (hx8394-based).  There is no alp_display_*
 *   indirection on Linux -- the OS-provided DRM stack IS the portable
 *   surface.  Apps talk to it directly (or through a toolkit such as
 *   LVGL, Qt, Wayland/Weston).
 *
 *
 * ── LVGL Linux DRM backend ────────────────────────────────────────────
 *
 *   LVGL 9.1 ships a turnkey Linux DRM backend
 *   (src/drivers/display/drm/lv_linux_drm.c) that:
 *
 *     1. Opens the requested DRM card (card0 on a stock alp-image-edge).
 *     2. Picks the first connected connector (or a specific connector_id
 *        when -1 is not passed as the wildcard).
 *     3. Sets up double-buffered dumb BOs for scanout.
 *     4. Registers itself as an lv_display_t so LVGL renders into it
 *        at the panel's native resolution.
 *
 *   The backend is compiled in when the Yocto `lvgl` recipe is built
 *   with PACKAGECONFIG = "drm", which sets CONFIG_LV_USE_LINUX_DRM=y
 *   and CONFIG_LV_USE_EVDEV=y via meta-oe's drm.cfg fragment.
 *
 *
 * ── Touch input ───────────────────────────────────────────────────────
 *
 *   When present, the GT911 capacitive touch controller is exposed as
 *   a standard evdev pointer (/dev/input/event0 on a stock image).
 *   LVGL's evdev backend (lv_evdev_create) reads it without any
 *   board-specific glue.
 *
 *   NOTE: on current V2N hardware the GT911's I2C bus terminates at
 *   the GD32 IO-MCU (BRD_I2C), not directly at the RZ/V2N RIIC
 *   controller.  The GD32 I2C-proxy follow-up task will add a Linux
 *   master path.  Until then TOUCH_EVDEV may not exist -- the demo
 *   detects this at runtime with access(2) and degrades gracefully to
 *   display-only operation.
 *
 *
 * ── Weston / DRM master conflict ──────────────────────────────────────
 *
 *   Only one DRM master may scan out at a time.  If alp-image-edge
 *   starts Weston on boot, you must stop it before running this app:
 *
 *     systemctl stop weston
 *     alp-lvgl-dashboard
 *
 *   See README.md for the full bring-up sequence.
 *
 *
 * ── Build ─────────────────────────────────────────────────────────────
 *
 *   Yocto (recommended): the alp-lvgl-dashboard recipe in
 *   meta-alp-sdk packages this binary into alp-image-edge.  After
 *   booting the image, run `alp-lvgl-dashboard` from the console or
 *   SSH.
 *
 *   Standalone cross-compile (hand-written firmware path):
 *     cmake -DCMAKE_TOOLCHAIN_FILE=<sysroot>/toolchain.cmake \
 *           -S examples/display/lvgl-dashboard-x-evk -B build/dash
 *     cmake --build build/dash
 *     scp build/dash/alp-lvgl-dashboard root@<board-ip>:
 *
 *   Both paths are first-class; see README.md for details.
 */

#include <unistd.h>      /* access(2), usleep(3) */
#include <lvgl/lvgl.h>   /* LVGL core + widget API (installed under
                          * ${includedir}/lvgl/ by meta-oe's lvgl recipe) */

/* DRM backend header -- provides lv_linux_drm_create / lv_linux_drm_set_file.
 * Only compiled into the library when LV_USE_LINUX_DRM=y (set by the Yocto
 * drm PACKAGECONFIG fragment).  Guarded at runtime by the same flag. */
#include <lvgl/src/drivers/display/drm/lv_linux_drm.h>

/* evdev input backend -- provides lv_evdev_create / lv_evdev_delete.
 * Also enabled by the drm.cfg fragment (LV_USE_EVDEV=y). */
#include <lvgl/src/drivers/evdev/lv_evdev.h>

/* ── Hardware node paths ─────────────────────────────────────────────── */

/* Primary DRM card for the rz-du display pipeline.  On a stock
 * alp-image-edge without GPU a second card usually doesn't exist, but
 * if it does the panel is always card0 (enumerated first by rz-du). */
#define DRM_CARD     "/dev/dri/card0"

/* GT911 touch node after the GD32 I2C-proxy lands.  On current silicon
 * this node is absent -- the access(2) guard below prevents a hard
 * failure so the dashboard runs display-only in the meantime. */
#define TOUCH_EVDEV  "/dev/input/event0"

/* ── UI construction ─────────────────────────────────────────────────── */

/*
 * make_dashboard() -- populate the active screen with three teaching widgets:
 *
 *   1. Title label   -- shows the platform name at the top of the screen.
 *   2. Arc gauge     -- a 360x360 arc set to 62%, representing any analogue
 *                       quantity (CPU load, battery, signal strength).
 *   3. Touch button  -- demonstrates interactive input; the GT911 sends
 *                       pointer events through the evdev indev registered
 *                       below once the I2C-proxy path is live.
 *
 * This function is intentionally minimal -- it demonstrates the LVGL 9
 * object hierarchy (lv_screen_active → child widget) without adding
 * framework complexity on top.  Real applications would call
 * lv_timer_create() for periodic data refresh or use LVGL's observer API.
 */
static void make_dashboard(void)
{
	/* lv_screen_active() returns the default screen; it is created
	 * automatically by lv_init() and bound to the first registered
	 * lv_display_t.  Widgets parented to it appear on the panel. */
	lv_obj_t *scr = lv_screen_active();

	/* ── Title ── */
	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "ALP SDK -- E1M-X V2N");
	/* LV_ALIGN_TOP_MID centres the label horizontally, 24 px from the
	 * top edge; keeps it visible on both portrait and landscape panels. */
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

	/* ── Arc gauge ── */
	lv_obj_t *arc = lv_arc_create(scr);
	/* 360 x 360 pixels -- comfortable on a 720 x 1280 panel and large
	 * enough to read at arm's length.  The arc draws a 270-degree sweep
	 * by default; lv_arc_set_value maps [0..100] onto that sweep. */
	lv_obj_set_size(arc, 360, 360);
	lv_arc_set_value(arc, 62); /* 62% -- placeholder for live telemetry */
	lv_obj_center(arc);        /* centred in the screen object */

	/* ── Touch-me button ── */
	lv_obj_t *btn = lv_button_create(scr);
	/* 48 px above the bottom edge so it clears any system navbar overlay. */
	lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -48);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "Touch me");
	/* No explicit size on btn -- LVGL wraps it around the label. */
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(void)
{
	/* lv_init() allocates LVGL's global state (draw buffers, timer queue,
	 * indev list).  Must be called before any other LVGL function. */
	lv_init();

	/* ── Display backend ── */

	/* lv_linux_drm_create() registers a new lv_display_t backed by the
	 * Linux DRM subsystem.  It does NOT open the card yet -- that happens
	 * inside lv_linux_drm_set_file() so the caller can set the path and
	 * connector selector first. */
	lv_display_t *disp = lv_linux_drm_create();

	/* Open card0 and pick the first connected connector.  Passing -1 as
	 * connector_id is the "wildcard / first found" sentinel defined in
	 * lv_linux_drm.h.  The function queries the KMS modesetting API,
	 * picks the preferred mode of that connector (720x1280@60 on the
	 * RK055HDMIPI4MA0), allocates dumb BO framebuffers, and begins
	 * double-buffered scanout.
	 *
	 * connector_id is int64_t in the LVGL 9.1 API (verified against
	 * src/drivers/display/drm/lv_linux_drm.h at tag v9.1.0). */
	lv_linux_drm_set_file(disp, DRM_CARD, -1);

	/* ── Touch input (optional) ── */

	/* access(R_OK) probes the node without opening it -- avoids a hard
	 * error on units where the GD32 I2C-proxy hasn't landed yet.  The
	 * demo is fully usable display-only; touch is additive. */
	if (access(TOUCH_EVDEV, R_OK) == 0) {
		/* lv_evdev_create() opens the evdev node, spawns an internal
		 * reader, and returns an lv_indev_t the LVGL input machinery
		 * polls on each lv_timer_handler() tick.  LV_INDEV_TYPE_POINTER
		 * maps ABS_X / ABS_Y / BTN_TOUCH to LVGL's pointer model. */
		lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER,
						    TOUCH_EVDEV);
		/* Bind the input device to our display so LVGL translates
		 * raw touch coordinates into the display's logical space. */
		lv_indev_set_display(touch, disp);
	}

	/* ── Build the UI ── */

	/* Widgets created here appear on lv_screen_active(), which LVGL
	 * already bound to `disp` when the DRM backend registered itself. */
	make_dashboard();

	/* ── Main loop ── */

	/* lv_timer_handler() drives LVGL's internal timers (animations,
	 * input polling, display flush).  It returns the number of
	 * milliseconds until the next timer fires -- sleeping that long
	 * prevents busy-waiting without missing deadlines.
	 *
	 * Return type is uint32_t in LVGL 9.1 (verified against
	 * src/misc/lv_timer.h at tag v9.1.0).  usleep takes microseconds,
	 * hence the *1000 conversion. */
	while (1) {
		uint32_t idle_ms = lv_timer_handler();
		usleep(idle_ms * 1000);
	}

	return 0; /* unreachable */
}
