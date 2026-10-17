/* SPDX-License-Identifier: Apache-2.0
 *
 * CONFIG_ALP_INFERENCE_MARKER: the `alp-inference-marker` pin is high
 * while a backend's invoke() runs and low before and after, on both the
 * success and the error path.  A fabricated handle carries an invoke()
 * that samples the emulated pin.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/ztest.h>

#include <alp/inference.h>

#include "../../../../src/backends/inference/inference_ops.h"
#include "alp_slot_claim.h"

static const struct gpio_dt_spec marker = GPIO_DT_SPEC_GET(DT_ALIAS(alp_inference_marker), gpios);

static int seen_during_invoke = -1;

static alp_status_t invoke_ok(alp_inference_backend_state_t *state)
{
	(void)state;
	seen_during_invoke = gpio_pin_get_dt(&marker);
	return ALP_OK;
}

static alp_status_t invoke_err(alp_inference_backend_state_t *state)
{
	(void)state;
	seen_during_invoke = gpio_pin_get_dt(&marker);
	return ALP_ERR_IO;
}

static const alp_inference_ops_t ops_ok  = { .invoke = invoke_ok };
static const alp_inference_ops_t ops_err = { .invoke = invoke_err };

/* gpio-emul only reflects a driven level on a pin that is also an input;
 * re-configure the marker pin as both, still parked inactive. */
static void before(void *fixture)
{
	(void)fixture;
	zassert_ok(gpio_pin_configure_dt(&marker, GPIO_OUTPUT_INACTIVE | GPIO_INPUT));
}

ZTEST_SUITE(alp_inference_marker, NULL, NULL, before, NULL, NULL);

static void check(const alp_inference_ops_t *ops, alp_status_t want)
{
	struct alp_inference fake = {
		.state                  = { .ops = ops },
		.lifecycle              = ALP_HANDLE_LC_OPEN,
		.last_invoke_latency_us = UINT32_MAX,
		.in_use                 = true,
	};
	seen_during_invoke = -1;
	zassert_equal(gpio_pin_get_dt(&marker), 0, "marker must idle low");
	zassert_equal(alp_inference_invoke(&fake), want);
	zassert_equal(seen_during_invoke, 1, "marker must be high inside invoke()");
	zassert_equal(gpio_pin_get_dt(&marker), 0, "marker must drop after invoke()");
}

ZTEST(alp_inference_marker, test_high_during_invoke_success)
{
	check(&ops_ok, ALP_OK);
}

ZTEST(alp_inference_marker, test_low_after_invoke_error)
{
	check(&ops_err, ALP_ERR_IO);
}
