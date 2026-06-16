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

#include "backends/i2s/i2s_ops.h"

ALP_BACKEND_DEFINE_CLASS(i2s);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_I2S_HANDLES
#define CONFIG_ALP_SDK_MAX_I2S_HANDLES 2
#endif

static struct alp_i2s _pool[CONFIG_ALP_SDK_MAX_I2S_HANDLES];

static struct alp_i2s *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_I2S_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_i2s *h)
{
	h->in_use = false;
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
	return h;
}

alp_status_t alp_i2s_start(alp_i2s_t *i2s)
{
	if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
	if (i2s->state.ops->start == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t rc = i2s->state.ops->start(&i2s->state);
	if (rc == ALP_OK) i2s->started = true;
	return rc;
}

alp_status_t alp_i2s_stop(alp_i2s_t *i2s)
{
	if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
	if (i2s->state.ops->stop == NULL) return ALP_ERR_NOSUPPORT;
	alp_status_t rc = i2s->state.ops->stop(&i2s->state);
	if (rc == ALP_OK) i2s->started = false;
	return rc;
}

alp_status_t alp_i2s_write(alp_i2s_t *i2s, const void *block, size_t bytes, uint32_t timeout_ms)
{
	if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
	if (block == NULL || bytes == 0u) return ALP_ERR_INVAL;
	if (i2s->state.ops->write == NULL) return ALP_ERR_NOSUPPORT;
	return i2s->state.ops->write(&i2s->state, block, bytes, timeout_ms);
}

alp_status_t
alp_i2s_read(alp_i2s_t *i2s, void *block, size_t bytes, size_t *bytes_out, uint32_t timeout_ms)
{
	if (i2s == NULL || !i2s->in_use) return ALP_ERR_NOT_READY;
	if (block == NULL || bytes == 0u) return ALP_ERR_INVAL;
	if (bytes_out != NULL) *bytes_out = 0u;
	if (i2s->state.ops->read == NULL) return ALP_ERR_NOSUPPORT;
	return i2s->state.ops->read(&i2s->state, block, bytes, bytes_out, timeout_ms);
}

void alp_i2s_close(alp_i2s_t *i2s)
{
	if (i2s == NULL || !i2s->in_use) return;
	if (i2s->state.ops != NULL && i2s->state.ops->close != NULL) {
		i2s->state.ops->close(&i2s->state);
	}
	_free(i2s);
}

const alp_capabilities_t *alp_i2s_capabilities(const alp_i2s_t *i2s)
{
	return (i2s != NULL) ? &i2s->cached_caps : NULL;
}
