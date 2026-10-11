/* SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #2808: the zephyr gpio backend must make the pad
 * readable (alp_z_gpio_pad_input, the Alif pad input-buffer hook) whenever a pin
 * is configured as an INPUT, and only then.  native_sim is not an Ensemble part,
 * so this file supplies the hook (the Alif one is not built) and records what the
 * backend asked it to do.
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/ztest.h>

#include <alp/peripheral.h>

#include "backends/gpio/gpio_pad.h"

static int             calls;
static uint8_t         last_pin;
static alp_gpio_pull_t last_pull;
static int             next_rc;

int alp_z_gpio_pad_input(const struct gpio_dt_spec *spec, alp_gpio_pull_t pull)
{
	calls++;
	last_pin  = spec->pin;
	last_pull = pull;
	return next_rc;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	calls   = 0;
	next_rc = 0;
}

ZTEST_SUITE(alp_gpio_input_pad, NULL, NULL, before, NULL, NULL);

ZTEST(alp_gpio_input_pad, test_input_configure_enables_pad)
{
	alp_gpio_t *h = alp_gpio_open(2);
	zassert_not_null(h);
	zassert_equal(calls, 0, "open alone must not touch the pad");

	zassert_equal(alp_gpio_configure(h, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE), ALP_OK);
	zassert_equal(calls, 1);
	zassert_equal(last_pin, 2);
	zassert_equal(last_pull, ALP_GPIO_PULL_NONE);
	alp_gpio_close(h);
}

ZTEST(alp_gpio_input_pad, test_requested_pull_reaches_the_pad)
{
	alp_gpio_t *h = alp_gpio_open(1);
	zassert_not_null(h);
	zassert_equal(alp_gpio_configure(h, ALP_GPIO_INPUT, ALP_GPIO_PULL_UP), ALP_OK);
	zassert_equal(last_pull, ALP_GPIO_PULL_UP);
	alp_gpio_close(h);
}

ZTEST(alp_gpio_input_pad, test_output_configure_leaves_pad_alone)
{
	alp_gpio_t *h = alp_gpio_open(3);
	zassert_not_null(h);
	zassert_equal(alp_gpio_configure(h, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE), ALP_OK);
	zassert_equal(calls, 0);
	alp_gpio_close(h);
}

ZTEST(alp_gpio_input_pad, test_pad_failure_is_reported_not_swallowed)
{
	alp_gpio_t *h = alp_gpio_open(0);
	zassert_not_null(h);
	next_rc = -EIO;
	zassert_not_equal(alp_gpio_configure(h, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE), ALP_OK);
	alp_gpio_close(h);
}
