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

#include "alp_slot_claim.h"
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
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it -- including
		 * lifecycle/active_ops, parking a fresh slot at UNOPENED). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_pwm, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_pwm *h)
{
	alp_slot_release(&h->in_use);
}

static struct alp_pwm_capture *_alloc_cap(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_PWM_HANDLES; ++i) {
		if (alp_slot_try_claim(&_cap_pool[i].in_use)) {
			memset(&_cap_pool[i], 0, offsetof(struct alp_pwm_capture, in_use));
			return &_cap_pool[i];
		}
	}
	return NULL;
}

static void _free_cap(struct alp_pwm_capture *h)
{
	alp_slot_release(&h->in_use);
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
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_pwm_set_duty(alp_pwm_t *pwm, uint32_t pulse_ns)
{
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc/_free, so mixing it with a
	 * plain read here is a data race, and a racing close could free
	 * the slot mid-op (issue #629). */
	if (pwm == NULL || !alp_handle_op_enter(&pwm->lifecycle, &pwm->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (pulse_ns > pwm->period_ns) {
		rc = ALP_ERR_INVAL;
	} else if (pwm->state.ops->set_duty == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = pwm->state.ops->set_duty(&pwm->state, pulse_ns);
	}
	alp_handle_op_leave(&pwm->active_ops);
	return rc;
}

alp_status_t alp_pwm_set_period(alp_pwm_t *pwm, uint32_t period_ns)
{
	if (pwm == NULL || !alp_handle_op_enter(&pwm->lifecycle, &pwm->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (period_ns == 0u) {
		rc = ALP_ERR_INVAL;
	} else if (pwm->state.ops->set_period == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = pwm->state.ops->set_period(&pwm->state, period_ns);
		if (rc == ALP_OK) pwm->period_ns = period_ns;
	}
	alp_handle_op_leave(&pwm->active_ops);
	return rc;
}

alp_status_t alp_pwm_configure(alp_pwm_t      *pwm,
                               alp_pwm_align_t align_mode,
                               uint32_t        dead_time_ns,
                               uint8_t         break_cfg)
{
	if (pwm == NULL || !alp_handle_op_enter(&pwm->lifecycle, &pwm->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if ((unsigned)align_mode > (unsigned)ALP_PWM_ALIGN_CENTER_BOTH) {
		rc = ALP_ERR_INVAL;
	} else if (pwm->state.ops->configure == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = pwm->state.ops->configure(&pwm->state, align_mode, dead_time_ns, break_cfg);
	}
	alp_handle_op_leave(&pwm->active_ops);
	return rc;
}

alp_status_t alp_pwm_single_pulse(alp_pwm_t *pwm, uint32_t pulse_ns)
{
	if (pwm == NULL || !alp_handle_op_enter(&pwm->lifecycle, &pwm->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (pulse_ns == 0u) {
		rc = ALP_ERR_INVAL;
	} else if (pwm->state.ops->single_pulse == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = pwm->state.ops->single_pulse(&pwm->state, pulse_ns);
	}
	alp_handle_op_leave(&pwm->active_ops);
	return rc;
}

void alp_pwm_close(alp_pwm_t *pwm)
{
	if (pwm == NULL) return;
	/* Gate out new ops and drain any in-flight one before touching
	 * state.ops -- makes "close races a blocked/in-flight op" a
	 * bounded wait instead of a use-after-free (issue #629).  Losing
	 * the CAS (already closed/closing/never-opened) makes this a
	 * no-op, matching the existing void-close idempotency contract. */
	if (!alp_handle_begin_close(&pwm->lifecycle, &pwm->active_ops)) return;
	if (pwm->state.ops != NULL && pwm->state.ops->close != NULL) {
		pwm->state.ops->close(&pwm->state);
	}
	alp_lifecycle_set(&pwm->lifecycle, ALP_HANDLE_LC_UNOPENED);
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
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t
alp_pwm_capture_read(alp_pwm_capture_t *cap, uint32_t *period_ns_out, uint32_t *pulse_ns_out)
{
	/* Validate the arguments BEFORE dereferencing the handle: "both
	 * outputs NULL" is a caller programming error independent of handle
	 * state, and checking it first keeps the result deterministic even
	 * when a test passes a not-ready/garbage handle (reading cap->in_use
	 * first is otherwise order-dependent on the handle's memory). */
	if (period_ns_out == NULL && pulse_ns_out == NULL) return ALP_ERR_INVAL;
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc_cap/_free_cap, so mixing it
	 * with a plain read here is a data race, and a racing close could
	 * free the slot mid-op (issue #629). */
	if (cap == NULL || !alp_handle_op_enter(&cap->lifecycle, &cap->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (cap->state.ops->capture_read == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = cap->state.ops->capture_read(&cap->state, period_ns_out, pulse_ns_out);
	}
	alp_handle_op_leave(&cap->active_ops);
	return rc;
}

void alp_pwm_capture_close(alp_pwm_capture_t *cap)
{
	if (cap == NULL) return;
	/* Gate out new ops and drain any in-flight one before touching
	 * state.ops -- makes "close races a blocked/in-flight op" a
	 * bounded wait instead of a use-after-free (issue #629).  Losing
	 * the CAS (already closed/closing/never-opened) makes this a
	 * no-op, matching the existing void-close idempotency contract. */
	if (!alp_handle_begin_close(&cap->lifecycle, &cap->active_ops)) return;
	if (cap->state.ops != NULL && cap->state.ops->capture_close != NULL) {
		cap->state.ops->capture_close(&cap->state);
	}
	alp_lifecycle_set(&cap->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_cap(cap);
}
