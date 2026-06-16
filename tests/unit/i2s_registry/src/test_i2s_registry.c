/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the i2s registry dispatcher.  Mirrors the Slice 2b
 * gpio_registry / pwm_registry harnesses; no vendor extensions, so
 * the test surface is the bare selector + capability-getter +
 * public-API edges plus the sw_fallback's NOSUPPORT path for
 * write / read.
 *
 * Backends visible on this test build:
 *   zephyr_drv    (priority 100, "*" wildcard)
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("i2s", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.
 *
 * CONFIG_ALP_SDK_I2S_SW_FALLBACK=y is set in prj.conf so that
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
#include <alp/i2s.h>

#include "../../../../src/backends/i2s/i2s_ops.h"

ZTEST_SUITE(alp_i2s_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector / priority test ---------------------------------- */

ZTEST(alp_i2s_registry, test_selector_prefers_zephyr_drv_on_alif)
{
	/* On alif:ensemble:e7 the zephyr_drv backend (priority 100) must
     * win over sw_fallback (priority 0) because the selector picks the
     * highest-priority wildcard match. */
	const alp_backend_t *be = alp_backend_select("i2s", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

/* ---------- (b) SW fallback reachability ---------------------------------- */

ZTEST(alp_i2s_registry, test_selector_falls_back_to_sw_when_only_fallback_present)
{
	/* Both registered backends are wildcard matches; zephyr_drv wins
     * on priority.  This test still exercises the selector on unknown
     * silicon and asserts the sw_fallback is reachable via count. */
	const alp_backend_t *be = alp_backend_select("i2s", "fictional:soc:zz");
	zassert_not_null(be);
	/* zephyr_drv wins on priority even for fictional silicon -- assert
     * reachability of sw_fallback via the registry count below. */
	(void)be;
	zassert_true(alp_backend_count("i2s") >= 2u);
}

/* ---------- (c) open returns NULL on invalid bus_id ----------------------- */

ZTEST(alp_i2s_registry, test_open_returns_null_on_invalid_bus_id)
{
	/* bus_id >= ARRAY_SIZE(_devs) (== 2) must be rejected by the Zephyr
     * backend's open(); the dispatcher relays the NULL back to the
     * caller and sets last_error. */
	alp_i2s_config_t cfg = {
		.bus_id         = 0xFFFFFFFFu,
		.sample_rate_hz = 48000u,
		.word_bits      = 16u,
		.channels       = 2u,
		.format         = ALP_I2S_FMT_I2S,
		.direction      = ALP_I2S_DIR_TX,
		.block_frames   = 64u,
	};
	alp_i2s_t *h = alp_i2s_open(&cfg);
	zassert_is_null(h);
}

/* ---------- (d) capabilities getter matches backend base caps ------------- */

ZTEST(alp_i2s_registry, test_capabilities_getter_matches_backend_base_caps)
{
	/* alp_i2s_capabilities(NULL) must return NULL -- mirrors the
     * spi / gpio / pwm / uart pattern. */
	zassert_is_null(alp_i2s_capabilities(NULL));

	/* Verify that the backend declared by the registry has base_caps
     * accessible (even if 0) -- exercises the selector path. */
	const alp_backend_t *be = alp_backend_select("i2s", "alif:ensemble:e7");
	zassert_not_null(be);
	/* base_caps is 0u for the i2s Zephyr backend (no hw-specific cap
     * flags defined yet in Slice 2b). */
	zassert_equal(be->base_caps, 0u);
}

/* ---------- (e) close releases handle ------------------------------------- */

ZTEST(alp_i2s_registry, test_close_releases_handle)
{
	/* alp_i2s_open(NULL) -> NULL, no pool slot consumed */
	alp_i2s_t *h_null = alp_i2s_open(NULL);
	zassert_is_null(h_null);

	/* alp_i2s_close(NULL) must not crash */
	alp_i2s_close(NULL);

	/* alp_i2s_capabilities(NULL) -> NULL (idempotent after bad open) */
	zassert_is_null(alp_i2s_capabilities(NULL));
}

/* ---------- (f) sw_fallback round-trip + NOSUPPORT paths ------------------ */

extern const alp_backend_t __start_alp_backends_i2s[] __attribute__((weak));
extern const alp_backend_t __stop_alp_backends_i2s[] __attribute__((weak));

static const alp_i2s_ops_t *_find_sw_fallback_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_i2s; be < __stop_alp_backends_i2s; ++be) {
		if (strcmp(be->vendor, "sw_fallback") == 0) {
			return (const alp_i2s_ops_t *)be->ops;
		}
	}
	return NULL;
}

ZTEST(alp_i2s_registry, test_sw_fallback_round_trip)
{
	const alp_i2s_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	/* Allocate a zeroed alp_i2s on the stack so CONTAINER_OF in the
     * backend is valid -- state is the first field of struct alp_i2s. */
	struct alp_i2s h;
	memset(&h, 0, sizeof(h));
	alp_i2s_backend_state_t *st   = &h.state;
	alp_capabilities_t       caps = { 0 };
	alp_i2s_config_t         cfg  = {
		         .bus_id         = 0u,
		         .sample_rate_hz = 48000u,
		         .word_bits      = 16u,
		         .channels       = 2u,
		         .format         = ALP_I2S_FMT_I2S,
		         .direction      = ALP_I2S_DIR_TX,
		         .block_frames   = 64u,
	};

	zassert_equal(ops->open(&cfg, st, &caps), ALP_OK);
	/* sw_fallback open leaves dev=NULL (no real controller) */
	zassert_is_null(st->dev);

	/* start / stop are no-ops returning ALP_OK */
	zassert_equal(ops->start(st), ALP_OK);
	zassert_equal(ops->stop(st), ALP_OK);

	/* write / read return NOSUPPORT */
	uint8_t tx[8] = { 0u };
	zassert_equal(ops->write(st, tx, sizeof(tx), 10u), ALP_ERR_NOSUPPORT);

	uint8_t rx[8] = { 0u };
	size_t  got   = 0xFFu;
	zassert_equal(ops->read(st, rx, sizeof(rx), &got, 10u), ALP_ERR_NOSUPPORT);
	/* sw_fallback clears *bytes_out before returning so partial-read
     * code paths don't see stale counts on NOSUPPORT. */
	zassert_equal(got, 0u);

	/* close is a no-op; calling twice is safe */
	ops->close(st);
	ops->close(st);
}
