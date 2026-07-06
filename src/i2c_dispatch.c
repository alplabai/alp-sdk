/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C class dispatcher.  Routes the public alp_i2c_* API
 * through the .alp_backends_i2c registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/i2c/i2c_ops.h"

ALP_BACKEND_DEFINE_CLASS(i2c);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_I2C_HANDLES
#define CONFIG_ALP_SDK_MAX_I2C_HANDLES 4
#endif

static struct alp_i2c _pool[CONFIG_ALP_SDK_MAX_I2C_HANDLES];

static struct alp_i2c *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_I2C_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_i2c, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_i2c *h)
{
	alp_slot_release(&h->in_use);
}

alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("i2c", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_i2c_ops_t *ops = (const alp_i2c_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_i2c *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
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

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
	if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	return bus->state.ops->write(&bus->state, addr, data, len);
}

alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len)
{
	if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	return bus->state.ops->read(&bus->state, addr, data, len);
}

alp_status_t alp_i2c_write_read(
    alp_i2c_t *bus, uint8_t addr, const uint8_t *wdata, size_t wlen, uint8_t *rdata, size_t rlen)
{
	if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
	if ((wdata == NULL && wlen > 0) || (rdata == NULL && rlen > 0)) {
		return ALP_ERR_INVAL;
	}
	return bus->state.ops->write_read(&bus->state, addr, wdata, wlen, rdata, rlen);
}

void alp_i2c_close(alp_i2c_t *bus)
{
	if (bus == NULL || !bus->in_use) return;
	if (bus->state.ops != NULL && bus->state.ops->close != NULL) {
		bus->state.ops->close(&bus->state);
	}
	_free(bus);
}

/* ------------------------------------------------------------------ */
/* Target (slave) mode                                                 */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES
#define CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES 2
#endif

static struct alp_i2c_target _tpool[CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES];

static struct alp_i2c_target *_talloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES; ++i) {
		if (alp_slot_try_claim(&_tpool[i].in_use)) {
			/* Zeroing everything before in_use also parks the
			 * lifecycle byte at LC_UNOPENED, so a stale handle
			 * pointer is treated as closed until open completes. */
			memset(&_tpool[i], 0, offsetof(struct alp_i2c_target, in_use));
			return &_tpool[i];
		}
	}
	return NULL;
}

alp_i2c_target_t *alp_i2c_target_open(const alp_i2c_target_config_t *cfg)
{
	alp_z_clear_last_error();
	/* 7-bit address space: 0x00-0x07 and 0x78-0x7F are reserved by
	 * the I2C spec (general call, CBUS, 10-bit prefixes, ...) -- a
	 * target must answer on 0x08..0x77 only. */
	if (cfg == NULL || cfg->on_write == NULL || cfg->on_read == NULL ||
	    cfg->own_addr_7bit < 0x08u || cfg->own_addr_7bit > 0x77u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("i2c", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_i2c_ops_t *ops = (const alp_i2c_ops_t *)be->ops;
	if (ops == NULL || ops->target_open == NULL || ops->target_close == NULL) {
		/* Backend has no target mode (e.g. the native_sim
		 * sw_fallback) -- degrade gracefully. */
		alp_z_set_last_error(ALP_ERR_NOSUPPORT);
		return NULL;
	}
	struct alp_i2c_target *h = _talloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend      = be;
	h->state.ops    = ops;
	alp_status_t rc = ops->target_open(cfg, &h->state);
	if (rc != ALP_OK) {
		alp_slot_release(&h->in_use);
		alp_z_set_last_error(rc);
		return NULL;
	}
	alp_lifecycle_set(&h->lifecycle, ALP_I2C_TARGET_LC_IDLE);
	return h;
}

void alp_i2c_target_close(alp_i2c_target_t *tgt)
{
	if (tgt == NULL) return;
	/* Exactly one caller wins the IDLE -> CLOSING step; a concurrent
	 * double-close (or a stale handle) loses the CAS and returns
	 * without touching the slot -- close stays idempotent without
	 * two threads unregistering the same registration. */
	if (!alp_lifecycle_cas(&tgt->lifecycle, ALP_I2C_TARGET_LC_IDLE, ALP_I2C_TARGET_LC_CLOSING)) {
		return;
	}
	if (tgt->state.ops != NULL && tgt->state.ops->target_close != NULL) {
		tgt->state.ops->target_close(&tgt->state);
	}
	alp_lifecycle_set(&tgt->lifecycle, ALP_I2C_TARGET_LC_UNOPENED);
	alp_slot_release(&tgt->in_use);
}

const alp_capabilities_t *alp_i2c_capabilities(const alp_i2c_t *bus)
{
	return (bus != NULL) ? &bus->cached_caps : NULL;
}
