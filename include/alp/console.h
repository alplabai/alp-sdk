/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SoM console -- application-facing hooks.
 */
#ifndef ALP_CONSOLE_H_
#define ALP_CONSOLE_H_

#include <alp/chips/cc3501e.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind the CC3501E companion the `alp companion` command talks to.
 *
 * On Alif there is no companion singleton, so the application opens its
 * CC3501E (cc3501e_init) and registers the handle here once.  No-op on
 * SoMs whose companion is a singleton (V2N binds the GD32 supervisor
 * automatically).  Pass NULL to unbind.
 *
 * @param ctx  Initialised CC3501E context, or NULL.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.9 new; companion-console binder tracking the [ABI-EXPERIMENTAL]
 *      CC3501E companion surface it depends on.  See docs/abi-markers.md.
 */
void alp_console_companion_set(cc3501e_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CONSOLE_H_ */
