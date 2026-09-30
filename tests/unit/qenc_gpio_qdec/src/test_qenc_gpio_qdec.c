/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the gpio-qdec input-subsystem QEnc backend (issue
 * #2095).  alp-qenc0 is aliased to a `gpio-qdec` node (see
 * boards/native_sim*.overlay) backed by native_sim's `gpio0` emul
 * controller.  Rather than toggling the emulated GPIO pins and
 * relying on the real gpio-qdec sampling state machine (a Gray-code
 * decode with its own idle/poll timing, not what this backend's
 * accumulation logic is being tested for), the test injects
 * INPUT_EV_REL events directly with input_report_rel() -- the exact
 * event shape the real driver posts once steps-per-period real phase
 * transitions accumulate (see zephyr/drivers/input/
 * input_gpio_qdec.c's gpio_qdec_event_worker()).  With
 * CONFIG_INPUT_MODE_SYNCHRONOUS=y the callback runs inline, so the
 * backend's accumulator is up to date the instant input_report_rel()
 * returns -- no k_msleep() needed.
 */

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/ztest.h>

#include <alp/counter.h>
#include <alp/peripheral.h>

#define QDEC_NODE DT_ALIAS(alp_qenc0)

ZTEST_SUITE(alp_qenc_gpio_qdec, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_qenc_gpio_qdec, test_open_selects_gpio_qdec_backend)
{
	const struct device *dev = DEVICE_DT_GET(QDEC_NODE);
	zassert_true(device_is_ready(dev), "gpio-qdec device not ready");

	alp_qenc_config_t cfg = ALP_QENC_CONFIG_DEFAULT(0);
	alp_qenc_t        *h  = alp_qenc_open(&cfg);
	zassert_not_null(h, "expected gpio_qdec backend to accept alp-qenc0");
	alp_qenc_close(h);
}

ZTEST(alp_qenc_gpio_qdec, test_position_accumulates_injected_rel_events)
{
	const struct device *dev = DEVICE_DT_GET(QDEC_NODE);

	alp_qenc_config_t cfg = ALP_QENC_CONFIG_DEFAULT(0);
	alp_qenc_t        *h  = alp_qenc_open(&cfg);
	zassert_not_null(h);

	/* Baseline: earlier tests in this suite may have already ticked
	 * the shared accumulator, so measure the delta rather than an
	 * absolute value. */
	int32_t before = 0;
	zassert_equal(alp_qenc_get_position(h, &before), ALP_OK);

	/* Three real gpio-qdec event postings: +1, +1, -1 detent (see
	 * gpio_qdec_event_worker() -- the value is the already-divided
	 * steps-per-period count, signed by direction). */
	zassert_equal(input_report_rel(dev, INPUT_REL_WHEEL, 1, true, K_NO_WAIT), 0);
	zassert_equal(input_report_rel(dev, INPUT_REL_WHEEL, 1, true, K_NO_WAIT), 0);
	zassert_equal(input_report_rel(dev, INPUT_REL_WHEEL, -1, true, K_NO_WAIT), 0);

	int32_t after = 0;
	zassert_equal(alp_qenc_get_position(h, &after), ALP_OK);
	zassert_equal(after - before, 1, "expected net +1 tick, got %d", (int)(after - before));

	alp_qenc_close(h);
}

ZTEST(alp_qenc_gpio_qdec, test_reset_position_zeroes_accumulator)
{
	const struct device *dev = DEVICE_DT_GET(QDEC_NODE);

	alp_qenc_config_t cfg = ALP_QENC_CONFIG_DEFAULT(0);
	alp_qenc_t        *h  = alp_qenc_open(&cfg);
	zassert_not_null(h);

	zassert_equal(input_report_rel(dev, INPUT_REL_WHEEL, 5, true, K_NO_WAIT), 0);

	zassert_equal(alp_qenc_reset_position(h), ALP_OK);

	int32_t pos = -1;
	zassert_equal(alp_qenc_get_position(h, &pos), ALP_OK);
	zassert_equal(pos, 0, "expected 0 after reset, got %d", (int)pos);

	alp_qenc_close(h);
}

ZTEST(alp_qenc_gpio_qdec, test_unrelated_event_type_ignored)
{
	const struct device *dev = DEVICE_DT_GET(QDEC_NODE);

	alp_qenc_config_t cfg = ALP_QENC_CONFIG_DEFAULT(0);
	alp_qenc_t        *h  = alp_qenc_open(&cfg);
	zassert_not_null(h);
	zassert_equal(alp_qenc_reset_position(h), ALP_OK);

	/* An INPUT_EV_KEY (or any non-REL) event on the same device must
	 * not move the position -- the callback filters on evt->type. */
	zassert_equal(input_report_key(dev, INPUT_KEY_0, 1, true, K_NO_WAIT), 0);

	int32_t pos = -1;
	zassert_equal(alp_qenc_get_position(h, &pos), ALP_OK);
	zassert_equal(pos, 0, "non-REL event must not move position, got %d", (int)pos);

	alp_qenc_close(h);
}
