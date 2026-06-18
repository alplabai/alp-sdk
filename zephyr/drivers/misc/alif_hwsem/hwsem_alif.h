/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 Alp Lab AB
 *
 * Public take/give API for the Alif Ensemble hardware-semaphore (HWSEM)
 * driver (zephyr/drivers/misc/alif_hwsem/hwsem_alif.c, compatible
 * "alif,hwsem"). HWSEM is an AMP mutual-exclusion latch shared between the two
 * RTSS cores (M55-HE <-> M55-HP); it has NO upstream Zephyr device class, so
 * this driver exposes its own minimal API rather than a class API. Resolve an
 * instance with DEVICE_DT_GET(DT_NODELABEL(hwsemN)) and pass the device here.
 *
 * vendor-ext, BENCH-UNVERIFIED. See the binding
 * zephyr/dts/bindings/misc/alif,hwsem.yaml.
 */

#ifndef ZEPHYR_DRIVERS_MISC_ALIF_HWSEM_HWSEM_ALIF_H_
#define ZEPHYR_DRIVERS_MISC_ALIF_HWSEM_HWSEM_ALIF_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Take (acquire) a HWSEM instance for the calling core.
 *
 * Writes @p master_id to the instance's request register (REQ +0x00) and reads
 * it back: the latch grants ownership to the FIRST master that requests it and
 * holds it until that same master releases an equal number of times (the latch
 * is a counting one -- it increments per request by the owner and the readback
 * returns the current owner's id). Ownership is granted iff the readback equals
 * @p master_id.
 *
 * Non-blocking: a single request attempt. To spin until acquired, call this in
 * a bounded retry loop (see hwsem_alif_take_busy()).
 *
 * @param dev       HWSEM device, e.g. DEVICE_DT_GET(DT_NODELABEL(hwsem0)).
 * @param master_id The calling core's MASTER ID (its CPUID; the same value MUST
 *                  be passed to the matching hwsem_alif_give()). HE and HP MUST
 *                  use DISTINCT ids or the latch cannot tell them apart.
 *
 * @retval 0        Ownership granted (readback == @p master_id).
 * @retval -EBUSY   Another master owns the latch (readback != @p master_id).
 * @retval -EINVAL  @p dev is NULL or not ready.
 */
int hwsem_alif_take(const struct device *dev, uint32_t master_id);

/**
 * @brief Spin (bounded) until a HWSEM instance is taken, or give up.
 *
 * Convenience wrapper that retries hwsem_alif_take() up to @p max_spins times.
 * Each failed attempt is a peer-owned latch; the spin lets the peer core
 * release. Bounded so a dead/absent peer cannot hang the caller.
 *
 * @param dev        HWSEM device.
 * @param master_id  The calling core's MASTER ID.
 * @param max_spins  Maximum request attempts (>=1).
 *
 * @retval 0        Ownership granted within @p max_spins attempts.
 * @retval -EBUSY   Still peer-owned after @p max_spins attempts.
 * @retval -EINVAL  @p dev is NULL/not ready, or @p max_spins is 0.
 */
int hwsem_alif_take_busy(const struct device *dev, uint32_t master_id, uint32_t max_spins);

/**
 * @brief Give (release) a HWSEM instance held by the calling core.
 *
 * Writes @p master_id to the instance's release register (REL +0x04). The latch
 * decrements its count for that master; when the count reaches zero the latch is
 * free for any master to take. A release with the WRONG @p master_id (a master
 * that does not own the latch) has no effect on the hardware.
 *
 * @param dev       HWSEM device.
 * @param master_id The calling core's MASTER ID -- MUST match the value passed
 *                  to the paired hwsem_alif_take().
 *
 * @retval 0        Release issued.
 * @retval -EINVAL  @p dev is NULL or not ready.
 */
int hwsem_alif_give(const struct device *dev, uint32_t master_id);

/**
 * @brief Read the current ownership count of a HWSEM instance.
 *
 * Reads the release register (REL +0x04), which reflects the latch's current
 * count: 0 when free, nonzero while owned. Useful for a smoke test to confirm a
 * take incremented and a give decremented the count.
 *
 * @param dev   HWSEM device.
 * @param count Out: the current count.
 *
 * @retval 0        @p count populated.
 * @retval -EINVAL  @p dev is NULL/not ready, or @p count is NULL.
 */
int hwsem_alif_get_count(const struct device *dev, uint32_t *count);

/**
 * @brief Force-reset a HWSEM instance to the free state.
 *
 * Writes 1 to the reset register (RST +0x08), clearing the count and any
 * ownership regardless of master. Use only for recovery (e.g. a peer crashed
 * holding the latch); not part of normal take/give flow.
 *
 * @param dev   HWSEM device.
 *
 * @retval 0        Reset issued.
 * @retval -EINVAL  @p dev is NULL or not ready.
 */
int hwsem_alif_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_MISC_ALIF_HWSEM_HWSEM_ALIF_H_ */
