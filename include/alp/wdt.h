/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wdt.h
 * @brief ALP SDK watchdog abstraction.
 *
 * Backends:
 *   - Zephyr   : `wdt_*` driver class.
 *   - Yocto    : `/dev/watchdogN` ioctl.
 *   - Baremetal: vendor HAL WDT peripheral.
 *
 * Most SoCs disable the watchdog after a feed-miss reset so the boot
 * code can choose whether to re-arm it; the ALP wrapper does **not**
 * mask that — apps are expected to know whether they re-installed the
 * timeout after a recovery boot.
 *
 * Typical usage:
 * @code
 *     alp_wdt_t *wdt = alp_wdt_open(0, &(alp_wdt_config_t){
 *         .timeout_ms = 5000,
 *         .on_timeout = ALP_WDT_RESET_SOC,
 *     });
 *     while (running) {
 *         do_work();
 *         alp_wdt_feed(wdt);    // every iteration; must be < 5 s
 *     }
 * @endcode
 */

#ifndef ALP_WDT_H
#define ALP_WDT_H

#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What happens when the watchdog fires. */
typedef enum {
    ALP_WDT_RESET_SOC      = 0,   /**< Full SoC reset (default; safest). */
    ALP_WDT_RESET_CPU      = 1,   /**< Core reset only — peripherals keep state. */
    ALP_WDT_INTERRUPT_ONLY = 2    /**< Generate an IRQ; no reset. */
} alp_wdt_action_t;

/** Opaque watchdog handle.  Allocate via @ref alp_wdt_open. */
typedef struct alp_wdt alp_wdt_t;

/** Configuration passed to @ref alp_wdt_open. */
typedef struct {
    uint32_t          timeout_ms;
    alp_wdt_action_t  on_timeout;
} alp_wdt_config_t;

/**
 * @brief Install a watchdog timeout and arm the timer.
 *
 * The watchdog starts feeding-required immediately on return.  Apps
 * must call @ref alp_wdt_feed before @c timeout_ms elapses or the
 * configured @c on_timeout action triggers.
 *
 * @param[in] wdt_id  Studio-resolved watchdog index (0..1).
 * @param[in] cfg     Configuration.  Must be non-NULL with non-zero
 *                    @c timeout_ms.
 * @return Open handle on success;
 *         NULL if @p cfg is invalid, @p wdt_id is out of range, the
 *         underlying device isn't ready, or the SoC rejected the
 *         requested timeout (too long for the hardware).
 */
alp_wdt_t   *alp_wdt_open(uint32_t wdt_id, const alp_wdt_config_t *cfg);

/**
 * @brief Reset the watchdog timer.
 *
 * Apps must feed faster than @c timeout_ms or the configured action
 * triggers.  Cheap call — typically a single MMIO write.
 *
 * @param[in] wdt  Handle from @ref alp_wdt_open.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_wdt_feed(alp_wdt_t *wdt);

/**
 * @brief Disable the watchdog if the SoC supports it.
 *
 * Many M-class watchdogs are write-once-armed and cannot be disabled
 * without a reset.  @ref ALP_ERR_NOSUPPORT is the expected return on
 * such hardware; the caller can treat it as informational.
 *
 * @param[in] wdt  Handle from @ref alp_wdt_open.
 * @return ALP_OK on success;
 *         ALP_ERR_NOSUPPORT if the SoC's WDT is one-shot;
 *         ALP_ERR_NOT_READY if @p wdt is closed.
 */
alp_status_t alp_wdt_disable(alp_wdt_t *wdt);

/** @brief Best-effort disable, then release the handle.  NULL is a no-op. */
void         alp_wdt_close(alp_wdt_t *wdt);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_WDT_H */
