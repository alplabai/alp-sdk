/* SPDX-License-Identifier: Apache-2.0
 *
 * ZTESTs for the <alp/display.h> portable surface over the
 * zephyr_drv backend (issue #23).  Three twister scenarios share
 * this source, split at compile time:
 *
 *   - alp_sdk.display.zephyr_drv   -- alp-display0 alias backed by
 *     the upstream dummy display controller (zephyr,dummy-dc,
 *     320x240, ARGB8888 default): the real open / get_caps / blit /
 *     clear / close paths plus param validation.
 *   - alp_sdk.display.no_device    -- zephyr_drv compiled but no
 *     alp-displayN alias in DT: open degrades with NOT_READY.
 *   - alp_sdk.display.stub_fallback -- CONFIG_DISPLAY=n, so only
 *     the priority-0 stub registers: open degrades with
 *     NOT_IMPLEMENTED.
 *
 * The dummy driver's declared geometry (320x240) is mirrored in
 * overlays/dummy_display.overlay -- keep the two in sync.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#define HAVE_DISPLAY_NODE DT_NODE_EXISTS(DT_ALIAS(alp_display0))

ZTEST_SUITE(alp_display, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / registry inventory --------------------------- */

ZTEST(alp_display, test_backend_selection)
{
	const alp_backend_t *be = alp_backend_select("display", ALP_SOC_REF_STR);

	zassert_not_null(be);
#ifdef CONFIG_ALP_SDK_DISPLAY_ZEPHYR_DRV
	/* Real wrapper registered: wins over the stub on priority. */
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 50);
	zassert_equal(alp_backend_count("display"), 2u);
#else
	/* CONFIG_DISPLAY=n build: only the stub registers. */
	zassert_equal(strcmp(be->vendor, "stub"), 0);
	zassert_equal(be->priority, 0);
	zassert_equal(alp_backend_count("display"), 1u);
#endif
}

/* ---------- Front-door param validation (all scenarios) ------------- */

ZTEST(alp_display, test_open_null_cfg_rejected)
{
	zassert_is_null(alp_display_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_display, test_ops_on_null_handle)
{
	alp_display_caps_t caps;

	zassert_equal(alp_display_get_caps(NULL, &caps), ALP_ERR_NOT_READY);
	zassert_equal(alp_display_blit(NULL, 0, 0, 1, 1, &caps), ALP_ERR_NOT_READY);
	zassert_equal(alp_display_clear(NULL), ALP_ERR_NOT_READY);
	zassert_is_null(alp_display_capabilities(NULL));
	alp_display_close(NULL); /* must be a no-op, not a crash */
}

#if HAVE_DISPLAY_NODE

/* ---------- Real-backend path (dummy display present) ---------------- */

ZTEST(alp_display, test_open_and_close)
{
	const alp_display_config_t cfg = { .display_id = 0 };
	alp_display_t             *d   = alp_display_open(&cfg);

	zassert_not_null(d, "open failed: last_error=%d", alp_last_error());
	zassert_not_null(alp_display_capabilities(d));

	alp_display_close(d);

	/* Ops on a closed handle must refuse, not touch the driver. */
	alp_display_caps_t caps;
	zassert_equal(alp_display_get_caps(d, &caps), ALP_ERR_NOT_READY);
}

ZTEST(alp_display, test_get_caps_matches_dt)
{
	const alp_display_config_t cfg = { .display_id = 0 };
	alp_display_t             *d   = alp_display_open(&cfg);

	zassert_not_null(d);

	alp_display_caps_t caps;
	zassert_equal(alp_display_get_caps(d, &caps), ALP_OK);
	/* Geometry from overlays/dummy_display.overlay; format is the
	 * dummy driver's ARGB8888 default (representable, so open()
	 * keeps it). */
	zassert_equal(caps.width, 320);
	zassert_equal(caps.height, 240);
	zassert_equal(caps.format, ALP_PIXFMT_ARGB8888);

	zassert_equal(alp_display_get_caps(d, NULL), ALP_ERR_INVAL);

	alp_display_close(d);
}

ZTEST(alp_display, test_blit_ok_and_param_validation)
{
	const alp_display_config_t cfg = { .display_id = 0 };
	alp_display_t             *d   = alp_display_open(&cfg);

	zassert_not_null(d);

	/* 4x2 ARGB8888 patch = 32 bytes. */
	static const uint8_t patch[4 * 2 * 4];

	zassert_equal(alp_display_blit(d, 0, 0, 4, 2, patch), ALP_OK);
	/* Bottom-right corner still in bounds. */
	zassert_equal(alp_display_blit(d, 316, 238, 4, 2, patch), ALP_OK);

	/* NULL pixels rejected at the dispatcher front door. */
	zassert_equal(alp_display_blit(d, 0, 0, 4, 2, NULL), ALP_ERR_INVAL);
	/* Zero-sized rects are invalid. */
	zassert_equal(alp_display_blit(d, 0, 0, 0, 2, patch), ALP_ERR_INVAL);
	zassert_equal(alp_display_blit(d, 0, 0, 4, 0, patch), ALP_ERR_INVAL);
	/* Rects crossing the panel edge are out of range. */
	zassert_equal(alp_display_blit(d, 317, 0, 4, 2, patch), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(alp_display_blit(d, 0, 239, 4, 2, patch), ALP_ERR_OUT_OF_RANGE);

	alp_display_close(d);
}

ZTEST(alp_display, test_clear_via_sw_fallback)
{
	const alp_display_config_t cfg = { .display_id = 0 };
	alp_display_t             *d   = alp_display_open(&cfg);

	zassert_not_null(d);

	/* The dummy driver has no clear op (-ENOSYS), so this exercises
	 * the backend's chunked zero-fill display_write fallback across
	 * the full 320x240 panel. */
	zassert_equal(alp_display_clear(d), ALP_OK);

	alp_display_close(d);
}

ZTEST(alp_display, test_open_unresolved_and_out_of_range_ids)
{
	/* display_id 3 is a valid slot but has no DT alias. */
	const alp_display_config_t no_alias = { .display_id = 3 };
	zassert_is_null(alp_display_open(&no_alias));
	zassert_equal(alp_last_error(), ALP_ERR_NOT_READY);

	/* display_id beyond the alias table is INVAL. */
	const alp_display_config_t oob = { .display_id = 99 };
	zassert_is_null(alp_display_open(&oob));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

#elif defined(CONFIG_ALP_SDK_DISPLAY_ZEPHYR_DRV)

/* ---------- No-device degrade (zephyr_drv, no DT alias) -------------- */

ZTEST(alp_display, test_open_degrades_not_ready_without_node)
{
	const alp_display_config_t cfg = { .display_id = 0 };

	zassert_is_null(alp_display_open(&cfg));
	zassert_equal(alp_last_error(), ALP_ERR_NOT_READY);
}

#else

/* ---------- Stub degrade (CONFIG_DISPLAY=n) --------------------------- */

ZTEST(alp_display, test_open_degrades_not_implemented_on_stub)
{
	const alp_display_config_t cfg = { .display_id = 0 };

	zassert_is_null(alp_display_open(&cfg));
	zassert_equal(alp_last_error(), ALP_ERR_NOT_IMPLEMENTED);
}

#endif /* HAVE_DISPLAY_NODE */
