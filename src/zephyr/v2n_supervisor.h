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
 * GD32 supervisor through the portable <alp/*.h> surface, which the
 * V2N backends transparently dispatch via this singleton.  Direct
 * use of the gd32g553_t ctx from app code is the bridge-demo
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

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_INTERNAL_ZEPHYR_V2N_SUPERVISOR_H_ */
