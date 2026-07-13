/**
 * @file ext/cc3501e/console.h
 * @brief CC3501E companion binder for the `alp companion` console command.
 *
 * Non-portable.  Include only when you've committed to a CC3501E
 * companion (E1M-AEN and other Alif SoMs that pair the TI/Puya
 * CC3501E Wi-Fi/BLE coprocessor).  This surface intentionally lives
 * under @c include/alp/ext/ so the root @c <alp/console.h> stays
 * chip-neutral: the portable console command group is registered
 * automatically, and only the companion attach hook is
 * chip-specific.
 *
 * SoMs whose companion is a singleton (V2N binds the GD32 supervisor
 * automatically) do not need this header.
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ALP_EXT_CC3501E_CONSOLE_H_
#define ALP_EXT_CC3501E_CONSOLE_H_

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
 * @par Supported silicon: alif:ensemble:e8 (E1M-AEN CC3501E companion);
 *      extends to any SoM that pairs a CC3501E coprocessor.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.9 new; companion-console binder tracking the [ABI-EXPERIMENTAL]
 *      CC3501E companion surface it depends on.  See docs/abi-markers.md.
 */
void alp_console_companion_set(cc3501e_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_CC3501E_CONSOLE_H_ */
