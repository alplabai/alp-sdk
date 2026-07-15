/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Camera registry dispatcher.  Extended in Slice 5
 * (real backend bodies + vendor exts) from the stub-only parent
 * harness.
 *
 * Backends visible on this test build:
 *   zephyr_stub      (priority 0,   "*" wildcard, vendor "stub")
 *
 * The real zephyr_video backend (priority 50 "*") and v2n_n44_isp
 * (priority 100 "renesas:rzv2n:n44") are NOT linked into this
 * native_sim test build -- they depend on CONFIG_VIDEO which the
 * native_sim Zephyr image does not ship by default.  The dispatcher,
 * vendor-ext gating, and selector code paths are exercised
 * directly via the .alp_backends_camera section walk + the public
 * vendor-ext entry points (NULL-handle gates and the non-vendor
 * gate against a fabricated handle pinned to "stub").
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("camera", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/ext/alif/camera.h>
#include <alp/ext/renesas/camera.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/camera/camera_ops.h"

ZTEST_SUITE(alp_camera_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_camera_registry, test_stub_picked_for_alif_e7)
{
	/* The wildcard stub is the only camera backend linked on this
     * test build.  Any silicon_ref resolves to it. */
	const alp_backend_t *be = alp_backend_select("camera", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "stub"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_camera_registry, test_stub_picked_for_unknown_silicon)
{
	const alp_backend_t *be = alp_backend_select("camera", "fictional:soc:zz");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "stub"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_camera_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_camera_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("camera", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_camera_registry, test_camera_open_returns_null_on_null_cfg)
{
	/* Dispatcher rejects NULL config at the front door before any
     * backend gets called. */
	alp_camera_t *h = alp_camera_open(NULL);
	zassert_is_null(h);
}

ZTEST(alp_camera_registry, test_camera_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_camera_capabilities(NULL));
}

ZTEST(alp_camera_registry, test_camera_configure_isp_rejects_null_isp)
{
	/* Dispatcher's INVAL gate fires on NULL isp ahead of backend
     * dispatch. */
	zassert_equal(alp_camera_configure_isp(NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_camera_registry, test_camera_capture_release_close_null_safe)
{
	alp_camera_frame_t frame = { .data = NULL };
	/* Every op on a NULL handle returns NOT_READY.  Close on NULL
     * is a documented no-op. */
	zassert_equal(alp_camera_start(NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_camera_stop(NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_camera_capture(NULL, &frame, 0), ALP_ERR_NOT_READY);
	zassert_equal(alp_camera_release(NULL, &frame), ALP_ERR_NOT_READY);
	alp_camera_close(NULL);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_camera_registry, test_backend_count_for_camera)
{
	/* Only zephyr_stub linked on this build; zephyr_video +
     * v2n_n44_isp depend on CONFIG_VIDEO which native_sim doesn't
     * ship.  When CONFIG_VIDEO arrives the count climbs to 2 or 3
     * and this assertion gets a follow-up. */
	zassert_equal(alp_backend_count("camera"), 1u);
}

/* ---------- Vendor-ext NULL-handle gate ---------------------------- */

ZTEST(alp_camera_registry, test_renesas_vendor_ext_returns_inval_on_null_handle)
{
	alp_renesas_camera_rect_t rect      = { .x = 0, .y = 0, .w = 320, .h = 240 };
	uint16_t                  table[16] = { 0 };

	zassert_equal(alp_renesas_camera_isp_3a_window_set(NULL, ALP_RENESAS_CAMERA_3A_AE, &rect),
	              ALP_ERR_INVAL);
	zassert_equal(
	    alp_renesas_camera_isp_gain_table_load(NULL, ALP_RENESAS_CAMERA_CHANNEL_R, table, 16u),
	    ALP_ERR_INVAL);
	zassert_equal(alp_renesas_camera_isp_lsc_lut_load(NULL, table, 64u), ALP_ERR_INVAL);
}

ZTEST(alp_camera_registry, test_alif_vendor_ext_returns_inval_on_null_handle)
{
	alp_alif_camera_rect_t rect      = { .x = 0, .y = 0, .w = 320, .h = 240 };
	uint16_t               table[16] = { 0 };

	zassert_equal(alp_alif_camera_isp_3a_window_set(NULL, ALP_ALIF_CAMERA_3A_AE, &rect),
	              ALP_ERR_INVAL);
	zassert_equal(alp_alif_camera_isp_gain_table_load(NULL, ALP_ALIF_CAMERA_CHANNEL_R, table, 16u),
	              ALP_ERR_INVAL);
	zassert_equal(alp_alif_camera_isp_lsc_lut_load(NULL, table, 64u), ALP_ERR_INVAL);
}

ZTEST(alp_camera_registry, test_vendor_ext_null_rect_lut_table_rejected)
{
	/* The NULL-pointer guards fire before the vendor-handle gate
     * since they're cheaper checks.  Fabricate any non-NULL handle
     * (its backend is never looked at). */
	struct alp_camera fake = {
		.in_use  = true,
		.backend = NULL,
	};

	zassert_equal(alp_renesas_camera_isp_3a_window_set(&fake, ALP_RENESAS_CAMERA_3A_AE, NULL),
	              ALP_ERR_INVAL);
	zassert_equal(
	    alp_renesas_camera_isp_gain_table_load(&fake, ALP_RENESAS_CAMERA_CHANNEL_R, NULL, 32u),
	    ALP_ERR_INVAL);
	zassert_equal(alp_renesas_camera_isp_lsc_lut_load(&fake, NULL, 64u), ALP_ERR_INVAL);

	zassert_equal(alp_alif_camera_isp_3a_window_set(&fake, ALP_ALIF_CAMERA_3A_AE, NULL),
	              ALP_ERR_INVAL);
	zassert_equal(alp_alif_camera_isp_gain_table_load(&fake, ALP_ALIF_CAMERA_CHANNEL_R, NULL, 32u),
	              ALP_ERR_INVAL);
	zassert_equal(alp_alif_camera_isp_lsc_lut_load(&fake, NULL, 64u), ALP_ERR_INVAL);
}

/* ---------- Vendor-ext non-matching-backend gate ------------------- */

/* Fabricate a backend descriptor pinned to a fictional vendor +
 * a handle that references it.  Verifies the vendor-handle gate
 * fires NOT_PRESENT_ON_THIS_SOC before any HAL code runs.
 *
 * Mirrors the storage_registry / power_registry vendor-ext gate
 * tests -- the fabricated backend exercises the "string mismatch"
 * branch of _is_renesas_backend / _is_alif_backend without
 * requiring the matching backend body to be linked in. */

static const alp_backend_t _fake_other_be = {
	.silicon_ref = "fictional:soc:zz",
	.vendor      = "fictional",
	.base_caps   = 0u,
	.priority    = 0,
	.ops         = NULL,
	.probe       = NULL,
};

ZTEST(alp_camera_registry, test_renesas_vendor_ext_rejects_non_renesas_backend)
{
	struct alp_camera fake = {
		.in_use  = true,
		.backend = &_fake_other_be,
	};
	alp_renesas_camera_rect_t rect      = { .x = 0, .y = 0, .w = 320, .h = 240 };
	uint16_t                  table[16] = { 0 };

	zassert_equal(alp_renesas_camera_isp_3a_window_set(&fake, ALP_RENESAS_CAMERA_3A_AE, &rect),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(
	    alp_renesas_camera_isp_gain_table_load(&fake, ALP_RENESAS_CAMERA_CHANNEL_R, table, 16u),
	    ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(alp_renesas_camera_isp_lsc_lut_load(&fake, table, 64u),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

ZTEST(alp_camera_registry, test_alif_vendor_ext_rejects_non_alif_backend)
{
	struct alp_camera fake = {
		.in_use  = true,
		.backend = &_fake_other_be,
	};
	alp_alif_camera_rect_t rect      = { .x = 0, .y = 0, .w = 320, .h = 240 };
	uint16_t               table[16] = { 0 };

	zassert_equal(alp_alif_camera_isp_3a_window_set(&fake, ALP_ALIF_CAMERA_3A_AE, &rect),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(alp_alif_camera_isp_gain_table_load(&fake, ALP_ALIF_CAMERA_CHANNEL_R, table, 16u),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(alp_alif_camera_isp_lsc_lut_load(&fake, table, 64u),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

/* ---------- Vendor-ext input range validation --------------------- */

/* Fabricate a handle pinned to the Renesas vendor so the
 * vendor-handle gate passes; this exercises the input-range
 * checks (zero-sized rect / LUT length bounds) ahead of the
 * be_data NOT_READY return (since the fake handle has no
 * backend state). */
static const alp_backend_t _fake_renesas_be = {
	.silicon_ref = "renesas:rzv2n:n44",
	.vendor      = "renesas",
	.base_caps   = 0u,
	.priority    = 100,
	.ops         = NULL,
	.probe       = NULL,
};

ZTEST(alp_camera_registry, test_renesas_vendor_ext_validates_input_ranges)
{
	struct alp_camera fake = {
		.in_use  = true,
		.backend = &_fake_renesas_be,
	};
	/* state.be_data stays NULL -- after a successful vendor gate
     * the body checks NOT_READY and returns it.  The range
     * checks below run BEFORE the NOT_READY return. */

	/* Zero-sized rectangle is rejected. */
	alp_renesas_camera_rect_t bad_rect = { .x = 0, .y = 0, .w = 0, .h = 200 };
	zassert_equal(alp_renesas_camera_isp_3a_window_set(&fake, ALP_RENESAS_CAMERA_3A_AE, &bad_rect),
	              ALP_ERR_INVAL);

	/* Channel enum out-of-range. */
	uint16_t table[16] = { 0 };
	zassert_equal(
	    alp_renesas_camera_isp_gain_table_load(&fake, (alp_renesas_camera_channel_t)99, table, 16u),
	    ALP_ERR_INVAL);

	/* Gain-table length below 16. */
	zassert_equal(
	    alp_renesas_camera_isp_gain_table_load(&fake, ALP_RENESAS_CAMERA_CHANNEL_R, table, 8u),
	    ALP_ERR_INVAL);

	/* Gain-table length above 1024. */
	zassert_equal(
	    alp_renesas_camera_isp_gain_table_load(&fake, ALP_RENESAS_CAMERA_CHANNEL_R, table, 2048u),
	    ALP_ERR_INVAL);

	/* LSC LUT bounds (64..4096). */
	zassert_equal(alp_renesas_camera_isp_lsc_lut_load(&fake, table, 32u), ALP_ERR_INVAL);
	zassert_equal(alp_renesas_camera_isp_lsc_lut_load(&fake, table, 8192u), ALP_ERR_INVAL);

	/* A valid call (rect + length in range) reaches the
     * NOT_READY return because the fake handle has no state.
     * This proves the validation checks pass cleanly through
     * the gate. */
	alp_renesas_camera_rect_t ok_rect = { .x = 10, .y = 10, .w = 100, .h = 100 };
	zassert_equal(alp_renesas_camera_isp_3a_window_set(&fake, ALP_RENESAS_CAMERA_3A_AE, &ok_rect),
	              ALP_ERR_NOT_READY);
}

/* Same input-range battery against the Alif surface (which
 * returns NOSUPPORT at the tail rather than ALP_OK -- but the
 * gating shape is identical). */
static const alp_backend_t _fake_alif_be = {
	.silicon_ref = "alif:ensemble:e8",
	.vendor      = "alif",
	.base_caps   = 0u,
	.priority    = 100,
	.ops         = NULL,
	.probe       = NULL,
};

ZTEST(alp_camera_registry, test_alif_vendor_ext_validates_input_ranges_then_nosupport)
{
	struct alp_camera fake = {
		.in_use  = true,
		.backend = &_fake_alif_be,
	};
	uint16_t table[16] = { 0 };

	/* Zero-sized rectangle still rejected. */
	alp_alif_camera_rect_t bad_rect = { .x = 0, .y = 0, .w = 0, .h = 200 };
	zassert_equal(alp_alif_camera_isp_3a_window_set(&fake, ALP_ALIF_CAMERA_3A_AE, &bad_rect),
	              ALP_ERR_INVAL);

	/* Length out of range still rejected. */
	zassert_equal(alp_alif_camera_isp_gain_table_load(&fake, ALP_ALIF_CAMERA_CHANNEL_R, table, 8u),
	              ALP_ERR_INVAL);
	zassert_equal(alp_alif_camera_isp_lsc_lut_load(&fake, table, 32u), ALP_ERR_INVAL);

	/* In-range arguments fall through to the NOSUPPORT tail -- not
     * because a HAL pack is missing (the isp_wrapper archive is
     * already vendored in hal_alif) but because AE is declared-but-undefined in
     * it, the gain-table contract can't be satisfied by the archive's
     * struct, and LSC is absent from it outright; see the per-entry
     * detail in src/backends/ext/alif/camera.c. */
	alp_alif_camera_rect_t ok_rect = { .x = 10, .y = 10, .w = 100, .h = 100 };
	zassert_equal(alp_alif_camera_isp_3a_window_set(&fake, ALP_ALIF_CAMERA_3A_AE, &ok_rect),
	              ALP_ERR_NOSUPPORT);
	zassert_equal(alp_alif_camera_isp_gain_table_load(&fake, ALP_ALIF_CAMERA_CHANNEL_R, table, 16u),
	              ALP_ERR_NOSUPPORT);
	zassert_equal(alp_alif_camera_isp_lsc_lut_load(&fake, table, 64u), ALP_ERR_NOSUPPORT);
}
