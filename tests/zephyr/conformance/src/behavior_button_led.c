/* SPDX-License-Identifier: Apache-2.0
 *
 * alp_button_led_set_press_callback() re-registration race (#2987 item 149).
 *
 * The edge ISR reads press_cb and cb_user as a pair, so the helper must mask the
 * IRQ BEFORE it rewrites either field.  A single-core test cannot interleave an
 * ISR, so this suite wraps alp_gpio_irq_disable() (link flag in CMakeLists.txt)
 * and snapshots the handle's fields at the moment the mask is requested: they
 * must still hold the previous registration.  Moving the disable below the field
 * stores turns test_irq_masked_before_fields_change red.
 */

#include <zephyr/ztest.h>

#include <alp/blocks/button_led.h>
#include <alp/peripheral.h>
#include <alp/testing/common.h>
#include <alp/testing/gpio.h>

#define BTN_PIN 20
#define LED_PIN 21

static alp_button_led_t         *g_watch;
static bool                      g_disable_seen;
static alp_button_led_press_cb_t g_cb_at_disable;
static void                     *g_user_at_disable;

alp_status_t __real_alp_gpio_irq_disable(alp_gpio_t *pin);

alp_status_t __wrap_alp_gpio_irq_disable(alp_gpio_t *pin)
{
	if (g_watch != NULL && pin == g_watch->button) {
		g_disable_seen    = true;
		g_cb_at_disable   = g_watch->press_cb;
		g_user_at_disable = g_watch->cb_user;
	}
	return __real_alp_gpio_irq_disable(pin);
}

static int   g_hits;
static void *g_last_user;

static void cb_a(void *user)
{
	g_hits++;
	g_last_user = user;
}

static void cb_b(void *user)
{
	g_hits++;
	g_last_user = user;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	alp_testing_reset_all();
	g_watch        = NULL;
	g_disable_seen = false;
	g_hits         = 0;
	g_last_user    = NULL;
}

ZTEST_SUITE(alp_button_led_race, NULL, NULL, before, NULL, NULL);

static void init_bl(alp_button_led_t *bl)
{
	const alp_button_led_config_t cfg = {
		.button_pin_id     = BTN_PIN,
		.led_pin_id        = LED_PIN,
		.active_low_button = true,
	};
	zassert_equal(alp_button_led_init(bl, &cfg), ALP_OK);
}

ZTEST(alp_button_led_race, test_irq_masked_before_fields_change)
{
	alp_button_led_t bl;
	init_bl(&bl);

	zassert_equal(alp_button_led_set_press_callback(&bl, cb_a, (void *)0xA), ALP_OK);

	g_watch        = &bl;
	g_disable_seen = false;
	zassert_equal(alp_button_led_set_press_callback(&bl, cb_b, (void *)0xB), ALP_OK);

	zassert_true(g_disable_seen, "re-registration must mask the edge IRQ");
	zassert_equal(g_cb_at_disable, cb_a, "IRQ must be masked before press_cb is rewritten");
	zassert_equal(g_user_at_disable, (void *)0xA, "IRQ must be masked before cb_user is rewritten");

	alp_button_led_deinit(&bl);
}

ZTEST(alp_button_led_race, test_callback_after_swap_gets_new_user)
{
	alp_button_led_t bl;
	init_bl(&bl);

	zassert_equal(alp_button_led_set_press_callback(&bl, cb_a, (void *)0xA), ALP_OK);
	zassert_equal(alp_button_led_set_press_callback(&bl, cb_b, (void *)0xB), ALP_OK);

	zassert_equal(alp_testing_gpio_edge(BTN_PIN, ALP_GPIO_EDGE_FALLING), ALP_OK);
	zassert_equal(g_hits, 1);
	zassert_equal(g_last_user, (void *)0xB, "callback must see the new registration's user");

	alp_button_led_deinit(&bl);
}

ZTEST(alp_button_led_race, test_clear_masks_irq)
{
	alp_button_led_t bl;
	init_bl(&bl);

	zassert_equal(alp_button_led_set_press_callback(&bl, cb_a, (void *)0xA), ALP_OK);
	zassert_equal(alp_button_led_set_press_callback(&bl, NULL, NULL), ALP_OK);

	(void)alp_testing_gpio_edge(BTN_PIN, ALP_GPIO_EDGE_FALLING);
	zassert_equal(g_hits, 0, "a cleared callback must not fire");

	alp_button_led_deinit(&bl);
}
