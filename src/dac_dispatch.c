/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DAC class dispatcher.  Owns the public alp_dac_* API surface
 * and routes through the backend registry mechanism shipped in
 * Slice 0 (PR #17).
 *
 * The handle struct layout (struct alp_dac) lives in
 * src/backends/dac/dac_ops.h so the backend .c files can reach
 * the fields directly without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dac.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/dac/dac_ops.h"

ALP_BACKEND_DEFINE_CLASS(dac);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_DAC_HANDLES
#define CONFIG_ALP_SDK_MAX_DAC_HANDLES 2
#endif

static struct alp_dac _pool[CONFIG_ALP_SDK_MAX_DAC_HANDLES];

static struct alp_dac *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_DAC_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_dac *h)
{
	h->in_use = false;
}

alp_dac_t *alp_dac_open(const alp_dac_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	/* SoC capability gate: reject an out-of-range channel before any
	 * backend dispatch.  ALP_SOC_DAC_COUNT is 0 under CONFIG_ALP_SOC_NONE,
	 * so this is skipped there and a valid-but-unresolved channel surfaces
	 * NOT_READY from the backend open() instead (mirrors the ADC dispatch's
	 * capability gate; the DAC registry migration in #33 dropped the old
	 * wrapper-array channel bound this restores). */
	if ((ALP_SOC_DAC_COUNT > 0) &&
	    (uint32_t)cfg->channel_id >= (uint32_t)ALP_SOC_DAC_COUNT) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	const alp_backend_t *be = alp_backend_select("dac", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_dac_ops_t *ops = (const alp_dac_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_dac *h = _alloc();
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

alp_status_t alp_dac_write_mv(alp_dac_t *h, uint16_t mv)
{
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->write_mv(&h->state, mv);
}

alp_status_t alp_dac_read_mv(alp_dac_t *h, uint16_t *mv_out)
{
	if (mv_out == NULL) return ALP_ERR_INVAL;
	if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
	return h->state.ops->read_mv(&h->state, mv_out);
}

void alp_dac_close(alp_dac_t *h)
{
	if (h == NULL || !h->in_use) return;
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	_free(h);
}
