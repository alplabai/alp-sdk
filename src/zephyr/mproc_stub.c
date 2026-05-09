/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/mproc.h>.  Every entry point returns
 * ALP_ERR_NOSUPPORT; *_open helpers return NULL.  Real M55-HE peer
 * bring-up + MHU / HWSEM wrappings land in v0.3's
 * "Multi-Processor Support Completion" milestone (see VERSIONS.md).
 */

#include "alp/mproc.h"

/* ------------------------------------------------------------------ */
/* Shared memory                                                       */
/* ------------------------------------------------------------------ */

alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_shmem_view(alp_shmem_t *s, void **base_out, size_t *size_out) {
    (void)s;
    if (base_out != NULL) *base_out = NULL;
    if (size_out != NULL) *size_out = 0;
    return ALP_ERR_NOSUPPORT;
}

void alp_shmem_close(alp_shmem_t *s) {
    (void)s;
}

/* ------------------------------------------------------------------ */
/* Mailbox                                                             */
/* ------------------------------------------------------------------ */

alp_mbox_t *alp_mbox_open(const alp_mbox_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_mbox_send(alp_mbox_t *mb, const void *data, size_t len,
                           uint32_t timeout_ms) {
    (void)mb; (void)data; (void)len; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_mbox_set_callback(alp_mbox_t *mb,
                                   alp_mbox_msg_cb_t cb, void *user) {
    (void)mb; (void)cb; (void)user;
    return ALP_ERR_NOSUPPORT;
}

void alp_mbox_close(alp_mbox_t *mb) {
    (void)mb;
}

/* ------------------------------------------------------------------ */
/* Hardware semaphore                                                  */
/* ------------------------------------------------------------------ */

alp_hwsem_t *alp_hwsem_open(uint32_t hwsem_id) {
    (void)hwsem_id;
    return NULL;
}

alp_status_t alp_hwsem_try_lock(alp_hwsem_t *sem) {
    (void)sem;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_hwsem_lock(alp_hwsem_t *sem, uint32_t timeout_ms) {
    (void)sem; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_hwsem_unlock(alp_hwsem_t *sem) {
    (void)sem;
    return ALP_ERR_NOSUPPORT;
}

void alp_hwsem_close(alp_hwsem_t *sem) {
    (void)sem;
}
