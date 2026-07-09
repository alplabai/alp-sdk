/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SoM console -- application-facing hooks.
 *
 * The portable `alp` Zephyr-shell command group (board / gpio / i2c /
 * adc / pwm / mem / clk / companion) is registered automatically on any
 * console-enabled build; applications need not call anything to get it.
 *
 * This root header is deliberately CHIP-NEUTRAL: it pulls in no
 * `<alp/chips/...>` surface so the public console API stays portable
 * across AEN, V2N/GD32, i.MX93, and future companions.  Companion
 * attach is companion-specific and lives behind an extension header:
 * to bind a CC3501E companion (Alif SoMs), include
 * <alp/ext/cc3501e/console.h>.  SoMs whose companion is a singleton
 * (e.g. the V2N GD32 supervisor) bind it automatically.
 */
#ifndef ALP_CONSOLE_H_
#define ALP_CONSOLE_H_

#ifdef __cplusplus
extern "C" {
#endif

/* No portable symbols yet: the console command group self-registers and
 * companion binding is chip-specific (see <alp/ext/<companion>/console.h>). */

#ifdef __cplusplus
}
#endif

#endif /* ALP_CONSOLE_H_ */
