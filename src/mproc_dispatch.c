/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mproc class dispatcher.  Owns the public <alp/mproc.h> surface --
 * all three IPC primitives (alp_shmem_t / alp_mbox_t / alp_hwsem_t)
 * -- on top of the backend registry mechanism shipped in Slice 0
 * (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers all three
 * handle types since they live behind a single header and the
 * backend that implements one always implements the other two on
 * the same SoC.  The ops vtable carries function pointers for
 * every surface; the dispatcher maintains three separate handle
 * pools (shmem + mbox + hwsem) keyed off the same backend.
 *
 * Slice 4c ships no vendor extensions for mproc: the Zephyr
 * backend's DT-anchored alias lookups + intra-core k_sem fallback
 * cover every shipped SoM; no second registry tier is needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/mproc.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/mproc/mproc_ops.h"

ALP_BACKEND_DEFINE_CLASS(mproc);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_SHMEM_HANDLES
#define CONFIG_ALP_SDK_MAX_SHMEM_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_MBOX_HANDLES
#define CONFIG_ALP_SDK_MAX_MBOX_HANDLES 4
#endif
#ifndef CONFIG_ALP_SDK_MAX_HWSEM_HANDLES
#define CONFIG_ALP_SDK_MAX_HWSEM_HANDLES 4
#endif

static struct alp_shmem _shmem_pool[CONFIG_ALP_SDK_MAX_SHMEM_HANDLES];
static struct alp_mbox  _mbox_pool[CONFIG_ALP_SDK_MAX_MBOX_HANDLES];
static struct alp_hwsem _hwsem_pool[CONFIG_ALP_SDK_MAX_HWSEM_HANDLES];

static struct alp_shmem *_alloc_shmem(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_SHMEM_HANDLES; ++i) {
		if (!_shmem_pool[i].in_use) {
			memset(&_shmem_pool[i], 0, sizeof(_shmem_pool[i]));
			_shmem_pool[i].in_use = true;
			return &_shmem_pool[i];
		}
	}
	return NULL;
}

static void _free_shmem(struct alp_shmem *h)
{
	h->in_use = false;
}

static struct alp_mbox *_alloc_mbox(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_MBOX_HANDLES; ++i) {
		if (!_mbox_pool[i].in_use) {
			memset(&_mbox_pool[i], 0, sizeof(_mbox_pool[i]));
			_mbox_pool[i].in_use = true;
			return &_mbox_pool[i];
		}
	}
	return NULL;
}

static void _free_mbox(struct alp_mbox *h)
{
	h->in_use = false;
}

static struct alp_hwsem *_alloc_hwsem(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_HWSEM_HANDLES; ++i) {
		if (!_hwsem_pool[i].in_use) {
			memset(&_hwsem_pool[i], 0, sizeof(_hwsem_pool[i]));
			_hwsem_pool[i].in_use = true;
			return &_hwsem_pool[i];
		}
	}
	return NULL;
}

static void _free_hwsem(struct alp_hwsem *h)
{
	h->in_use = false;
}

/* ================================================================== */
/* Shared memory                                                       */
/* ================================================================== */

alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->name == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("mproc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_mproc_ops_t *ops = (const alp_mproc_ops_t *)be->ops;
	if (ops == NULL || ops->shmem_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_shmem *h = _alloc_shmem();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->shmem_open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_shmem(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_shmem_view(alp_shmem_t *s, void **base_out, size_t *size_out)
{
	if (base_out != NULL) *base_out = NULL;
	if (size_out != NULL) *size_out = 0;
	if (s == NULL || !s->in_use) return ALP_ERR_NOT_READY;
	if (s->state.ops == NULL || s->state.ops->shmem_view == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return s->state.ops->shmem_view(&s->state, base_out, size_out);
}

void alp_shmem_close(alp_shmem_t *s)
{
	if (s == NULL || !s->in_use) return;
	if (s->state.ops != NULL && s->state.ops->shmem_close != NULL) {
		s->state.ops->shmem_close(&s->state);
	}
	_free_shmem(s);
}

/* ================================================================== */
/* Mailbox                                                             */
/* ================================================================== */

alp_mbox_t *alp_mbox_open(const alp_mbox_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("mproc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_mproc_ops_t *ops = (const alp_mproc_ops_t *)be->ops;
	if (ops == NULL || ops->mbox_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_mbox *h = _alloc_mbox();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->mbox_open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_mbox(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_mbox_send(alp_mbox_t *mb, const void *data, size_t len, uint32_t timeout_ms)
{
	if (mb == NULL || !mb->in_use) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	if (mb->state.ops == NULL || mb->state.ops->mbox_send == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return mb->state.ops->mbox_send(&mb->state, data, len, timeout_ms);
}

alp_status_t alp_mbox_set_callback(alp_mbox_t *mb, alp_mbox_msg_cb_t cb, void *user)
{
	if (mb == NULL || !mb->in_use) return ALP_ERR_NOT_READY;
	if (mb->state.ops == NULL || mb->state.ops->mbox_set_callback == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return mb->state.ops->mbox_set_callback(&mb->state, cb, user);
}

void alp_mbox_close(alp_mbox_t *mb)
{
	if (mb == NULL || !mb->in_use) return;
	if (mb->state.ops != NULL && mb->state.ops->mbox_close != NULL) {
		mb->state.ops->mbox_close(&mb->state);
	}
	_free_mbox(mb);
}

/* ================================================================== */
/* Hardware semaphore                                                  */
/* ================================================================== */

alp_hwsem_t *alp_hwsem_open(uint32_t hwsem_id)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("mproc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_mproc_ops_t *ops = (const alp_mproc_ops_t *)be->ops;
	if (ops == NULL || ops->hwsem_open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_hwsem *h = _alloc_hwsem();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->hwsem_open(hwsem_id, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_hwsem(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_hwsem_try_lock(alp_hwsem_t *sem)
{
	if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
	if (sem->state.ops == NULL || sem->state.ops->hwsem_try_lock == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return sem->state.ops->hwsem_try_lock(&sem->state);
}

alp_status_t alp_hwsem_lock(alp_hwsem_t *sem, uint32_t timeout_ms)
{
	if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
	if (sem->state.ops == NULL || sem->state.ops->hwsem_lock == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return sem->state.ops->hwsem_lock(&sem->state, timeout_ms);
}

alp_status_t alp_hwsem_unlock(alp_hwsem_t *sem)
{
	if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
	if (sem->state.ops == NULL || sem->state.ops->hwsem_unlock == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return sem->state.ops->hwsem_unlock(&sem->state);
}

void alp_hwsem_close(alp_hwsem_t *sem)
{
	if (sem == NULL || !sem->in_use) return;
	if (sem->state.ops != NULL && sem->state.ops->hwsem_close != NULL) {
		sem->state.ops->hwsem_close(&sem->state);
	}
	_free_hwsem(sem);
}

/* ================================================================== */
/* Capability getters                                                  */
/* ================================================================== */

const alp_capabilities_t *alp_shmem_capabilities(const alp_shmem_t *s)
{
	return (s != NULL) ? &s->cached_caps : NULL;
}

const alp_capabilities_t *alp_mbox_capabilities(const alp_mbox_t *mb)
{
	return (mb != NULL) ? &mb->cached_caps : NULL;
}

const alp_capabilities_t *alp_hwsem_capabilities(const alp_hwsem_t *sem)
{
	return (sem != NULL) ? &sem->cached_caps : NULL;
}
