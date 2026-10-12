/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SoM power-domain runtime through the real library (#2784, U5): the three
 * public domain calls answer through zephyr_pm_policy.c's vtable, and the pad
 * state is applied through the real pinctrl API before the first drive.
 */

#include <stdint.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/pm/state.h>
#include <zephyr/ztest.h>

#include <alp/power.h>

#include "som_power.h"

/* The STOP_MODE register window of the test devicetree is a made-up address.  The
 * cold-boot restore reads it on every boot that has no record, so stand in for it:
 * this image is a plain power-on reset (STOP_MODE_STAT clear). */
uint32_t alp_som_power_stop_mode_read(void)
{
	return 0u;
}

/* The emulator keeps a driven output apart from its input side; read the output
 * latch the way a pad with an input buffer reads back on silicon. */
int alp_som_power_pad_read(const struct gpio_dt_spec *s)
{
	int phys = gpio_emul_output_get(s->port, s->pin);

	return (phys < 0) ? phys : (((s->dt_flags & GPIO_ACTIVE_LOW) != 0U) ? !phys : phys);
}

static unsigned int g_mux_calls;
static unsigned int g_mux_pins;

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	(void)pins;
	(void)reg;
	g_mux_calls++;
	g_mux_pins = pin_cnt;
	return 0;
}

static void before(void *unused)
{
	(void)unused;
	alp_som_power_reset_for_test();
	g_mux_calls = 0;
	g_mux_pins  = 0;
}

ZTEST_SUITE(power_som_pm_policy, NULL, NULL, before, NULL, NULL);

ZTEST(power_som_pm_policy, test_public_domain_api_answers_through_the_vtable)
{
	alp_power_domain_info_t i;
	alp_power_boot_info_t   b;
	alp_power_t            *h = alp_power_open();

	zassert_not_null(h);

	zassert_ok(alp_power_domain_info(ALP_POWER_DOMAIN_WIFI_BLE, &i));
	zassert_true(i.present);
	zassert_equal(i.default_action, ALP_POWER_ACTION_HOLD_RESET);
	zassert_equal(i.dependents, ALP_POWER_DEP_CAM_LDO);

	zassert_ok(alp_power_domain_info(ALP_POWER_DOMAIN_EXT_RAM, &i));
	zassert_false(i.present, "the test SoM carries no HyperRAM");

	zassert_equal(alp_power_domain_policy_set(
	                  h, ALP_POWER_DOMAIN_EXT_RAM, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(
	    alp_power_domain_policy_set(h, ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_POLICY_RAIL_OFF),
	    ALP_ERR_NOSUPPORT,
	    "CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF is off");
	zassert_ok(alp_power_domain_policy_set(
	    h, ALP_POWER_DOMAIN_BACKLIGHT, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE));
	zassert_equal(alp_som_power_policy(ALP_POWER_DOMAIN_BACKLIGHT),
	              ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE);

	zassert_ok(alp_power_boot_wake_info(&b));
	zassert_false(b.valid, "no record at POR");

	alp_power_close(h);
}

ZTEST(power_som_pm_policy, test_pinctrl_state_is_applied_once_before_the_first_drive)
{
	const struct device *lpgpio = DEVICE_DT_GET(DT_NODELABEL(lpgpio));

	zassert_ok(gpio_pin_configure(lpgpio, 1, GPIO_OUTPUT_HIGH));
	zassert_equal(g_mux_calls, 0u, "nothing is muxed until a pad is driven");

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_equal(g_mux_calls, 1u);
	zassert_equal(g_mux_pins, 5u, "the whole pad group, not one pad");
	zassert_equal(gpio_emul_output_get(lpgpio, 1), 0, "nRESET held low");

	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(g_mux_calls, 1u, "applied once, not per drive");
}

/* The SoC power hooks native_sim does not have (the kernel idle path references
 * them once CONFIG_PM is on); the suite never sleeps. */
void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	(void)state;
	(void)substate_id;
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	(void)state;
	(void)substate_id;
}
