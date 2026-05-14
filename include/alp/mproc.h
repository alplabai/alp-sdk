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
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.3 mailbox + shmem + hwsem.
 *      See docs/abi-markers.md for the convention.
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

/** Opaque shared-memory region.  Allocate via @ref alp_shmem_open. */
typedef struct alp_shmem alp_shmem_t;

/** Configuration for a shared-memory region. */
typedef struct {
    const char *name;       /**< Region name shared across cores (DT-anchored). */
    size_t      size;       /**< Required bytes; rounded up to MMU/MPU page. */
    bool        cacheable;  /**< false ⇒ allocate non-cacheable; required for
                                 the simple "core A writes, core B reads"
                                 pattern. */
} alp_shmem_config_t;

/**
 * @brief Acquire access to a named shared-memory region.
 *
 * Both cores opening the same @c name see the same physical bytes;
 * cache coherency is the caller's responsibility unless
 * @c cacheable = false.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL with a non-empty name.
 * @return Open handle on success, or NULL if the region isn't declared
 *         in the build's DT or isn't reachable from this core.
 */
alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg);

/**
 * @brief Get a pointer + length view of the region.
 *
 * Both cores receive the same bytes; cache flush/invalidate is
 * handled by the backend when @c cacheable = false at open.
 *
 * @param[in]  s         Region handle.
 * @param[out] base_out  Receives the region base pointer.
 * @param[out] size_out  Receives the region size in bytes.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL.
 */
alp_status_t alp_shmem_view(alp_shmem_t *s, void **base_out, size_t *size_out);

/** @brief Release the region handle.  Doesn't free underlying memory. */
void         alp_shmem_close(alp_shmem_t *s);

/* ------------------------------------------------------------------ */
/* Mailbox                                                             */
/* ------------------------------------------------------------------ */

/** Opaque mailbox channel.  Allocate via @ref alp_mbox_open. */
typedef struct alp_mbox alp_mbox_t;

/**
 * @brief Inbound-message callback.  Runs on the host stack's mailbox
 *        thread (Zephyr) or interrupt context (bare-metal).
 *
 * @param[in] channel  Mailbox channel index.
 * @param[in] data     Inbound payload.  Owned by the SDK; copy what you need.
 * @param[in] len      Payload length.
 * @param[in] user     Opaque pointer set via @ref alp_mbox_set_callback.
 */
typedef void (*alp_mbox_msg_cb_t)(uint32_t channel,
                                  const void *data, size_t len,
                                  void *user);

/** Mailbox-channel configuration. */
typedef struct {
    uint32_t channel;        /**< MHU/Mailbox channel index per the SoC manifest. */
    alp_core_id_t peer;      /**< Counterpart core. */
} alp_mbox_config_t;

/**
 * @brief Acquire a mailbox-channel handle.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 * @return Open handle on success, or NULL on resolution failure.
 */
alp_mbox_t  *alp_mbox_open(const alp_mbox_config_t *cfg);

/**
 * @brief Send @p len bytes to the peer core.
 *
 * @param[in] mb          Channel handle.
 * @param[in] data        Payload.
 * @param[in] len         Length, ≤ MHU MTU (typically 16–64 B).
 * @param[in] timeout_ms  Max wait for peer to drain its inbox.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT.
 */
alp_status_t alp_mbox_send(alp_mbox_t *mb, const void *data, size_t len,
                           uint32_t timeout_ms);

/**
 * @brief Register or replace the inbound-message callback.
 *
 * @param[in] mb    Channel handle.
 * @param[in] cb    Callback.  Pass NULL to detach.
 * @param[in] user  Opaque pointer forwarded to @p cb.
 * @return ALP_OK / ALP_ERR_NOT_READY.
 */
alp_status_t alp_mbox_set_callback(alp_mbox_t *mb,
                                   alp_mbox_msg_cb_t cb, void *user);

/** @brief Release the channel handle.  In-flight messages may be dropped. */
void         alp_mbox_close(alp_mbox_t *mb);

/* ------------------------------------------------------------------ */
/* Hardware semaphore                                                  */
/* ------------------------------------------------------------------ */

/** Opaque hardware-semaphore handle.  Allocate via @ref alp_hwsem_open. */
typedef struct alp_hwsem alp_hwsem_t;

/**
 * @brief Acquire a handle for a SoC hardware semaphore.
 *
 * Hardware semaphores are scarce — typically 4–16 per SoC.  The
 * @p hwsem_id corresponds to a SoC-specific block and must agree
 * with what the peer core uses.
 *
 * @param[in] hwsem_id  SoC-specific hardware-semaphore index.
 * @return Open handle on success, or NULL if the index is out of range.
 */
alp_hwsem_t *alp_hwsem_open(uint32_t hwsem_id);

/**
 * @brief Try to take the hwsem without blocking.
 *
 * @param[in] sem  Semaphore handle.
 * @return ALP_OK if the caller now owns the semaphore;
 *         ALP_ERR_BUSY if held by another core;
 *         ALP_ERR_NOT_READY / ALP_ERR_INVAL otherwise.
 */
alp_status_t alp_hwsem_try_lock(alp_hwsem_t *sem);

/**
 * @brief Block until the hwsem is owned, or @p timeout_ms elapses.
 *
 * @param[in] sem         Semaphore handle.
 * @param[in] timeout_ms  Max wait.  Use UINT32_MAX for unbounded.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT.
 */
alp_status_t alp_hwsem_lock(alp_hwsem_t *sem, uint32_t timeout_ms);

/** @brief Release ownership.  Must be called by the same core that locked. */
alp_status_t alp_hwsem_unlock(alp_hwsem_t *sem);

/** @brief Release the handle.  Doesn't release the lock — call unlock first. */
void         alp_hwsem_close(alp_hwsem_t *sem);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_MPROC_H */
