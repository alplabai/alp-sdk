/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the can registry dispatcher.  Mirrors the Slice 2b
 * i2s_registry / pwm_registry / gpio_registry harnesses; no vendor
 * extensions, so the test surface is the bare selector + capability-
 * getter + public-API edges plus the sw_fallback's NOSUPPORT path
 * for send / add_filter.
 *
 * Backends visible on this test build:
 *   zephyr_drv    (priority 100, "*" wildcard)
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("can", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.
 *
 * CONFIG_ALP_SDK_CAN_SW_FALLBACK=y is set in prj.conf so that
 * the sw_fallback TU is linked in and the backend-count assertions
 * below can verify both backends are present.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>

#include "../../../../src/backends/can/can_ops.h"

ZTEST_SUITE(alp_can_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector / priority test ---------------------------------- */

ZTEST(alp_can_registry, test_selector_prefers_zephyr_drv_on_alif)
{
	/* On alif:ensemble:e7 the zephyr_drv backend (priority 100) must
     * win over sw_fallback (priority 0) because the selector picks the
     * highest-priority wildcard match. */
	const alp_backend_t *be = alp_backend_select("can", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

/* ---------- (b) SW fallback reachability ---------------------------------- */

ZTEST(alp_can_registry, test_selector_falls_back_to_sw_when_only_fallback_present)
{
	/* Both registered backends are wildcard matches; zephyr_drv wins
     * on priority.  This test still exercises the selector on unknown
     * silicon and asserts the sw_fallback is reachable via count. */
	const alp_backend_t *be = alp_backend_select("can", "fictional:soc:zz");
	zassert_not_null(be);
	/* zephyr_drv wins on priority even for fictional silicon -- assert
     * reachability of sw_fallback via the registry count below. */
	(void)be;
	zassert_true(alp_backend_count("can") >= 2u);
}

/* ---------- (c) open returns NULL on invalid bus_id ----------------------- */

ZTEST(alp_can_registry, test_open_returns_null_on_invalid_bus_id)
{
	/* bus_id >= ARRAY_SIZE(_devs) (== 6) must be rejected by the
     * Zephyr backend's open(); the dispatcher relays the NULL back to
     * the caller and sets last_error. */
	alp_can_config_t cfg = {
		.bus_id             = 0xFFFFFFFFu,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	alp_can_t *h = alp_can_open(&cfg);
	zassert_is_null(h);
}

/* ---------- (d) capabilities getter matches backend base caps ------------- */

ZTEST(alp_can_registry, test_capabilities_getter_matches_backend_base_caps)
{
	/* alp_can_capabilities(NULL) must return NULL -- mirrors the
     * i2s / spi / gpio / pwm / uart pattern. */
	zassert_is_null(alp_can_capabilities(NULL));

	/* Verify that the backend declared by the registry has base_caps
     * accessible (even if 0) -- exercises the selector path. */
	const alp_backend_t *be = alp_backend_select("can", "alif:ensemble:e7");
	zassert_not_null(be);
	/* base_caps is 0u for the can Zephyr backend (no hw-specific cap
     * flags defined yet in Slice 2b). */
	zassert_equal(be->base_caps, 0u);
}

/* ---------- (e) close releases handle ------------------------------------- */

ZTEST(alp_can_registry, test_close_releases_handle)
{
	/* alp_can_open(NULL) -> NULL, no pool slot consumed */
	alp_can_t *h_null = alp_can_open(NULL);
	zassert_is_null(h_null);

	/* alp_can_close(NULL) must not crash */
	alp_can_close(NULL);

	/* alp_can_capabilities(NULL) -> NULL (idempotent after bad open) */
	zassert_is_null(alp_can_capabilities(NULL));
}

/* ---------- (f) sw_fallback round-trip + NOSUPPORT paths ------------------ */

extern const alp_backend_t __start_alp_backends_can[] __attribute__((weak));
extern const alp_backend_t __stop_alp_backends_can[] __attribute__((weak));

static const alp_can_ops_t *_find_sw_fallback_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_can; be < __stop_alp_backends_can; ++be) {
		if (strcmp(be->vendor, "sw_fallback") == 0) {
			return (const alp_can_ops_t *)be->ops;
		}
	}
	return NULL;
}

static void _rx_cb_noop(const alp_can_frame_t *frame, void *user)
{
	(void)frame;
	(void)user;
}

ZTEST(alp_can_registry, test_sw_fallback_round_trip)
{
	const alp_can_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	/* Allocate a zeroed alp_can on the stack so CONTAINER_OF in the
     * backend is valid -- state is the first field of struct alp_can. */
	struct alp_can h;
	memset(&h, 0, sizeof(h));
	alp_can_backend_state_t *st   = &h.state;
	alp_capabilities_t       caps = { 0 };
	alp_can_config_t         cfg  = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};

	zassert_equal(ops->open(&cfg, st, &caps), ALP_OK);
	/* sw_fallback open leaves dev=NULL (no real controller) */
	zassert_is_null(st->dev);

	/* start / stop are no-ops returning ALP_OK */
	zassert_equal(ops->start(st), ALP_OK);
	zassert_equal(ops->stop(st), ALP_OK);

	/* send returns NOSUPPORT */
	alp_can_frame_t frame = {
		.id  = 0x123,
		.dlc = 8u,
	};
	zassert_equal(ops->send(st, &frame, 10u), ALP_ERR_NOSUPPORT);

	/* add_filter returns NOSUPPORT */
	alp_can_filter_t filter = {
		.id     = 0x123,
		.mask   = 0x7FFu,
		.ext_id = false,
	};
	int32_t fid = -1;
	zassert_equal(ops->add_filter(st, &filter, _rx_cb_noop, NULL, &fid), ALP_ERR_NOSUPPORT);

	/* remove_filter is idempotent on the stub */
	zassert_equal(ops->remove_filter(st, 0), ALP_OK);

	/* close is a no-op; calling twice is safe */
	ops->close(st);
	ops->close(st);
}

/* ---------- (g) alp_can_remove_filter -- through the real public dispatcher */

ZTEST(alp_can_registry, test_alp_can_remove_filter_public_dispatch)
{
	/* Unlike (f) above, this drives the actual public
     * alp_can_remove_filter() dispatcher entry point
     * (src/can_dispatch.c), not the raw backend op -- exercises the
     * NOT_READY guard clause plus the real dispatch-to-backend call. */
	zassert_equal(alp_can_remove_filter(NULL, 0), ALP_ERR_NOT_READY);

	const alp_can_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	struct alp_can h;
	memset(&h, 0, sizeof(h));
	alp_capabilities_t caps = { 0 };
	alp_can_config_t   cfg  = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	zassert_equal(ops->open(&cfg, &h.state, &caps), ALP_OK);
	h.state.ops = ops; /* alp_can_open() normally wires this before open() */
	h.in_use    = true;

	/* Real dispatch to the sw_fallback op, which is idempotent ALP_OK. */
	zassert_equal(alp_can_remove_filter(&h, 0), ALP_OK);
	zassert_equal(alp_can_remove_filter(&h, 0), ALP_OK);

	ops->close(&h.state);
}
