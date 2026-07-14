/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal shared state for the `alp companion` command-group split
 * (#673 Phase 2): alp_console_companion.c (core) plus the
 * _wifi/_ble/_diag/_ota/_sock sibling TUs that register their commands
 * onto the (alp, companion) dynamic subcommand set.  alp_console_companion_gpio.c
 * (the V2N supervisor path) talks to the GD32 singleton via
 * "../v2n_supervisor.h" instead and needs neither symbol declared here.
 *
 * Not a public header -- do not install under include/alp.
 */
#ifndef ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_COMPANION_INTERNAL_H_
#define ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_COMPANION_INTERNAL_H_

#include <zephyr/kernel.h>

#include <alp/ext/cc3501e/console.h>

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)

/* App-registered CC3501E handle: set via alp_console_companion_set(),
 * defined (non-static) in alp_console_companion.c.  NULL until the app
 * registers one. */
extern cc3501e_t *companion_cc3501e;

/* Bridge-bus serialisation (Alif CC3501E), defined in
 * alp_console_companion.c.  The shell thread (every companion command
 * body across this split) and the async worker threads (the wifi
 * connect worker, the event-poll thread) all drive the inter-chip
 * bridge, and cc3501e_request is not internally locked -- two
 * concurrent transactions would interleave on the SPI bus and desync
 * the link.  Every bridge access from any companion TU takes this
 * mutex. */
extern struct k_mutex companion_bus_lock;

#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */

#endif /* ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_COMPANION_INTERNAL_H_ */
