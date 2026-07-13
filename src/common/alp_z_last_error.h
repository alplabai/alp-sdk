/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private contract for the shared class dispatchers' (src/<class>_dispatch.c,
 * src/backends/uart/zephyr_drv.c, src/common/alp_model_loader.c)
 * thread-local last-error hooks.
 *
 * The dispatchers are written OS-agnostic -- compiled into both the
 * Zephyr build and, since the RTC/WDT registry migration (#33), the
 * Yocto build -- so they can't include src/zephyr/handles.h (which
 * pulls in <zephyr/kernel.h> and is Zephyr-only).  Exactly one OS
 * backend supplies the definitions:
 *
 *   - src/zephyr/last_error.c on Zephyr (__thread-backed when
 *     CONFIG_THREAD_LOCAL_STORAGE is set; see that file for the
 *     single-thread / build-warning fallback otherwise).
 *   - src/yocto/dispatch_last_error_shim.c on the Yocto plain-CMake
 *     build, forwarding onto alp_internal_set_last_error() -- the one
 *     ALP_LAST_ERROR_TLS-qualified slot in src/common/stub/stub_core.c
 *     (see src/common/alp_internal.h).
 *
 * Every caller includes this header instead of hand-writing its own
 * `extern` prototypes for the two symbols below (issue #627).
 */

#ifndef ALP_Z_LAST_ERROR_H_
#define ALP_Z_LAST_ERROR_H_

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Stamp the calling thread's last-error slot. */
void alp_z_set_last_error(alp_status_t s);

/** @brief Clear the calling thread's last-error slot (set it to @ref ALP_OK). */
void alp_z_clear_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_Z_LAST_ERROR_H_ */
