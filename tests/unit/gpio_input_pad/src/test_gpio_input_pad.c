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
#include "backends/gpio/gpio_pad_alif_word.h"

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

/* The exact pad word (port 3 pin 0, AF 0 = GPIO).  The pinctrl driver writes the
 * low AF bits plus word[23:16] to the pad register, so the register is
 * 0x00210000 for no pull: REN (bit 16) + ODS 4 mA (bit 21), the value the
 * trace-runner workaround produced on EVK-03.  Not 0x00010000 (ODS dropped to
 * 2 mA, the #2844 bench finding) and not 0x00230000 (SMT, bit 17, is off). */
ZTEST(alp_gpio_input_pad, test_alif_input_pad_word_is_exact)
{
	const uint32_t addr = (3u << 6) | (0u << 3);

	zassert_equal(alp_alif_pad_input_word(3, 0, ALP_GPIO_PULL_NONE), addr | 0x00210000u);
	zassert_equal(alp_alif_pad_input_word(3, 1, ALP_GPIO_PULL_NONE),
	              (3u << 6) | (1u << 3) | 0x00210000u);
	zassert_equal(alp_alif_pad_input_word(4, 3, ALP_GPIO_PULL_NONE),
	              (4u << 6) | (3u << 3) | 0x00210000u);
	/* Pull is the DSC field, bits 19:20. */
	zassert_equal(alp_alif_pad_input_word(3, 0, ALP_GPIO_PULL_UP), addr | 0x00290000u);
	zassert_equal(alp_alif_pad_input_word(3, 0, ALP_GPIO_PULL_DOWN), addr | 0x00310000u);
	/* Register content = word[23:16] << 16 | AF. */
	zassert_equal(alp_alif_pad_input_word(4, 3, ALP_GPIO_PULL_NONE) & 0x00ff0000u, 0x00210000u);
}
