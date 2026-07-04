/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power class dispatcher.  Owns the public alp_power_* API
 * surface and routes through the backend registry mechanism
 * shipped in Slice 0 (PR #17).
 *
 * Handle pool sized to 1 by default -- the legacy
 * src/zephyr/power_zephyr.c returned a single static global; the
 * registry-pattern equivalent is a one-slot static pool so the
 * documented "only one open() active at a time" contract stays
 * intact.  Customers needing per-core handles bump
 * CONFIG_ALP_SDK_MAX_POWER_HANDLES.
 *
 * The handle struct layout (struct alp_power) lives in
 * src/backends/power/power_ops.h so per-backend .c files can reach
 * the fields without duplicating the layout.
 *
 * The dispatcher runs the request_sleep INVAL pre-checks
 * (RUN-mode rejection, invalid enum, no-wake-source + zero
 * wake_after_ms guard) before reaching the backend so every
 * backend sees a validated request.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/power.h>
#include <alp/soc_caps.h>

#include "backends/power/power_ops.h"

ALP_BACKEND_DEFINE_CLASS(power);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid pulling
 * in the broader handles.h header (which carries unrelated
 * peripheral pool declarations the dispatcher does not touch). */
extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_POWER_HANDLES
#define CONFIG_ALP_SDK_MAX_POWER_HANDLES 1
#endif

static struct alp_power _pool[CONFIG_ALP_SDK_MAX_POWER_HANDLES];

static struct alp_power *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_POWER_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_power *h)
{
	h->in_use = false;
}

alp_power_t *alp_power_open(void)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("power", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_power_ops_t *ops = (const alp_power_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_power *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(&h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_power_configure_wake_source(alp_power_t *h, uint32_t wake_bitmap)
{
	if (h == NULL || !h->in_use) {
		return ALP_ERR_NOT_READY;
	}
	h->state.wake_bitmap = wake_bitmap;
	return h->state.ops->configure_wake_source(&h->state, wake_bitmap);
}

alp_status_t alp_power_request_sleep(alp_power_t           *h,
                                     alp_power_mode_t       mode,
                                     uint32_t               wake_after_ms,
                                     alp_power_wake_info_t *info)
{
	if (h == NULL || !h->in_use) {
		return ALP_ERR_NOT_READY;
	}
	/* RUN is never a valid sleep target -- the caller is asking to
     * stay awake, which is a no-op (and likely an API misuse). */
	if (mode == ALP_POWER_MODE_RUN) {
		return ALP_ERR_INVAL;
	}
	if (mode != ALP_POWER_MODE_SLEEP && mode != ALP_POWER_MODE_DEEP_SLEEP &&
	    mode != ALP_POWER_MODE_STANDBY) {
		return ALP_ERR_INVAL;
	}
	/* No wake source AND no timer means the SoC would never wake.
     * Reject as INVAL so callers see the mistake. */
	if (h->state.wake_bitmap == 0u && wake_after_ms == 0u) {
		return ALP_ERR_INVAL;
	}
	return h->state.ops->request_sleep(&h->state, mode, wake_after_ms, info);
}

void alp_power_close(alp_power_t *h)
{
	if (h == NULL || !h->in_use) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free(h);
}

const alp_capabilities_t *alp_power_capabilities(const alp_power_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}

/* ================================================================== */
/* Operating-point profiles (class "power_profile", handle-less)       */
/*                                                                     */
/* A separate registry class from "power": the profile surface lives   */
/* behind a system-controller firmware on the SoCs that have one, and  */
/* its silicon-specific backend must not displace the portable         */
/* request_sleep winner above.  Handle-less TMU pattern: cache the      */
/* selected ops vtable on first call.                                  */
/* ================================================================== */

ALP_BACKEND_DEFINE_CLASS(power_profile);

static const alp_power_profile_ops_t *_cached_profile_ops = NULL;

static const alp_power_profile_ops_t *_get_profile_ops(void)
{
	if (_cached_profile_ops != NULL) {
		return _cached_profile_ops;
	}
	const alp_backend_t *be = alp_backend_select("power_profile", ALP_SOC_REF_STR);
	if (be == NULL) {
		return NULL;
	}
	_cached_profile_ops = (const alp_power_profile_ops_t *)be->ops;
	return _cached_profile_ops;
}

static bool _profile_id_valid(alp_power_profile_id_t which)
{
	return which == ALP_POWER_PROFILE_RUN || which == ALP_POWER_PROFILE_STANDBY;
}

alp_status_t alp_power_profile_get(alp_power_profile_id_t which, alp_power_profile_t *out)
{
	if (out == NULL || !_profile_id_valid(which)) {
		return ALP_ERR_INVAL;
	}
	memset(out, 0, sizeof(*out));
	const alp_power_profile_ops_t *ops = _get_profile_ops();
	if (ops == NULL || ops->get == NULL) {
		return ALP_ERR_NOSUPPORT;
	}
	return ops->get(which, out);
}

alp_status_t alp_power_profile_set(alp_power_profile_id_t which, const alp_power_profile_t *profile)
{
	if (profile == NULL || !_profile_id_valid(which)) {
		return ALP_ERR_INVAL;
	}
	/* wake_events is a standby-profile concept; rejecting it here keeps
	 * every backend's RUN read-modify-write path honest. */
	if (which == ALP_POWER_PROFILE_RUN && profile->wake_events != 0u) {
		return ALP_ERR_INVAL;
	}
	const alp_power_profile_ops_t *ops = _get_profile_ops();
	if (ops == NULL || ops->set == NULL) {
		return ALP_ERR_NOSUPPORT;
	}
	return ops->set(which, profile);
}
