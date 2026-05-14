/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal: lazy-initialised singleton wrapping the GD32G553 host
 * driver context for V2N modules.  Backends that need to dispatch a
 * portable peripheral call through the GD32 bridge
 * (peripheral_pwm.c, peripheral_adc.c, peripheral_qenc.c,
 * peripheral_counter.c, peripheral_dac.c) acquire / release the
 * singleton around each transport call.  The acquire helper takes a
 * shared mutex for the duration of the caller's bridge op so the
 * gd32g553_t context's documented "caller must serialise" contract
 * is honoured without forcing every backend to maintain its own
 * lock.
 *
 * This header is SDK-internal.  Application code MUST reach the
 * GD32 supervisor through the portable `<alp/pwm.h>` / `<alp/adc.h>`
 * / `<alp/counter.h>` headers (the `<alp>` peripheral surface),
 * which the V2N backends transparently dispatch via this singleton.
 * Direct use of the gd32g553_t ctx from app code is the bridge-demo
 * pattern (examples/v2n-gd32-bridge-ping/) -- a different code
 * path that opens its own buses.
 */

#ifndef ALP_INTERNAL_ZEPHYR_V2N_SUPERVISOR_H_
#define ALP_INTERNAL_ZEPHYR_V2N_SUPERVISOR_H_

#include "alp/chips/gd32g553.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Acquire a pointer to the singleton GD32G553 supervisor ctx.
 *
 * Lazy-initialises on first call: opens the configured SPI + I2C
 * buses, runs the GD32 handshake (PING + GET_VERSION), and caches
 * the result.  Subsequent calls re-use the cached state.
 *
 * The supervisor mutex is held from a successful acquire until the
 * matching release; callers MUST issue exactly one alp_z_v2n_supervisor_release()
 * per ALP_OK return.  Failed acquires (any non-OK return) leave the
 * mutex unlocked and MUST NOT be released.
 *
 * @param[out] ctx_out  Populated on @ref ALP_OK with the address of
 *                      the singleton context.  Untouched otherwise.
 * @return ALP_OK on success;
 *         ALP_ERR_NOSUPPORT if the supervisor is not compiled in;
 *         ALP_ERR_NOT_READY if no bus is configured or the bus
 *           open() failed;
 *         any status from @ref gd32g553_init otherwise (mismatched
 *           firmware version, transport error during the handshake).
 */
alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out);

/**
 * @brief Release the supervisor mutex.  Pairs with a successful
 *        @ref alp_z_v2n_supervisor_acquire.  No-op when the
 *        supervisor is not compiled in.
 */
void alp_z_v2n_supervisor_release(void);

/**
 * @brief Acquire the BRD_I²C bus handle held by the supervisor.
 *
 * V2N's BRD_I²C is a shared bus: the GD32 bridge uses it for slow-path
 * management commands, and the DA9292 PMIC + future BRD-side
 * peripherals share the same physical lines.  This helper hands out
 * the supervisor's cached `alp_i2c_t *` under the same mutex used by
 * @ref alp_z_v2n_supervisor_acquire so a transfer in flight on either
 * consumer can't be interrupted by the other.
 *
 * Pairs with @ref alp_z_v2n_supervisor_brd_i2c_release exactly the
 * way the GD32 acquire/release pair does: exactly one release per
 * ALP_OK return.  Failed acquires (any non-OK return) leave the
 * mutex unlocked and MUST NOT be released.
 *
 * Lock semantics: GD32 acquire and BRD_I²C acquire share a single
 * mutex.  Holding the BRD_I²C lock blocks the GD32 bridge dispatch
 * and vice versa -- by design, since both transfer over the same
 * physical bus.
 *
 * @param[out] i2c_out  Populated on @ref ALP_OK with the supervisor's
 *                      `alp_i2c_t *` handle.  Untouched otherwise.
 * @return ALP_OK on success;
 *         ALP_ERR_NOSUPPORT if the supervisor is not compiled in
 *           or its I²C bus is not configured (Kconfig
 *           `*_I2C_BUS_ID < 0`);
 *         ALP_ERR_BUSY if the mutex couldn't be taken within the
 *           Kconfig-configured timeout;
 *         ALP_ERR_NOT_READY if no bus opened during lazy init.
 */
alp_status_t alp_z_v2n_supervisor_brd_i2c_acquire(alp_i2c_t **i2c_out);

/**
 * @brief Release the BRD_I²C bus handle.  Pairs with a successful
 *        @ref alp_z_v2n_supervisor_brd_i2c_acquire.  No-op when the
 *        supervisor is not compiled in.
 */
void alp_z_v2n_supervisor_brd_i2c_release(void);

/**
 * @brief Invalidate the cached supervisor state so the next
 *        acquire re-runs the bus open + GD32 handshake.
 *
 * Used by the @c <alp/power.h> path after a deep-sleep -> wakeup
 * cycle: the GD32 may have been reset or the SPI / I2C transport
 * may have stalled during the sleep period, so the next bridge
 * call MUST start from a clean slate rather than reusing the
 * cached `gd32g553_t` context.  Closes any open bus handles and
 * clears the latch so a subsequent `acquire()` does the work
 * lazily on its first call.
 *
 * Safe to call any time; takes the supervisor mutex internally.
 * No-op when the supervisor isn't compiled in (the
 * `!CONFIG_ALP_SDK_V2N_SUPERVISOR` stub at the bottom of
 * `v2n_supervisor.c`).
 *
 * This is an internal SDK API.  Application code MUST NOT call
 * it directly -- the `<alp/power.h>` wake handler is the
 * authoritative caller once the wake path lands.
 */
void alp_z_v2n_supervisor_invalidate(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_INTERNAL_ZEPHYR_V2N_SUPERVISOR_H_ */
