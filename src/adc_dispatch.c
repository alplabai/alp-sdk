/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC class dispatcher.  Owns the public alp_adc_* API surface
 * and routes through the backend registry mechanism shipped in
 * Slice 0 (PR #17).  All voltage-conversion math (raw -> uV)
 * lives here so every backend reports raw values in the same
 * convention.
 *
 * The handle struct layout (struct alp_adc) lives in
 * src/backends/adc/adc_ops.h so vendor-ext backend .c files can
 * reach the fields directly without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/adc/adc_ops.h"

ALP_BACKEND_DEFINE_CLASS(adc);
/* Pull the adc registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(adc);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Declared in src/zephyr/handles.h but
 * we forward-declare here to avoid pulling in the broader handles
 * header (which carries unrelated peripheral declarations). */
#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static struct alp_adc _pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static struct alp_adc *_alloc_handle(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_ADC_HANDLE_POOL; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it -- including
		 * lifecycle/active_ops, parking a fresh slot at UNOPENED). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_adc, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free_handle(struct alp_adc *h)
{
	alp_slot_release(&h->in_use);
}

alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg)
{
	/* Public contract (alp_last_error, <alp/peripheral.h>): every
     * successful alp_*_open clears the thread-local last-error slot,
     * matching the sibling i2c/spi dispatchers' alp_z_clear_last_error()
     * at open() entry -- a stale failure from an earlier call must not
     * still read back after THIS open succeeds. */
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	/* SoC capability gate: reject a resolution the active SoC can't
     * deliver before any backend dispatch.  ALP_SOC_ADC_MAX_RESOLUTION_BITS
     * is UINT16_MAX under CONFIG_ALP_SOC_NONE -- provably >= any uint8_t
     * cfg->resolution_bits, so the check is a documented no-op there (a
     * valid-but-unresolved channel surfaces NOT_READY from the backend
     * open() instead) and the #if below compiles it out entirely instead
     * of leaving a runtime comparison GCC's -Wtype-limits (correctly)
     * flags as provably-always-false for that sentinel (issue #634). */
#if ALP_SOC_ADC_MAX_RESOLUTION_BITS < UINT8_MAX
	if ((uint32_t)cfg->resolution_bits > (uint32_t)ALP_SOC_ADC_MAX_RESOLUTION_BITS) {
		alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
		return NULL;
	}
#endif
	const alp_backend_t *be = alp_backend_select("adc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_adc_ops_t *ops = (const alp_adc_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_adc *h = _alloc_handle();
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
		_free_handle(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_adc_read_raw(alp_adc_t *h, int32_t *raw_out)
{
	/* Preserve the original contract: a NULL handle OR NULL out-param is
	 * ALP_ERR_INVAL (not NOT_READY). Only a non-NULL but closed/closing
	 * handle yields NOT_READY via the op-enter gate. */
	if (h == NULL || raw_out == NULL) {
		return ALP_ERR_INVAL;
	}
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc_handle/_free_handle, so
	 * mixing it with a plain read here is a data race, and a racing
	 * close could free the slot mid-op (issue #629). */
	if (!alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = h->state.ops->read_raw(&h->state, raw_out);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_adc_read_uv(alp_adc_t *h, int32_t *uv_out)
{
	if (h == NULL || uv_out == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	int32_t      raw = 0;
	alp_status_t rc  = h->state.ops->read_raw(&h->state, &raw);
	if (rc != ALP_OK) {
		alp_handle_op_leave(&h->active_ops);
		return rc;
	}
	/* Width-safe full-scale: h->state.resolution_bits is
     * backend-resolved (not gated by the SoC-cap check under
     * CONFIG_ALP_SOC_NONE -- see the comment on that check above),
     * so a misbehaving/test backend can hand back an out-of-range
     * width.  raw is an int32_t, so no width beyond 31 bits is even
     * representable; reject 0 (the prior "not configured" sentinel)
     * and anything > 31 before shifting -- `1 << 31` on the int64_t
     * literal below is then always well-defined. */
	if (h->state.resolution_bits == 0 || h->state.resolution_bits > 31) {
		alp_handle_op_leave(&h->active_ops);
		return ALP_ERR_NOT_READY;
	}
	const int64_t fs = ((int64_t)1 << h->state.resolution_bits) - 1;
	*uv_out          = (int32_t)((int64_t)raw * (int64_t)h->state.reference_uv / fs);
	alp_handle_op_leave(&h->active_ops);
	return ALP_OK;
}

void alp_adc_close(alp_adc_t *h)
{
	if (h == NULL) {
		return;
	}
	/* Gate out new ops and drain any in-flight one before touching
	 * state.ops -- makes "close races a blocked/in-flight op" a
	 * bounded wait instead of a use-after-free (issue #629).  Losing
	 * the CAS (already closed/closing/never-opened) makes this a
	 * no-op, matching the existing void-close idempotency contract. */
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_handle(h);
}

const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
