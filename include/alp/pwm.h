/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pwm.h
 * @brief ALP SDK pulse-width-modulation abstraction.
 *
 * The E1M standard reserves eight PWM channels (`PWM0..PWM7` in
 * `<alp/e1m_pinout.h>`).  Every E1M-conformant SoM routes them; the
 * studio's pin allocator binds them to the SoC's underlying timer/
 * compare blocks.  Apps see a uniform `alp_pwm_*` surface.
 *
 * Backends:
 *   - Zephyr   : `pwm_*` driver class.
 *   - Yocto    : `/sys/class/pwm/` sysfs interface.
 *   - Baremetal: vendor HAL timer compare-output channels.
 *
 * Typical usage:
 * @code
 *     alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
 *         .channel_id = 0,                     // PWM0
 *         .period_ns  = 1000000,               // 1 kHz
 *         .polarity   = ALP_PWM_POLARITY_NORMAL,
 *     });
 *     alp_pwm_set_duty(led, 500000);           // 50 % duty
 *     // ...
 *     alp_pwm_close(led);
 * @endcode
 */

#ifndef ALP_PWM_H
#define ALP_PWM_H

#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Output polarity (which level represents the "on" portion of the cycle). */
typedef enum {
    ALP_PWM_POLARITY_NORMAL   = 0,    /**< High during the active portion. */
    ALP_PWM_POLARITY_INVERTED = 1     /**< Low during the active portion. */
} alp_pwm_polarity_t;

/** Opaque PWM channel handle.  Allocate via @ref alp_pwm_open. */
typedef struct alp_pwm alp_pwm_t;

/** Configuration passed to @ref alp_pwm_open. */
typedef struct {
    uint32_t            channel_id;   /**< Studio-resolved PWM channel index (0..7). */
    uint32_t            period_ns;    /**< PWM period in nanoseconds. 0 = use DT default. */
    alp_pwm_polarity_t  polarity;
} alp_pwm_config_t;

/**
 * @brief Acquire and start a PWM channel at 0 % duty.
 *
 * Resolves @p cfg->channel_id via the `alp-pwm<N>` devicetree alias,
 * borrows a free handle from the SDK's pool, and primes the hardware
 * with the requested period.  The output stays low / inactive until
 * the caller sets a non-zero duty via @ref alp_pwm_set_duty.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL; @c channel_id must
 *                 be < 8 and resolvable to a ready Zephyr pwm device.
 * @return Open handle on success, or NULL on any of:
 *         - @p cfg is NULL
 *         - @c channel_id out of range or alias unset
 *         - underlying device not ready
 *         - handle pool exhausted
 *         - hardware rejected the period
 */
alp_pwm_t   *alp_pwm_open(const alp_pwm_config_t *cfg);

/**
 * @brief Set the active-level pulse width.
 *
 * @param[in] pwm       Handle from @ref alp_pwm_open.
 * @param[in] pulse_ns  Active-level duration in ns.  Must be ≤ the
 *                      configured period; pass 0 to drive fully off,
 *                      or the period value to drive fully on.
 * @return ALP_OK on success;
 *         ALP_ERR_NOT_READY if @p pwm is NULL or closed;
 *         ALP_ERR_INVAL if @p pulse_ns exceeds the period;
 *         ALP_ERR_IO on a backend failure.
 */
alp_status_t alp_pwm_set_duty(alp_pwm_t *pwm, uint32_t pulse_ns);

/**
 * @brief Update the period without closing.
 *
 * Some SoCs share the period across channels of a timer block — a
 * change here may affect siblings.  The duty is reset to 0 % so the
 * caller must re-arm via @ref alp_pwm_set_duty.
 *
 * @param[in] pwm        Handle from @ref alp_pwm_open.
 * @param[in] period_ns  New period in ns.  Must be > 0.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_pwm_set_period(alp_pwm_t *pwm, uint32_t period_ns);

/**
 * @brief Drive the output low and release the handle back to the pool.
 *
 * NULL or already-closed handles are silently ignored.
 *
 * @param[in] pwm  Handle from @ref alp_pwm_open, or NULL.
 */
void         alp_pwm_close(alp_pwm_t *pwm);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_PWM_H */
