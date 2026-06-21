/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp adc` (read a channel raw) + `alp pwm` (set period/duty).  Both
 * speak the portable <alp/adc.h> / <alp/pwm.h> surface by channel_id.
 *
 * `alp pwm set` is gated behind CONFIG_ALP_SDK_CONSOLE_UNSAFE because it
 * drives hardware outputs; `alp adc read` is always-on (read-only).
 *
 * `alp adc` is gated behind CONFIG_ALP_SDK_PERIPH_ADC so the console links
 * on board builds where the ADC subsystem is absent.
 *
 * Note: portable <alp/pwm.h> has no duty-read, so v1 exposes only
 * `pwm set <ch> <period_ns> <duty_ns>` (no `get`).
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#include <alp/pwm.h>

#include "alp_console.h"

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_ADC)
#include <alp/adc.h>

static int cmd_adc_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long ch;

	if (alp_console_parse_ulong(argv[1], &ch) != 0) {
		shell_error(sh, "bad channel");
		return -EINVAL;
	}

	alp_adc_t *adc = alp_adc_open(&(alp_adc_config_t){
	    .channel_id      = (uint32_t)ch,
	    .resolution_bits = 12,
	});

	if (adc == NULL) {
		shell_error(sh, "open ch %lu failed (err %d)", ch, (int)alp_last_error());
		return -EIO;
	}

	int32_t      raw = 0;
	alp_status_t s   = alp_adc_read_raw(adc, &raw);

	alp_adc_close(adc);
	if (s != ALP_OK) {
		shell_error(sh, "read failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "adc[%lu] raw = %d", ch, raw);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_adc_subcmds,
    SHELL_CMD_ARG(read, NULL, "read <ch> -- one-shot raw conversion", cmd_adc_read, 2, 0),
    SHELL_SUBCMD_SET_END);
SHELL_SUBCMD_ADD((alp), adc, &alp_adc_subcmds, "ADC one-shot read", NULL, 1, 0);
#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_ADC */

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_PWM)
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_pwm_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long ch;
	unsigned long period_ns;
	unsigned long duty_ns;

	if (alp_console_parse_ulong(argv[1], &ch) != 0 ||
	    alp_console_parse_ulong(argv[2], &period_ns) != 0 ||
	    alp_console_parse_ulong(argv[3], &duty_ns) != 0 || duty_ns > period_ns) {
		shell_error(sh, "usage: alp pwm set <ch> <period_ns> <duty_ns<=period>");
		return -EINVAL;
	}

	alp_pwm_t *pwm = alp_pwm_open(&(alp_pwm_config_t){
	    .channel_id = (uint32_t)ch,
	    .period_ns  = (uint32_t)period_ns,
	});

	if (pwm == NULL) {
		shell_error(sh, "open ch %lu failed (err %d)", ch, (int)alp_last_error());
		return -EIO;
	}

	alp_status_t s = alp_pwm_set_period(pwm, (uint32_t)period_ns);

	if (s == ALP_OK) {
		s = alp_pwm_set_duty(pwm, (uint32_t)duty_ns);
	}
	alp_pwm_close(pwm);
	if (s != ALP_OK) {
		shell_error(sh, "set failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "pwm[%lu] period=%luns duty=%luns", ch, period_ns, duty_ns);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_pwm_subcmds,
    SHELL_CMD_ARG(set, NULL, "set <ch> <period_ns> <duty_ns> (UNSAFE)", cmd_pwm_set, 4, 0),
    SHELL_SUBCMD_SET_END);
SHELL_SUBCMD_ADD((alp), pwm, &alp_pwm_subcmds, "PWM set period/duty (UNSAFE)", NULL, 1, 0);
#endif /* CONFIG_ALP_SDK_CONSOLE_UNSAFE */
#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_PWM */
