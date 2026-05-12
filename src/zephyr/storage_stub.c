/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/storage.h>.  Real impl lands v0.4 alongside
 * the Yocto first-class work.  Same shape as iot_stub.c.
 */

#include <stddef.h>

#include "alp/storage.h"

alp_storage_t *alp_storage_open(const alp_storage_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_storage_get_info(alp_storage_t *s, alp_storage_info_t *info) {
    (void)s;
    if (info != NULL) {
        *info = (alp_storage_info_t){0};
    }
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_storage_read(alp_storage_t *s, uint64_t off,
                              void *data, size_t len) {
    (void)s; (void)off; (void)data; (void)len;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_storage_write(alp_storage_t *s, uint64_t off,
                               const void *data, size_t len) {
    (void)s; (void)off; (void)data; (void)len;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_storage_erase(alp_storage_t *s, uint64_t off, uint64_t len) {
    (void)s; (void)off; (void)len;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_storage_sync(alp_storage_t *s) {
    (void)s;
    return ALP_ERR_NOSUPPORT;
}

void alp_storage_close(alp_storage_t *s) {
    (void)s;
}

alp_status_t alp_storage_configure_inline_aes(alp_storage_t *storage,
                                              const alp_storage_aes_config_t *cfg)
{
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
    (void)storage;
    /* No SoM in scope ships the SecAES / OTFAD HAL body yet.
     * AEN E4 / E6 / E8 + i.MX 93 wire when their vendor packs
     * register the corresponding Zephyr flash-device extensions. */
    return ALP_ERR_NOSUPPORT;
}
