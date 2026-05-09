/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mproc.h
 * @brief ALP SDK multi-processor IPC primitives.
 *
 * v0.3 deliverable.  v0.1 ships only the public surface; every entry
 * point returns ALP_ERR_NOSUPPORT.  This header is the wrapping the
 * "Multi-Processor Support Completion" line in `VERSIONS.md` v0.3
 * targets.
 *
 * Scope.  E1M-AEN ships with at minimum two heterogeneous Cortex-M55
 * cores (HP @ 400 MHz + HE @ 160 MHz), and on E5/E6/E7/E8 also one or
 * two Cortex-A32 cores @ 800 MHz.  This header gives the M55-HP / M55-HE
 * peer pair a uniform IPC surface; A32 ↔ M55 IPC is added in v0.4
 * when Linux/Yocto handles the A32 side.
 *
 * Three primitives:
 *   1. Shared-memory regions      — `alp_shmem_*`
 *   2. Mailbox channels           — `alp_mbox_*`  (Zephyr `mbox_*` / MHU)
 *   3. Hardware semaphores        — `alp_hwsem_*` (Zephyr `hwsem_*` / HWSEM)
 *
 * On top of these v0.3 also lands an OpenAMP-based RPC primitive (see
 * `<alp/rpc.h>`, ships with v0.3) so blocks can offload compute to the
 * M55-HE core without hand-rolling IPC.  This header stays at the lower
 * primitive level on purpose — RPC is a build-time choice, not a
 * mandatory abstraction.
 */

#ifndef ALP_MPROC_H
#define ALP_MPROC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Identifies a peer core in the MP topology. */
typedef enum {
    ALP_CORE_SELF      = 0,     /**< Whichever core this code is running on. */
    ALP_CORE_M55_HP    = 1,     /**< Application core on E1M-AEN. */
    ALP_CORE_M55_HE    = 2,     /**< High-efficiency core on E1M-AEN. */
    ALP_CORE_A32_0     = 3,     /**< Cortex-A32 #0 on E5/E6/E7/E8 (Linux). */
    ALP_CORE_A32_1     = 4      /**< Cortex-A32 #1 on E7/E8 (Linux). */
} alp_core_id_t;

/* ------------------------------------------------------------------ */
/* Shared memory                                                       */
/* ------------------------------------------------------------------ */

typedef struct alp_shmem alp_shmem_t;

typedef struct {
    const char *name;       /**< Region name shared across cores (DT-anchored). */
    size_t      size;       /**< Required bytes; rounded up to MMU/MPU page. */
    bool        cacheable;  /**< false ⇒ allocate non-cacheable; required for the
                                 simple "core A writes, core B reads" pattern. */
} alp_shmem_config_t;

alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg);

/** Pointer + length view of the region.  Both cores receive the same
 *  bytes; cache-flush/invalidate handled by the backend. */
alp_status_t alp_shmem_view(alp_shmem_t *s, void **base_out, size_t *size_out);

void         alp_shmem_close(alp_shmem_t *s);

/* ------------------------------------------------------------------ */
/* Mailbox                                                             */
/* ------------------------------------------------------------------ */

typedef struct alp_mbox alp_mbox_t;

typedef void (*alp_mbox_msg_cb_t)(uint32_t channel,
                                  const void *data, size_t len,
                                  void *user);

typedef struct {
    uint32_t channel;        /**< MHU/Mailbox channel index per the SoC manifest. */
    alp_core_id_t peer;      /**< Counterpart core. */
} alp_mbox_config_t;

alp_mbox_t  *alp_mbox_open(const alp_mbox_config_t *cfg);
alp_status_t alp_mbox_send(alp_mbox_t *mb, const void *data, size_t len,
                           uint32_t timeout_ms);
alp_status_t alp_mbox_set_callback(alp_mbox_t *mb,
                                   alp_mbox_msg_cb_t cb, void *user);
void         alp_mbox_close(alp_mbox_t *mb);

/* ------------------------------------------------------------------ */
/* Hardware semaphore                                                  */
/* ------------------------------------------------------------------ */

typedef struct alp_hwsem alp_hwsem_t;

alp_hwsem_t *alp_hwsem_open(uint32_t hwsem_id);

/** Try to take the hwsem.  Returns ALP_OK if owned, ALP_ERR_BUSY if held. */
alp_status_t alp_hwsem_try_lock(alp_hwsem_t *sem);

/** Block until the hwsem is owned, or @p timeout_ms elapses. */
alp_status_t alp_hwsem_lock(alp_hwsem_t *sem, uint32_t timeout_ms);

alp_status_t alp_hwsem_unlock(alp_hwsem_t *sem);

void         alp_hwsem_close(alp_hwsem_t *sem);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_MPROC_H */
