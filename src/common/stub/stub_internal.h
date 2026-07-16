/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private cross-TU contract for the split stub backend (the
 * src/common/stub/ sources -- one translation unit per public API
 * class, replacing the single src/common/stub_backend.c monolith;
 * see issue #673).
 *
 * Every public `alp_*` NOSUPPORT stub lives in one of the sibling
 * `stub_<class>.c` files in this directory.  This header carries the
 * two things more than one of those TUs needs:
 *
 *   - `z_last_error`: the single canonical last-error slot.
 *     `stub_core.c` owns the definition; every other TU that stamps
 *     a failure directly (same as the monolith did) reaches it
 *     through this `extern` declaration, so the write stays the
 *     exact same statement it always was rather than routing through
 *     `alp_internal_set_last_error()`.
 *   - the `ALP_VENDOR_OVERRIDES_PERIPHERAL` umbrella-to-per-class
 *     forwarding (`I2C`/`SPI`/`GPIO`/`UART`), needed by every one of
 *     those four class TUs.
 */

#ifndef ALP_STUB_INTERNAL_H_
#define ALP_STUB_INTERNAL_H_

#include "alp/peripheral.h"

#include "../alp_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single canonical last-error slot -- defined in stub_core.c. */
extern ALP_LAST_ERROR_TLS alp_status_t z_last_error;

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ------------------------------------------------------------------ */
/* I2C / SPI / GPIO / UART (peripheral.h)                              */
/*                                                                     */
/* Each class is independently gateable so a backend can override one  */
/* class (e.g. Yocto I2C against /dev/i2c-N) while the others fall     */
/* back to NOSUPPORT.  The umbrella `ALP_VENDOR_OVERRIDES_PERIPHERAL`  */
/* macro is preserved for backends that provide all four at once      */
/* (vendors/alif/ etc.); it implies every per-class macro below.      */
/* ------------------------------------------------------------------ */

#if defined(ALP_VENDOR_OVERRIDES_PERIPHERAL)
#define ALP_VENDOR_OVERRIDES_I2C  1
#define ALP_VENDOR_OVERRIDES_SPI  1
#define ALP_VENDOR_OVERRIDES_GPIO 1
#define ALP_VENDOR_OVERRIDES_UART 1
#endif

#endif /* ALP_STUB_INTERNAL_H_ */
