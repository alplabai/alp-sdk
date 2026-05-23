/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Storage class dispatcher.  Routes the public alp_storage_* API
 * through the .alp_backends_storage registry.
 *
 * Two Zephyr-side backends compete for selection on real silicon
 * (zephyr_flash, zephyr_littlefs); a stateless sw_fallback wildcards
 * in at priority 0 for native_sim builds.  Vendor inline-AES
 * extensions (Alif OSPI SecAES, NXP FlexSPI OTFAD) plug in as
 * additional registrations once their vendor packs land.
 *
 * The configure_inline_aes input validation (mode range, NULL key /
 * iv when mode != OFF, key_bytes in {16, 24, 32}) lives in this
 * dispatcher so every backend gets it for free -- the per-backend
 * op only runs once the inputs are known good.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/soc_caps.h>
#include <alp/storage.h>

#include "backends/storage/storage_ops.h"

ALP_BACKEND_DEFINE_CLASS(storage);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_STORAGE_HANDLES
#define CONFIG_ALP_SDK_MAX_STORAGE_HANDLES 4
#endif

static struct alp_storage _pool[CONFIG_ALP_SDK_MAX_STORAGE_HANDLES];

static struct alp_storage *_alloc(void) {
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_STORAGE_HANDLES; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free(struct alp_storage *h) { h->in_use = false; }

alp_storage_t *alp_storage_open(const alp_storage_config_t *cfg) {
    alp_z_clear_last_error();
    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("storage", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_storage_ops_t *ops = (const alp_storage_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_storage *h = _alloc();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend     = be;
    h->state.ops   = ops;
    h->state.kind  = cfg->kind;
    h->state.read_only = cfg->read_only;
    h->state.instance_id = cfg->instance_id;

    alp_capabilities_t caps = { .flags = be->base_caps };
    if (be->probe != NULL) {
        uint32_t refined = caps.flags;
        (void)be->probe(cfg->instance_id, &refined);
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

alp_status_t alp_storage_get_info(alp_storage_t *s, alp_storage_info_t *info) {
    if (info == NULL) return ALP_ERR_INVAL;
    *info = (alp_storage_info_t){0};
    if (s == NULL || !s->in_use) return ALP_ERR_NOT_READY;
    if (s->state.ops->get_info == NULL) return ALP_ERR_NOSUPPORT;
    return s->state.ops->get_info(&s->state, info);
}

alp_status_t alp_storage_read(alp_storage_t *s, uint64_t off,
                              void *data, size_t len) {
    if (s == NULL || !s->in_use) return ALP_ERR_NOT_READY;
    if (len == 0u) return ALP_OK;
    if (data == NULL) return ALP_ERR_INVAL;
    if (s->state.ops->read == NULL) return ALP_ERR_NOSUPPORT;
    return s->state.ops->read(&s->state, off, data, len);
}

alp_status_t alp_storage_write(alp_storage_t *s, uint64_t off,
                               const void *data, size_t len) {
    if (s == NULL || !s->in_use) return ALP_ERR_NOT_READY;
    if (s->state.read_only) return ALP_ERR_NOT_READY;
    if (len == 0u) return ALP_OK;
    if (data == NULL) return ALP_ERR_INVAL;
    if (s->state.ops->write == NULL) return ALP_ERR_NOSUPPORT;
    return s->state.ops->write(&s->state, off, data, len);
}

alp_status_t alp_storage_erase(alp_storage_t *s, uint64_t off, uint64_t len) {
    if (s == NULL || !s->in_use) return ALP_ERR_NOT_READY;
    if (s->state.read_only) return ALP_ERR_INVAL;
    if (len == 0u) return ALP_OK;
    if (s->state.ops->erase == NULL) return ALP_ERR_NOSUPPORT;
    return s->state.ops->erase(&s->state, off, len);
}

alp_status_t alp_storage_sync(alp_storage_t *s) {
    if (s == NULL || !s->in_use) return ALP_ERR_NOT_READY;
    if (s->state.ops->sync == NULL) return ALP_OK;   /* nothing to flush */
    return s->state.ops->sync(&s->state);
}

void alp_storage_close(alp_storage_t *s) {
    if (s == NULL || !s->in_use) return;
    if (s->state.ops != NULL && s->state.ops->close != NULL) {
        s->state.ops->close(&s->state);
    }
    _free(s);
}

const alp_capabilities_t *alp_storage_capabilities(const alp_storage_t *s) {
    return (s != NULL) ? &s->cached_caps : NULL;
}

alp_status_t alp_storage_configure_inline_aes(alp_storage_t                  *storage,
                                              const alp_storage_aes_config_t *cfg)
{
    /* Input validation runs ahead of backend dispatch so every backend
     * (including future vendor packs) gets the same parameter contract
     * for free.  Preserves the validation from the v0.1 storage_stub.c
     * verbatim. */
    if (cfg == NULL) return ALP_ERR_INVAL;
    if (cfg->mode != ALP_STORAGE_AES_OFF &&
        cfg->mode != ALP_STORAGE_AES_CTR &&
        cfg->mode != ALP_STORAGE_AES_XTS) {
        return ALP_ERR_INVAL;
    }
    if (cfg->mode != ALP_STORAGE_AES_OFF) {
        if (cfg->key == NULL || cfg->iv == NULL) return ALP_ERR_INVAL;
        if (cfg->key_bytes != 16u && cfg->key_bytes != 24u && cfg->key_bytes != 32u) {
            return ALP_ERR_INVAL;
        }
    }
    if (storage == NULL || !storage->in_use) return ALP_ERR_NOT_READY;
    if (storage->state.ops->configure_inline_aes == NULL) return ALP_ERR_NOSUPPORT;
    return storage->state.ops->configure_inline_aes(&storage->state, cfg);
}
