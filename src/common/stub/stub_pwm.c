/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * PWM (+ capture) NOSUPPORT stubs -- <alp/pwm.h>.  Split out of the
 * former src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_pwm_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_PWM)
alp_pwm_t *alp_pwm_open(const alp_pwm_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_pwm_set_duty(alp_pwm_t *p, uint32_t n)
{
	(void)p;
	(void)n;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_pwm_set_period(alp_pwm_t *p, uint32_t n)
{
	(void)p;
	(void)n;
	return ALP_ERR_NOSUPPORT;
}
void alp_pwm_close(alp_pwm_t *p)
{
	(void)p;
}
const alp_capabilities_t *alp_pwm_capabilities(const alp_pwm_t *p)
{
	(void)p;
	return NULL;
}
alp_status_t alp_pwm_configure(alp_pwm_t      *pwm,
                               alp_pwm_align_t align_mode,
                               uint32_t        dead_time_ns,
                               uint8_t         break_cfg)
{
	(void)pwm;
	(void)align_mode;
	(void)dead_time_ns;
	(void)break_cfg;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_pwm_single_pulse(alp_pwm_t *pwm, uint32_t pulse_ns)
{
	(void)pwm;
	(void)pulse_ns;
	return ALP_ERR_NOSUPPORT;
}
alp_pwm_capture_t *alp_pwm_capture_open(const alp_pwm_capture_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_pwm_capture_read(alp_pwm_capture_t *cap, uint32_t *period_ns_out, uint32_t *pulse_ns_out)
{
	(void)cap;
	if (period_ns_out != NULL) *period_ns_out = 0;
	if (pulse_ns_out != NULL) *pulse_ns_out = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_pwm_capture_close(alp_pwm_capture_t *cap)
{
	(void)cap;
}
#endif /* !ALP_VENDOR_OVERRIDES_PWM */
