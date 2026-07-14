/* SPDX-License-Identifier: Apache-2.0
 *
 * ZTESTs for alp_gui_lvgl_attach() -- the <alp/gui.h> LVGL v9 bridge
 * (issue #23).  Two twister scenarios, split at compile time:
 *
 *   - alp_sdk.gui.lvgl_attach            -- CONFIG_LVGL=y.  Registers a
 *     local priority-255 test-double display backend (the
 *     ALP_BACKEND_REGISTER test-double convention documented on
 *     <alp/backend.h>, precedented by tests/zephyr/security_fallback/'s
 *     fake_hw/fake_sw) and proves the real flush_cb path reaches
 *     alp_display_blit() once LVGL renders a frame.  Also proves the
 *     unsupported-pixel-format (MONO_VLSB) rejection.
 *   - alp_sdk.gui.lvgl_attach_nosupport  -- CONFIG_LVGL=n.  Confirms the
 *     guard-clause degrade (NOSUPPORT) still holds when no build wires
 *     LVGL in.
 *
 * NULL-rejection (ALP_ERR_INVAL) is common to both -- alp_gui_lvgl_attach()
 * checks it before ever branching on ALP_HAS_LVGL.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include <alp/display.h>
#include <alp/gui.h>
#include <alp/peripheral.h>

ZTEST_SUITE(alp_gui_lvgl, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_gui_lvgl, test_attach_null_rejected)
{
	zassert_equal(alp_gui_lvgl_attach(NULL), ALP_ERR_INVAL);
}

#ifdef CONFIG_LVGL

#include <lvgl.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>

#include "../../../../src/backends/display/display_ops.h"

/* ---------------------------------------------------------------------
 * Local priority-255 test-double display backend -- outranks every real
 * backend (see the priority note on ALP_BACKEND_REGISTER in
 * <alp/backend.h>).  display_id 0 reports a small RGB565 panel and
 * counts blits, so the test can prove the LVGL flush_cb path actually
 * reaches alp_display_blit() without any real/emulated display
 * controller; display_id 1 reports MONO_VLSB, to exercise
 * alp_gui_lvgl_attach()'s unsupported-format rejection.
 * --------------------------------------------------------------------- */

#define TD_WIDTH  32
#define TD_HEIGHT 16

static uint32_t g_blit_count;
static uint16_t g_last_w;
static uint16_t g_last_h;

static alp_status_t td_open(const alp_display_config_t  *cfg,
                            alp_display_backend_state_t *st,
                            alp_capabilities_t          *caps_out)
{
	(void)cfg;
	(void)caps_out;
	st->be_data = NULL;
	return ALP_OK;
}

static alp_status_t td_get_caps(alp_display_backend_state_t *st, alp_display_caps_t *out)
{
	out->width  = TD_WIDTH;
	out->height = TD_HEIGHT;
	out->format = (st->display_id == 1) ? ALP_PIXFMT_MONO_VLSB : ALP_PIXFMT_RGB565;
	return ALP_OK;
}

static alp_status_t td_blit(alp_display_backend_state_t *st,
                            uint16_t                     x,
                            uint16_t                     y,
                            uint16_t                     w,
                            uint16_t                     h,
                            const void                  *pixels)
{
	(void)st;
	(void)x;
	(void)y;
	(void)pixels;
	g_blit_count++;
	g_last_w = w;
	g_last_h = h;
	return ALP_OK;
}

static alp_status_t td_clear(alp_display_backend_state_t *st)
{
	(void)st;
	return ALP_OK;
}

static void td_close(alp_display_backend_state_t *st)
{
	(void)st;
}

static const alp_display_ops_t _td_ops = {
	.open     = td_open,
	.get_caps = td_get_caps,
	.blit     = td_blit,
	.clear    = td_clear,
	.close    = td_close,
};

ALP_BACKEND_REGISTER(display,
                     alp_gui_lvgl_test,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_gui_test",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_td_ops,
                         .probe       = NULL,
                     });

ZTEST(alp_gui_lvgl, test_attach_ok_and_flush_reaches_blit)
{
	g_blit_count = 0;
	g_last_w     = 0;
	g_last_h     = 0;

	const alp_display_config_t cfg = { .display_id = 0 };
	alp_display_t             *d   = alp_display_open(&cfg);
	zassert_not_null(d, "open failed: last_error=%d", alp_last_error());

	/* Single ZTEST_SUITE in this image -- lv_init() runs exactly once,
	 * before any lv_display_create()/lv_obj_create() call. */
	lv_init();

	zassert_equal(alp_gui_lvgl_attach(d), ALP_OK);

	lv_obj_t *label = lv_label_create(lv_screen_active());
	lv_label_set_text(label, "alp");

	/* Force a synchronous render + flush cycle -- lv_refr_now() drives
	 * the dirty area straight through to our flush_cb. */
	lv_refr_now(NULL);

	zassert_true(g_blit_count > 0, "flush_cb never reached alp_display_blit");
	zassert_true(g_last_w > 0 && g_last_h > 0, "flush rect had zero area");

	alp_display_close(d);
}

ZTEST(alp_gui_lvgl, test_attach_rejects_unsupported_pixfmt)
{
	/* display_id 1 -> the test double reports MONO_VLSB, which has no
	 * LVGL v9 equivalent (see src/gui_lvgl.c's _map_pixfmt comment). */
	const alp_display_config_t cfg = { .display_id = 1 };
	alp_display_t             *d   = alp_display_open(&cfg);
	zassert_not_null(d, "open failed: last_error=%d", alp_last_error());

	zassert_equal(alp_gui_lvgl_attach(d), ALP_ERR_NOSUPPORT);

	alp_display_close(d);
}

#else /* !CONFIG_LVGL */

ZTEST(alp_gui_lvgl, test_attach_degrades_to_nosupport_without_lvgl)
{
	/* The guard clause (src/gui_lvgl.c's #else body) never dereferences
	 * a non-NULL display -- it only branches on NULL vs not -- so a
	 * fake handle exercises the contract without needing a real open().
	 * (The test-double backend above is compiled out in this
	 * CONFIG_LVGL=n build, and zephyr_drv/zephyr_stub's own open()
	 * paths don't hand back a usable handle here either, so a fake
	 * handle is the simplest correct fixture.)  Mirrors
	 * tests/zephyr/chips/src/test_core_surfaces.c's identical
	 * guard-clause test. */
	alp_display_t *fake_handle = (alp_display_t *)0x1;
	zassert_equal(alp_gui_lvgl_attach(fake_handle), ALP_ERR_NOSUPPORT);
}

#endif /* CONFIG_LVGL */
