/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gpio.h
 * @brief CC3501E GPIO proxy host helpers (opcodes 0x50..0x53) + the
 *        portable GPIO proxy backend wiring.
 *
 * The CC3501E fronts a set of E1M pads (IO11/IO13/IO15..IO21); these
 * helpers let the host drive/read them over the inter-chip bridge.  @p pad
 * is the RAW CC3501E GPIO index (the firmware drives the pad 1:1, refusing
 * the bridge's own SPI/UART pads); the logical IO11.. -> raw-index map lives
 * in board metadata, not on the wire.  These ops are synchronous + fast in
 * the firmware (no worker), so they use the caller's @p timeout_ms with no
 * radio-down-window floor -- but they still retry on a transient IO (the
 * bridge is briefly down if a radio op is running concurrently).
 */

#ifndef ALP_CHIPS_CC3501E_GPIO_H
#define ALP_CHIPS_CC3501E_GPIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/chips/cc3501e/core.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure a proxied CC3501E GPIO's direction + internal pull
 *        (GPIO_CONFIGURE, 0x50).
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index (e.g. 13 for GPIO13).
 * @param dir         One of @ref alp_cc3501e_gpio_direction_t.  NOTE: the CC35xx
 *                    GPIO controller has no true open-drain, so DIR_OPEN_DRAIN is
 *                    emulated as a push-pull output idling high (fine on a single-
 *                    driver line; not on a shared line).
 * @param pull        One of @ref alp_cc3501e_gpio_pull_t.
 * @param timeout_ms  Caller budget (per-request retry on transient IO).
 * @return ALP_OK on success; ALP_ERR_INVAL if @p pad is reserved/out of
 *         range; ALP_ERR_NOT_READY on a firmware build without the proxy.
 */
alp_status_t cc3501e_gpio_configure(cc3501e_t                   *ctx,
                                    uint8_t                      pad,
                                    alp_cc3501e_gpio_direction_t dir,
                                    alp_cc3501e_gpio_pull_t      pull,
                                    uint32_t                     timeout_ms);

/**
 * @brief Drive a proxied CC3501E GPIO (GPIO_WRITE, 0x51).
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index.
 * @param level       false = low, true = high (open-drain: low asserts).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; otherwise the mapped error.
 */
alp_status_t cc3501e_gpio_write(cc3501e_t *ctx, uint8_t pad, bool level, uint32_t timeout_ms);

/**
 * @brief Sample a proxied CC3501E GPIO (GPIO_READ, 0x52).
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index.
 * @param level_out   Receives the sampled level (false/true).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p level_out is NULL or the
 *         pad is reserved; otherwise the mapped error.
 */
alp_status_t cc3501e_gpio_read(cc3501e_t *ctx, uint8_t pad, bool *level_out, uint32_t timeout_ms);

/**
 * @brief Arm/disable an edge interrupt on a proxied CC3501E GPIO
 *        (GPIO_SET_INTERRUPT, 0x53).
 *
 * The firmware arms the pad's HW edge interrupt; on this CS-less, no-host-IRQ
 * rev the async EVT_GPIO_INTERRUPT delivery has no slave->master attention
 * line, so the edge is latched on the CC3501E for a future poll/EVT path --
 * arming the controller is real, the host notification is deferred to the
 * next board rev.
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index.
 * @param edge        One of @ref alp_cc3501e_gpio_edge_t (NONE disables).  NOTE: the
 *                    CC35xx controller has no both-edges trigger, so EDGE_BOTH
 *                    returns ALP_ERR_INVAL -- arm RISING or FALLING.
 * @param enabled     false = disable, true = enable.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once armed/disabled; otherwise the mapped error.
 */
alp_status_t cc3501e_gpio_set_interrupt(cc3501e_t              *ctx,
                                        uint8_t                 pad,
                                        alp_cc3501e_gpio_edge_t edge,
                                        bool                    enabled,
                                        uint32_t                timeout_ms);

/* ------------------------------------------------------------------ */
/* Portable GPIO proxy wiring (CONFIG_ALP_SDK_GPIO_CC3501E_PROXY).     */
/*                                                                    */
/* When the proxy backend is built, alp_gpio_open(pin_id) on the AEN   */
/* target routes pin_ids listed in cc3501e_gpio_routes[] through the   */
/* bridge (cc3501e_gpio_*); every other pin_id delegates to the        */
/* platform GPIO driver.  The board provides the route table (filled   */
/* from the SoM pad map); the SDK ships a WEAK empty default so an      */
/* un-mapped build delegates every pin (no behaviour change).  The app */
/* calls alp_gpio_cc3501e_attach() once after cc3501e_init() so the     */
/* proxy has a bridge handle; until then proxied pins delegate too.    */
/* ------------------------------------------------------------------ */

/** One portable-pin -> raw CC3501E GPIO index route. */
typedef struct {
	uint32_t pin_id;    /**< portable alp_gpio_open() id (board-defined). */
	uint8_t  cc35_gpio; /**< raw CC3501E GPIO index the bridge drives. */
} cc3501e_gpio_route_t;

/** Board-provided route table (WEAK empty default in the proxy backend).
 *  Populate from the SoM pad map to enable proxied IOs. */
extern const cc3501e_gpio_route_t cc3501e_gpio_routes[];
extern const size_t               cc3501e_gpio_route_count;

/**
 * @brief Attach the live bridge handle to the GPIO proxy backend.
 *
 * Call once after cc3501e_init().  Without it (or with an empty route table)
 * every alp_gpio_open() pin delegates to the platform GPIO driver, so the
 * proxy is a no-op until both a bridge is attached and the route table is
 * populated.  Only defined when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is set.
 *
 * @param ctx  Initialised bridge handle.
 * @return ALP_OK; ALP_ERR_INVAL on a NULL @p ctx.
 */
alp_status_t alp_gpio_cc3501e_attach(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_GPIO_H */
