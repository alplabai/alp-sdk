/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mproc.h
 * @brief Alp SDK multi-processor IPC primitives.
 *
 * v0.3 deliverable; v0.7 wires the Zephyr backend.  As of v0.7 the
 * Zephyr backend implements all three primitives (mailbox, shared
 * memory, hardware semaphore).  The Yocto + GD32 backends still
 * return ALP_ERR_NOSUPPORT for every entry point and are tracked
 * separately.
 *
 * @par Layering
 * This header is the LOW-LEVEL primitive layer.  Use it when the app
 * needs raw shared memory, a specific mailbox channel, or a hardware
 * semaphore -- e.g. lock-free SPSC ring buffers between Alif's
 * M55-HP and M55-HE, or a tight latency-bounded mailbox handshake.
 *
 * For HIGH-LEVEL portable IPC across SoMs (Yocto Linux on the A-cluster
 * talking to Zephyr on the M-class peer) prefer `<alp/rpc.h>`.  That
 * surface carries OpenAMP / RPMsg framing, looks up endpoint IDs +
 * mailbox channels + carve-out addresses from the build-time-generated
 * `<alp/system_ipc.h>`, and works identically on every heterogeneous
 * SoM (AEN E5+, V2N, V2M, NX9).  rpc.h is the right answer for almost
 * every "two cores talk to each other" use case; mproc.h is the
 * escape hatch when you need to bypass RPMsg overhead.
 *
 * @par Scope
 * The three primitives are intended to cover every E1M SoM:
 *   1. Shared-memory regions      — `alp_shmem_*`
 *   2. Mailbox channels           — `alp_mbox_*`  (Zephyr `mbox_*`)
 *   3. Hardware semaphores        — `alp_hwsem_*` (Zephyr `hwsem_*`)
 *
 * The @ref alp_core_id_t enum enumerates every core that appears in
 * any shipped SoM preset's `topology:` block, so apps can address
 * peers on AEN (M55-HP / M55-HE / A32 cluster), V2N (M33-SM / A55
 * cluster), V2M (same as V2N), or NX9 (M33 / A55 cluster) through
 * the same surface.  Adding a new SoM SKU MUST extend the enum and
 * the test in `tests/scripts/test_core_id_enum_coverage.py` enforces
 * that.
 *
 * @par Implementation status
 *   - `alp_mbox_*` -- Zephyr backend functional since v0.4 (DT-anchored
 *     MBOX devices via the `alp-mboxN` aliases, with optional nanopb
 *     framing).  Yocto backend is a shim only; GD32 backend is
 *     NOSUPPORT.
 *   - `alp_shmem_*` -- Zephyr backend functional since v0.7 via the
 *     DT aliases `alp-shmem0..N` (commit 2e9deec).  Yocto + GD32
 *     backends still return ALP_ERR_NOSUPPORT.
 *   - `alp_hwsem_*` -- Zephyr backend functional since v0.7 with an
 *     intra-core `k_sem` fallback (commit 877fa29);
 *     `CONFIG_ALP_SDK_MPROC_HWSEM_COUNT` controls the pool size
 *     (default 16).  Real per-SoC HWSEM blocks (AEN HWSEM, ST HSEM,
 *     etc.) are wired up under Track 1 HiL bring-up.  Yocto + GD32
 *     backends still return ALP_ERR_NOSUPPORT.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.3 mailbox + shmem + hwsem.  The @ref alp_core_id_t enum is
 *      extended (not reshaped) as new SoMs land; existing values
 *      keep their integer assignments forever.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_MPROC_H
#define ALP_MPROC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/cap_instance.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Identifies a peer core in the MP topology.
 *
 *  Names mirror the canonical core_ids used in each SoM preset's
 *  `topology:` block at `metadata/e1m_modules/E1M-<MPN>.yaml`.  The
 *  integer assignments are stable across Alp SDK releases: adding a
 *  new SoM appends entries with fresh integer values; it never
 *  re-numbers existing ones.  The coverage test in
 *  `tests/scripts/test_core_id_enum_coverage.py` enforces that every
 *  topology core_id has a matching enum entry here.
 *
 *  **Alif HP / HE distinction.**  Alif Ensemble's M55-HP (400 MHz,
 *  Helium + the application workload) and M55-HE (160 MHz, always-on
 *  low-power) are genuinely different cores; preserving them as
 *  distinct enum entries keeps the firmware author in control of
 *  which peer they're addressing -- routing "always-on" work to
 *  M55-HE preserves the AEN family's headline low-power story.
 *
 *  **Cross-SoM portability.**  For apps that need to be SoM-agnostic
 *  (Yocto consumer talks to Zephyr producer regardless of which
 *  M-class core it lives on), prefer `<alp/rpc.h>` -- it looks up
 *  the peer endpoint via the board.yaml-declared channel name, not
 *  via this enum.  Use `alp_core_id_t` when the app is explicitly
 *  SoM-scoped and wants to talk to a specific physical core. */
typedef enum {
	ALP_CORE_SELF = 0, /**< Whichever core this code is running on. */

	/* ---- Alif Ensemble (AEN family) ---- */
	ALP_CORE_M55_HP      = 1, /**< AEN High-Performance Cortex-M55 (400 MHz). */
	ALP_CORE_M55_HE      = 2, /**< AEN High-Efficiency Cortex-M55 (160 MHz, always-on). */
	ALP_CORE_A32_0       = 3, /**< AEN Cortex-A32 #0 on E5/E6/E7/E8 (Linux). */
	ALP_CORE_A32_1       = 4, /**< AEN Cortex-A32 #1 on E7/E8 (Linux). */
	ALP_CORE_A32_CLUSTER = 5, /**< AEN Cortex-A32 cluster as a single endpoint
                                     (E5/E6/E7/E8 -- matches `a32_cluster` in
                                     the AEN5+ topology blocks). */

	/* ---- Renesas RZ/V2N (V2N + V2M families) ---- */
	ALP_CORE_M33_SM      = 6, /**< V2N Cortex-M33 system-manager
                                     (matches `m33_sm` in V2N101/V2M101
                                     topology). */
	ALP_CORE_A55_CLUSTER = 7, /**< V2N / NX9 Cortex-A55 cluster as a single
                                     endpoint (matches `a55_cluster`). */

	/* ---- NXP i.MX 93 (NX9 family) ---- */
	ALP_CORE_M33 = 8 /**< NX9 Cortex-M33 (no _sm suffix -- NX9's
                                     M33 doesn't carry the system-manager
                                     role; matches `m33` in NX9101 topology). */
} alp_core_id_t;

/* ------------------------------------------------------------------ */
/* Peer-core lifecycle                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Release (boot) a peer core at a given entry address.
 *
 * On heterogeneous SoMs the firmware for a secondary core is often
 * LOADED at boot (by the boot ROM / secure firmware, from the boot
 * package) but not RELEASED -- the master core decides at runtime
 * when the peer starts.  This call asks the platform's boot authority
 * (typically the SoC's secure / system-controller firmware) to start
 * @p core executing at @p entry_addr.
 *
 * @p entry_addr is the peer's entry point in the GLOBAL address map
 * -- the same load address the boot package declares for that core's
 * image (e.g. the ITCM global alias the image was packaged for).  It
 * comes from the application's boot-package layout; this API performs
 * no address validation beyond what the boot authority enforces.
 *
 * Bounded: backends riding a controller mailbox time-limit the
 * round-trip, so the call returns rather than hangs when the
 * controller is unreachable.  A successful return means the boot
 * authority ACCEPTED the request -- confirming the peer actually runs
 * is the application's business (heartbeat/beacon, or open an IPC
 * channel via `<alp/rpc.h>`).
 *
 * @param[in] core        Peer to start.  @ref ALP_CORE_SELF is
 *                        invalid.
 * @param[in] entry_addr  Entry point in the global address map.
 *
 * @return  @ref ALP_OK when the boot authority accepted the request.
 *          @ref ALP_ERR_INVAL for @ref ALP_CORE_SELF.
 *          @ref ALP_ERR_NOSUPPORT when this build has no boot
 *                                 authority for @p core (wrong SoM,
 *                                 native_sim, or a core the platform
 *                                 boots by other means).
 *          @ref ALP_ERR_NOT_READY when the boot authority is
 *                                 asleep/unreachable (retryable).
 *          @ref ALP_ERR_IO on a transport fault or a rejected
 *                          request.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.9 -- portable peer-core release (first consumers:
 *      the AEN dual-core examples).
 */
alp_status_t alp_mproc_boot_core(alp_core_id_t core, uintptr_t entry_addr);

/* ------------------------------------------------------------------ */
/* Shared memory                                                       */
/* ------------------------------------------------------------------ */

/** Opaque shared-memory region.  Allocate via @ref alp_shmem_open. */
typedef struct alp_shmem alp_shmem_t;

/** Configuration for a shared-memory region. */
typedef struct {
	const char *name;      /**< Region name shared across cores (DT-anchored). */
	size_t      size;      /**< Required bytes; rounded up to MMU/MPU page. */
	bool        cacheable; /**< false ⇒ allocate non-cacheable; required for
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
 *
 * @note The region pool is built at compile time from the DT aliases
 *       @c alp-shmem0 .. @c alp-shmemN; @p cfg->name must match the
 *       alias-derived label (e.g. @c "alp_shmem0").  Aliases that
 *       aren't defined for the active board simply don't contribute
 *       entries.  On lookup miss the call returns NULL with
 *       @c alp_last_error() == @c ALP_ERR_NOT_READY.
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
void alp_shmem_close(alp_shmem_t *s);

/**
 * @brief Query the capabilities of an opened shared-memory handle.
 *
 * @param[in] s  Handle from @ref alp_shmem_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p s is NULL.
 */
const alp_capabilities_t *alp_shmem_capabilities(const alp_shmem_t *s);

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
typedef void (*alp_mbox_msg_cb_t)(uint32_t channel, const void *data, size_t len, void *user);

/** Mailbox-channel configuration. */
typedef struct {
	uint32_t      channel; /**< MHU/Mailbox channel index per the SoC manifest. */
	alp_core_id_t peer;    /**< Counterpart core. */
} alp_mbox_config_t;

/**
 * @brief Acquire a mailbox-channel handle.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 * @return Open handle on success, or NULL on resolution failure.
 */
alp_mbox_t *alp_mbox_open(const alp_mbox_config_t *cfg);

/**
 * @brief Send @p len bytes to the peer core.
 *
 * @param[in] mb          Channel handle.
 * @param[in] data        Payload.
 * @param[in] len         Length, ≤ MHU MTU (typically 16–64 B).
 * @param[in] timeout_ms  Max wait for peer to drain its inbox.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT.
 */
alp_status_t alp_mbox_send(alp_mbox_t *mb, const void *data, size_t len, uint32_t timeout_ms);

/**
 * @brief Register or replace the inbound-message callback.
 *
 * @param[in] mb    Channel handle.
 * @param[in] cb    Callback.  Pass NULL to detach.
 * @param[in] user  Opaque pointer forwarded to @p cb.
 * @return ALP_OK / ALP_ERR_NOT_READY.
 */
alp_status_t alp_mbox_set_callback(alp_mbox_t *mb, alp_mbox_msg_cb_t cb, void *user);

/** @brief Release the channel handle.  In-flight messages may be dropped. */
void alp_mbox_close(alp_mbox_t *mb);

/**
 * @brief Query the capabilities of an opened mailbox channel handle.
 *
 * @param[in] mb  Handle from @ref alp_mbox_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p mb is NULL.
 */
const alp_capabilities_t *alp_mbox_capabilities(const alp_mbox_t *mb);

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
 *
 * @note The Zephyr backend's intra-core fallback caps @p hwsem_id at
 *       @c CONFIG_ALP_SDK_MPROC_HWSEM_COUNT (default 16); passing an
 *       id at or above the cap returns NULL with
 *       @c alp_last_error() == @c ALP_ERR_OUT_OF_RANGE.  The fallback
 *       serialises within a single Zephyr image only -- a real
 *       per-SoC HWSEM block (cross-core) is wired up in a follow-on
 *       track.
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
 * @param[in] timeout_ms  Max wait in milliseconds.  The sentinel value
 *                        @c UINT32_MAX means "wait indefinitely"
 *                        (maps to Zephyr's @c K_FOREVER on the
 *                        Zephyr backend); a value of @c 0 returns
 *                        immediately if the semaphore isn't free.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT.
 */
alp_status_t alp_hwsem_lock(alp_hwsem_t *sem, uint32_t timeout_ms);

/**
 * @brief Release ownership.  Must be called by the same core that locked.
 *
 * @param[in] sem  Semaphore handle previously locked by this core.
 * @return ALP_OK on successful release;
 *         ALP_ERR_NOT_READY if @p sem is NULL or no longer in use
 *         (e.g. already closed);
 *         ALP_ERR_INVAL if @p sem is a valid handle but the caller
 *         isn't currently holding the lock (catches double-unlock
 *         and unlock-without-lock bugs).
 */
alp_status_t alp_hwsem_unlock(alp_hwsem_t *sem);

/**
 * @brief Release the handle back to the pool.
 *
 * @param[in] sem  Handle from @ref alp_hwsem_open, or NULL.
 *
 * @note If @p sem is still holding the lock at close time the
 *       backend defensively gives the kobj back, so a leaked-lock
 *       close doesn't permanently strand the underlying hwsem id.
 *       Callers should still pair every successful lock with an
 *       explicit @ref alp_hwsem_unlock; the defensive release is a
 *       safety net, not a substitute.
 */
void alp_hwsem_close(alp_hwsem_t *sem);

/**
 * @brief Query the capabilities of an opened hardware-semaphore handle.
 *
 * @param[in] sem  Handle from @ref alp_hwsem_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p sem is NULL.
 */
const alp_capabilities_t *alp_hwsem_capabilities(const alp_hwsem_t *sem);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_MPROC_H */
