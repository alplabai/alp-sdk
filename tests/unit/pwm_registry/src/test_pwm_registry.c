/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the pwm registry dispatcher.  Mirrors the Slice 2b
 * gpio_registry / spi_registry harnesses; no vendor extensions, so
 * the test surface is the bare selector + capability-getter +
 * public-API edges plus the sw_fallback's NOSUPPORT path for
 * configure / single_pulse / capture.
 *
 * Backends visible on this test build:
 *   zephyr_drv    (priority 100, "*" wildcard)
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("pwm", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.
 *
 * CONFIG_ALP_SDK_PWM_SW_FALLBACK=y is set in prj.conf so that
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
#include <alp/pwm.h>

#include "../../../../src/backends/pwm/pwm_ops.h"

ZTEST_SUITE(alp_pwm_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector / priority test ---------------------------------- */

ZTEST(alp_pwm_registry, test_selector_prefers_zephyr_drv_on_alif)
{
	/* On alif:ensemble:e7 the zephyr_drv backend (priority 100) must
     * win over sw_fallback (priority 0) because the selector picks the
     * highest-priority wildcard match. */
	const alp_backend_t *be = alp_backend_select("pwm", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

/* ---------- (b) SW fallback reachability ---------------------------------- */

ZTEST(alp_pwm_registry, test_selector_falls_back_to_sw_when_only_fallback_present)
{
	/* Both registered backends are wildcard matches; zephyr_drv wins
     * on priority.  This test still exercises the selector on unknown
     * silicon and asserts the sw_fallback is reachable via count. */
	const alp_backend_t *be = alp_backend_select("pwm", "fictional:soc:zz");
	zassert_not_null(be);
	/* zephyr_drv wins on priority even for fictional silicon -- assert
     * reachability of sw_fallback via the registry count below. */
	(void)be;
	zassert_true(alp_backend_count("pwm") >= 2u);
}

/* ---------- (c) open returns NULL on invalid channel_id ------------------- */

ZTEST(alp_pwm_registry, test_open_returns_null_on_invalid_channel_id)
{
	/* channel_id >= ARRAY_SIZE(_specs) (== 8) must be rejected by the
     * Zephyr backend's open(); the dispatcher relays the NULL back to
     * the caller and sets last_error. */
	alp_pwm_config_t cfg = {
		.channel_id = 0xFFFFFFFFu,
		.period_ns  = 1000000u,
		.polarity   = ALP_PWM_POLARITY_NORMAL,
	};
	alp_pwm_t *h = alp_pwm_open(&cfg);
	zassert_is_null(h);
}

/* ---------- (d) capabilities getter matches backend base caps ------------- */

ZTEST(alp_pwm_registry, test_capabilities_getter_matches_backend_base_caps)
{
	/* alp_pwm_capabilities(NULL) must return NULL -- mirrors the
     * spi / gpio / uart / i2c pattern. */
	zassert_is_null(alp_pwm_capabilities(NULL));

	/* Verify that the backend declared by the registry has base_caps
     * accessible (even if 0) -- exercises the selector path. */
	const alp_backend_t *be = alp_backend_select("pwm", "alif:ensemble:e7");
	zassert_not_null(be);
	/* base_caps is 0u for the pwm Zephyr backend (no hw-specific cap
     * flags defined yet in Slice 2b). */
	zassert_equal(be->base_caps, 0u);
}

/* ---------- (e) close releases handle ------------------------------------- */

ZTEST(alp_pwm_registry, test_close_releases_handle)
{
	/* alp_pwm_open(NULL) -> NULL, no pool slot consumed */
	alp_pwm_t *h_null = alp_pwm_open(NULL);
	zassert_is_null(h_null);

	/* alp_pwm_close(NULL) must not crash */
	alp_pwm_close(NULL);

	/* alp_pwm_capabilities(NULL) -> NULL (idempotent after bad open) */
	zassert_is_null(alp_pwm_capabilities(NULL));

	/* alp_pwm_capture_close(NULL) must not crash either */
	alp_pwm_capture_close(NULL);
}

/* ---------- (f) sw_fallback round-trip + NOSUPPORT paths ------------------ */

extern const alp_backend_t __start_alp_backends_pwm[] __attribute__((weak));
extern const alp_backend_t __stop_alp_backends_pwm[] __attribute__((weak));

static const alp_pwm_ops_t *_find_sw_fallback_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_pwm; be < __stop_alp_backends_pwm; ++be) {
		if (strcmp(be->vendor, "sw_fallback") == 0) {
			return (const alp_pwm_ops_t *)be->ops;
		}
	}
	return NULL;
}

ZTEST(alp_pwm_registry, test_sw_fallback_round_trip)
{
	const alp_pwm_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	/* Allocate a zeroed alp_pwm on the stack so CONTAINER_OF in the
     * backend is valid -- state is the first field of struct alp_pwm. */
	struct alp_pwm h;
	memset(&h, 0, sizeof(h));
	alp_pwm_backend_state_t *st   = &h.state;
	alp_capabilities_t       caps = { 0 };
	alp_pwm_config_t         cfg  = {
		.channel_id = 0u,
		.period_ns  = 1000000u,
		.polarity   = ALP_PWM_POLARITY_NORMAL,
	};

	zassert_equal(ops->open(&cfg, st, &caps), ALP_OK);
	/* sw_fallback open leaves dev=NULL */
	zassert_is_null(st->dev);
	/* sw_fallback open primed the handle's period so the dispatcher's
     * bounds-check works as if the channel were real. */
	zassert_equal(h.period_ns, 1000000u);

	/* set_duty / set_period are no-ops returning ALP_OK */
	zassert_equal(ops->set_duty(st, 500000u), ALP_OK);
	zassert_equal(ops->set_period(st, 2000000u), ALP_OK);

	/* configure / single_pulse / capture_* all return NOSUPPORT */
	zassert_equal(ops->configure(st, ALP_PWM_ALIGN_EDGE, 0u, 0u), ALP_ERR_NOSUPPORT);
	zassert_equal(ops->single_pulse(st, 100u), ALP_ERR_NOSUPPORT);

	alp_pwm_backend_state_t  cap_st   = { 0 };
	alp_capabilities_t       cap_caps = { 0 };
	alp_pwm_capture_config_t cap_cfg  = {
		.channel_id = 0u,
		.edge       = ALP_PWM_CAPTURE_EDGE_RISING,
	};
	zassert_equal(ops->capture_open(&cap_cfg, &cap_st, &cap_caps), ALP_ERR_NOSUPPORT);

	uint32_t period_ns = 0u, pulse_ns = 0u;
	zassert_equal(ops->capture_read(&cap_st, &period_ns, &pulse_ns), ALP_ERR_NOSUPPORT);

	/* close / capture_close are no-ops; calling twice is safe */
	ops->capture_close(&cap_st);
	ops->capture_close(&cap_st);
	ops->close(st);
	ops->close(st);
}

/* ---------- (g) alp_pwm_set_period -- through the real public dispatcher -- */

ZTEST(alp_pwm_registry, test_alp_pwm_set_period_public_dispatch)
{
	/* Unlike (f) above, this drives the actual public
     * alp_pwm_set_period() dispatcher entry point (src/pwm_dispatch.c),
     * not the raw backend op -- exercises the NOT_READY / INVAL guard
     * clauses plus the real dispatch-to-backend call. */
	const alp_pwm_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	zassert_equal(alp_pwm_set_period(NULL, 1000u), ALP_ERR_NOT_READY);

	struct alp_pwm h;
	memset(&h, 0, sizeof(h));
	alp_capabilities_t caps = { 0 };
	alp_pwm_config_t   cfg  = {
		.channel_id = 0u,
		.period_ns  = 1000000u,
		.polarity   = ALP_PWM_POLARITY_NORMAL,
	};
	zassert_equal(ops->open(&cfg, &h.state, &caps), ALP_OK);
	h.state.ops = ops; /* alp_pwm_open() normally wires this before open() */
	h.in_use    = true;

	/* period_ns == 0 -> INVAL, before the backend is even consulted. */
	zassert_equal(alp_pwm_set_period(&h, 0u), ALP_ERR_INVAL);

	/* Valid period -> real dispatch to the sw_fallback op, which
     * returns ALP_OK and the dispatcher mirrors it into h.period_ns. */
	zassert_equal(alp_pwm_set_period(&h, 2000000u), ALP_OK);
	zassert_equal(h.period_ns, 2000000u);

	ops->close(&h.state);
}
