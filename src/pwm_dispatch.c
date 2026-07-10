/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PWM class dispatcher.  Routes the public alp_pwm_* API
 * (including the capture-side surface) through the
 * .alp_backends_pwm registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/pwm.h>
#include <alp/soc_caps.h>

#include "backends/pwm/pwm_ops.h"

ALP_BACKEND_DEFINE_CLASS(pwm);
/* Pull the pwm registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(pwm);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_PWM_HANDLES
#define CONFIG_ALP_SDK_MAX_PWM_HANDLES 8
#endif

static struct alp_pwm         _pool[CONFIG_ALP_SDK_MAX_PWM_HANDLES];
static struct alp_pwm_capture _cap_pool[CONFIG_ALP_SDK_MAX_PWM_HANDLES];

static struct alp_pwm *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_PWM_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_pwm *h)
{
	h->in_use = false;
}

static struct alp_pwm_capture *_alloc_cap(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_PWM_HANDLES; ++i) {
		if (!_cap_pool[i].in_use) {
			memset(&_cap_pool[i], 0, sizeof(_cap_pool[i]));
			_cap_pool[i].in_use = true;
			return &_cap_pool[i];
		}
	}
	return NULL;
}

static void _free_cap(struct alp_pwm_capture *h)
{
	h->in_use = false;
}

alp_pwm_t *alp_pwm_open(const alp_pwm_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("pwm", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_pwm_ops_t *ops = (const alp_pwm_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_pwm *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->channel_id, &refined);
		caps.flags = refined;
	}
	alp_status_t rc = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_pwm_set_duty(alp_pwm_t *pwm, uint32_t pulse_ns)
{
	if (pwm == NULL || !pwm->in_use) return ALP_ERR_NOT_READY;
	if (pulse_ns > pwm->period_ns) return ALP_ERR_INVAL;
	if (pwm->state.ops->set_duty == NULL) return ALP_ERR_NOSUPPORT;
	return pwm->state.ops->set_duty(&pwm->state, pulse_ns);
}

alp_status_t alp_pwm_set_period(alp_pwm_t *pwm, uint32_t period_ns)
{
	if (pwm == NULL || !pwm->in_use) return ALP_ERR_NOT_READY;
	if (period_ns == 0u) return ALP_ERR_INVAL;
	if (pwm->state.ops->set_period == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t rc = pwm->state.ops->set_period(&pwm->state, period_ns);
	if (rc == ALP_OK) pwm->period_ns = period_ns;
	return rc;
}

alp_status_t alp_pwm_configure(alp_pwm_t      *pwm,
                               alp_pwm_align_t align_mode,
                               uint32_t        dead_time_ns,
                               uint8_t         break_cfg)
{
	if (pwm == NULL || !pwm->in_use) return ALP_ERR_NOT_READY;
	if ((unsigned)align_mode > (unsigned)ALP_PWM_ALIGN_CENTER_BOTH) {
		return ALP_ERR_INVAL;
	}
	if (pwm->state.ops->configure == NULL) return ALP_ERR_NOSUPPORT;
	return pwm->state.ops->configure(&pwm->state, align_mode, dead_time_ns, break_cfg);
}

alp_status_t alp_pwm_single_pulse(alp_pwm_t *pwm, uint32_t pulse_ns)
{
	if (pwm == NULL || !pwm->in_use) return ALP_ERR_NOT_READY;
	if (pulse_ns == 0u) return ALP_ERR_INVAL;
	if (pwm->state.ops->single_pulse == NULL) return ALP_ERR_NOSUPPORT;
	return pwm->state.ops->single_pulse(&pwm->state, pulse_ns);
}

void alp_pwm_close(alp_pwm_t *pwm)
{
	if (pwm == NULL || !pwm->in_use) return;
	if (pwm->state.ops != NULL && pwm->state.ops->close != NULL) {
		pwm->state.ops->close(&pwm->state);
	}
	_free(pwm);
}

const alp_capabilities_t *alp_pwm_capabilities(const alp_pwm_t *pwm)
{
	return (pwm != NULL) ? &pwm->cached_caps : NULL;
}

/* ====================================================================== */
/* Input capture                                                          */
/* ====================================================================== */

alp_pwm_capture_t *alp_pwm_capture_open(const alp_pwm_capture_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	if (cfg->channel_id >= 8u) {
		alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
		return NULL;
	}
	if ((unsigned)cfg->edge > (unsigned)ALP_PWM_CAPTURE_EDGE_BOTH) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("pwm", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_pwm_ops_t *ops = (const alp_pwm_ops_t *)be->ops;
	if (ops == NULL || ops->capture_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOSUPPORT);
		return NULL;
	}
	struct alp_pwm_capture *h = _alloc_cap();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->channel_id, &refined);
		caps.flags = refined;
	}
	alp_status_t rc = ops->capture_open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_cap(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t
alp_pwm_capture_read(alp_pwm_capture_t *cap, uint32_t *period_ns_out, uint32_t *pulse_ns_out)
{
	if (cap == NULL || !cap->in_use) return ALP_ERR_NOT_READY;
	if (period_ns_out == NULL && pulse_ns_out == NULL) return ALP_ERR_INVAL;
	if (cap->state.ops->capture_read == NULL) return ALP_ERR_NOSUPPORT;
	return cap->state.ops->capture_read(&cap->state, period_ns_out, pulse_ns_out);
}

void alp_pwm_capture_close(alp_pwm_capture_t *cap)
{
	if (cap == NULL || !cap->in_use) return;
	if (cap->state.ops != NULL && cap->state.ops->capture_close != NULL) {
		cap->state.ops->capture_close(&cap->state);
	}
	_free_cap(cap);
}
