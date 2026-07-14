/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S class dispatcher.  Routes the public alp_i2s_* API
 * through the .alp_backends_i2s registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/i2s/i2s_ops.h"

ALP_BACKEND_DEFINE_CLASS(i2s);
/* Pull the i2s registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(i2s);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_I2S_HANDLES
#define CONFIG_ALP_SDK_MAX_I2S_HANDLES 2
#endif

static struct alp_i2s _pool[CONFIG_ALP_SDK_MAX_I2S_HANDLES];

static struct alp_i2s *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_I2S_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it -- including
		 * lifecycle/active_ops, parking a fresh slot at UNOPENED). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_i2s, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_i2s *h)
{
	alp_slot_release(&h->in_use);
}

alp_i2s_t *alp_i2s_open(const alp_i2s_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->channels == 0u || cfg->channels > 2u || cfg->block_frames == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	if (cfg->word_bits != 8u && cfg->word_bits != 16u && cfg->word_bits != 24u &&
	    cfg->word_bits != 32u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("i2s", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_i2s_ops_t *ops = (const alp_i2s_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_i2s *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	h->cfg                  = *cfg;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->bus_id, &refined);
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

alp_status_t alp_i2s_start(alp_i2s_t *i2s)
{
	/* Gate on the lifecycle byte, not a plain in_use read: in_use is
	 * claimed/released atomically in _alloc/_free, so mixing it with a
	 * plain read here is a data race, and a racing close could free
	 * the slot mid-op (issue #629). */
	if (i2s == NULL || !alp_handle_op_enter(&i2s->lifecycle, &i2s->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc = ALP_ERR_NOSUPPORT;
	if (i2s->state.ops->start != NULL) {
		rc = i2s->state.ops->start(&i2s->state);
		if (rc == ALP_OK) i2s->started = true;
	}
	alp_handle_op_leave(&i2s->active_ops);
	return rc;
}

alp_status_t alp_i2s_stop(alp_i2s_t *i2s)
{
	if (i2s == NULL || !alp_handle_op_enter(&i2s->lifecycle, &i2s->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc = ALP_ERR_NOSUPPORT;
	if (i2s->state.ops->stop != NULL) {
		rc = i2s->state.ops->stop(&i2s->state);
		if (rc == ALP_OK) i2s->started = false;
	}
	alp_handle_op_leave(&i2s->active_ops);
	return rc;
}

alp_status_t alp_i2s_write(alp_i2s_t *i2s, const void *block, size_t bytes, uint32_t timeout_ms)
{
	if (block == NULL || bytes == 0u) return ALP_ERR_INVAL; /* param check before gate */
	/* Counted via alp_handle_op_enter/leave (issue #629): write() can
	 * block up to timeout_ms draining the transfer, so alp_i2s_close()
	 * drains this op with the sleep-poll alp_handle_begin_close_blocking()
	 * (src/common/alp_slot_claim.c) instead of the busy-spin
	 * alp_handle_begin_close() -- generalised from rpc_dispatch.c's
	 * _rpc_op_enter()/_rpc_begin_close()/_rpc_drain() (GHSA-xhm8).
	 * start/stop stay on the short, synchronous op_enter/leave path. */
	if (i2s == NULL || !alp_handle_op_enter(&i2s->lifecycle, &i2s->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc = (i2s->state.ops->write == NULL)
	                      ? ALP_ERR_NOSUPPORT
	                      : i2s->state.ops->write(&i2s->state, block, bytes, timeout_ms);
	alp_handle_op_leave(&i2s->active_ops);
	return rc;
}

alp_status_t
alp_i2s_read(alp_i2s_t *i2s, void *block, size_t bytes, size_t *bytes_out, uint32_t timeout_ms)
{
	if (bytes_out != NULL) *bytes_out = 0u;
	if (block == NULL || bytes == 0u) return ALP_ERR_INVAL; /* param check before gate */
	/* Counted via alp_handle_op_enter/leave (issue #629) -- see
	 * alp_i2s_write() above for the same rationale. */
	if (i2s == NULL || !alp_handle_op_enter(&i2s->lifecycle, &i2s->active_ops))
		return ALP_ERR_NOT_READY;
	alp_status_t rc = (i2s->state.ops->read == NULL)
	                      ? ALP_ERR_NOSUPPORT
	                      : i2s->state.ops->read(&i2s->state, block, bytes, bytes_out, timeout_ms);
	alp_handle_op_leave(&i2s->active_ops);
	return rc;
}

void alp_i2s_close(alp_i2s_t *i2s)
{
	if (i2s == NULL) return;
	/* Sleep-poll drain (issue #629): this pool counts alp_i2s_write()/
	 * alp_i2s_read(), each of which can block up to its caller's
	 * timeout_ms, so alp_handle_begin_close_blocking() sleeps between
	 * polls instead of busy-spinning -- see src/common/alp_slot_claim.c/.h.
	 * Idempotent: a second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&i2s->lifecycle, &i2s->active_ops)) return;
	if (i2s->state.ops != NULL && i2s->state.ops->close != NULL) {
		i2s->state.ops->close(&i2s->state);
	}
	alp_lifecycle_set(&i2s->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(i2s);
}

const alp_capabilities_t *alp_i2s_capabilities(const alp_i2s_t *i2s)
{
	return (i2s != NULL) ? &i2s->cached_caps : NULL;
}
