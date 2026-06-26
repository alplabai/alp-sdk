/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SoM console -- application-facing hooks.
 */

/**
 * @file console.h
 * @brief Application-facing hooks for the Alp SoM interactive console.
 *
 * The built-in console (the `alp` command shell) talks to a companion
 * controller for some commands (`alp companion`).  How that companion is
 * discovered differs per SoM: on SoMs with a singleton supervisor (e.g. the
 * V2N's GD32) it is bound automatically, while SoMs without one (e.g. Alif)
 * must register their CC3501E handle explicitly via the hook below.
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
 */
void alp_console_companion_set(cc3501e_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CONSOLE_H_ */
