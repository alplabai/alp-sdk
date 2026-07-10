/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pwm.h
 * @brief Alp SDK pulse-width-modulation abstraction.
 *
 * The E1M and E1M-X standards each reserve eight PWM channels.  E1M
 * firmware uses `ALP_E1M_PWM0..7` from `<alp/e1m_pinout.h>`; E1M-X
 * firmware uses `ALP_E1M_X_PWM0..7` from `<alp/e1m_x_pinout.h>`.
 * The studio's pin allocator binds the selected form-factor ID to the
 * SoC's underlying timer/compare blocks.  Apps see a uniform
 * `alp_pwm_*` surface.
 *
 * Backends:
 *   - Zephyr   : `pwm_*` driver class.
 *   - Yocto    : `/sys/class/pwm/` sysfs interface.
 *   - Baremetal: vendor HAL timer compare-output channels.
 *
 * Typical usage:
 * @code
 *     alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
 *         .channel_id = ALP_E1M_PWM0,     // E1M; use ALP_E1M_X_PWM0 on E1M-X
 *         .period_ns  = 1000000,               // 1 kHz
 *         .polarity   = ALP_PWM_POLARITY_NORMAL,
 *     });
 *     alp_pwm_set_duty(led, 500000);           // 50 % duty
 *     // ...
 *     alp_pwm_close(led);
 * @endcode
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2 surface; locked.
 *      See docs/abi-markers.md for the convention.
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
	ALP_PWM_POLARITY_NORMAL   = 0, /**< High during the active portion. */
	ALP_PWM_POLARITY_INVERTED = 1  /**< Low during the active portion. */
} alp_pwm_polarity_t;

/** Per-channel alignment mode for @ref alp_pwm_configure.  Edge-aligned
 *  is the conventional sawtooth-counter behaviour; center-aligned modes
 *  use a triangle-counter so the rising and falling edges sit
 *  symmetrically around either the count-up phase, count-down phase, or
 *  both phases, which reduces high-frequency harmonic content in motor-
 *  drive applications. */
typedef enum {
	ALP_PWM_ALIGN_EDGE        = 0, /**< Sawtooth counter (default). */
	ALP_PWM_ALIGN_CENTER_UP   = 1, /**< Triangle, edges in count-up. */
	ALP_PWM_ALIGN_CENTER_DOWN = 2, /**< Triangle, edges in count-down. */
	ALP_PWM_ALIGN_CENTER_BOTH = 3, /**< Triangle, edges in both phases. */
} alp_pwm_align_t;

/** Bitmap of break-input / fault sources for @ref alp_pwm_configure.
 *  Bit 0 enables an external break input that disables the channel's
 *  output when asserted; remaining bits are reserved for future fault
 *  sources (e.g. on-die comparator trip).  Backends that don't support
 *  a particular bit silently ignore it. */
#define ALP_PWM_BREAK_NONE     0x00u
#define ALP_PWM_BREAK_EXTERNAL 0x01u

/** Opaque PWM channel handle.  Allocate via @ref alp_pwm_open. */
typedef struct alp_pwm alp_pwm_t;

/** Configuration passed to @ref alp_pwm_open. */
typedef struct {
	uint32_t channel_id; /**< Form-factor PWM instance ID: ALP_E1M_PWM0..7 or ALP_E1M_X_PWM0..7. */
	uint32_t period_ns;  /**< PWM period in nanoseconds. 0 = use DT default. */
	alp_pwm_polarity_t polarity;
} alp_pwm_config_t;

/**
 * @brief Default-initialize an @ref alp_pwm_config_t for channel @p id.
 *
 * Identity from @p id; canonical defaults: @c period_ns = 0 (use the
 * devicetree default period), @c polarity = @ref ALP_PWM_POLARITY_NORMAL
 * (high during the active portion). Set @c period_ns for an explicit
 * frequency after expansion.
 */
#define ALP_PWM_CONFIG_DEFAULT(id)                                                                 \
	((alp_pwm_config_t){ .channel_id = (id), .period_ns = 0u, .polarity = ALP_PWM_POLARITY_NORMAL })

/**
 * @brief Acquire and start a PWM channel at 0 % duty.
 *
 * Resolves @p cfg->channel_id via the `alp-pwm<N>` devicetree alias,
 * borrows a free handle from the SDK's pool, and primes the hardware
 * with the requested period.  The output stays low / inactive until
 * the caller sets a non-zero duty via @ref alp_pwm_set_duty.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL; @c channel_id must
 *                 be less than the selected form factor's PWM count
 *                 (@ref ALP_E1M_PWM_COUNT or @ref ALP_E1M_X_PWM_COUNT)
 *                 and resolvable to a ready Zephyr pwm device.
 * @return Open handle on success, or NULL on any of:
 *         - @p cfg is NULL
 *         - @c channel_id out of range or alias unset
 *         - underlying device not ready
 *         - handle pool exhausted
 *         - hardware rejected the period
 */
alp_pwm_t *alp_pwm_open(const alp_pwm_config_t *cfg);

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
 * @brief Sticky per-channel tuning: alignment mode + dead-time + break input.
 *
 * Applies tuning that the underlying hardware retains across subsequent
 * @ref alp_pwm_set_duty / @ref alp_pwm_set_period calls.  On the V2N
 * family (V2N + V2N-M1, both of which carry the GD32G553 supervisor
 * MCU) the call hits the GD32's per-channel advanced-timer registers
 * (CHCTL / CCHP / BKDT, see GD32G553 reference manual §17).  On SoMs
 * whose backend has no comparable surface -- notably Zephyr's portable
 * `pwm_*` driver class, which exposes neither dead-time nor center-
 * alignment outside vendor-specific extensions -- the call returns
 * @ref ALP_ERR_NOSUPPORT so callers don't silently get a no-op.
 *
 * Backends round @p dead_time_ns down to the nearest tick they can
 * honour at the configured period; on the GD32 family that's
 * ~4.63 ns at the 216 MHz timer clock.  @p break_cfg is treated as an
 * opaque bitmap of @ref ALP_PWM_BREAK_NONE / @ref ALP_PWM_BREAK_EXTERNAL
 * flags.
 *
 * Safe to call before or after the first @ref alp_pwm_set_duty; the
 * tuning takes effect immediately and persists until the next
 * @ref alp_pwm_configure or until the channel is closed.
 *
 * @param[in] pwm           Handle from @ref alp_pwm_open.
 * @param[in] align_mode    One of @ref alp_pwm_align_t.
 * @param[in] dead_time_ns  Dead-time for complementary outputs (ns).
 *                          0 = no dead time.
 * @param[in] break_cfg     Bitmap of @c ALP_PWM_BREAK_* flags.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL (bad align
 *         enum) / ALP_ERR_NOSUPPORT (backend can't honour) /
 *         ALP_ERR_OUT_OF_RANGE (dead_time_ns exceeds the timer's
 *         maximum at the configured period) / ALP_ERR_IO.
 */
alp_status_t alp_pwm_configure(alp_pwm_t      *pwm,
                               alp_pwm_align_t align_mode,
                               uint32_t        dead_time_ns,
                               uint8_t         break_cfg);

/**
 * @brief Drive the output low and release the handle back to the pool.
 *
 * NULL or already-closed handles are silently ignored.
 *
 * @param[in] pwm  Handle from @ref alp_pwm_open, or NULL.
 */
void alp_pwm_close(alp_pwm_t *pwm);

/**
 * @brief Query the capabilities of an opened PWM channel handle.
 *
 * @param pwm  Handle from @ref alp_pwm_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p pwm is NULL.
 */
const alp_capabilities_t *alp_pwm_capabilities(const alp_pwm_t *pwm);

/* ================================================================== */
/* Single-pulse output (v0.5, §2B.2)                                   */
/* ================================================================== */

/**
 * @brief Drive a one-shot pulse of @p pulse_ns then stop the channel.
 *
 * The channel must already be open at a non-zero period via
 * @ref alp_pwm_open.  After the pulse, the output returns to the
 * inactive level and the channel stays open at 0 % duty until the
 * caller fires another pulse or sets a steady duty via
 * @ref alp_pwm_set_duty.
 *
 * Single-pulse is useful for trigger generation (camera flash, ADC
 * external start) where the timing of one pulse matters but the
 * application doesn't want to manage a steady-state PWM.
 *
 * @param[in] pwm       Handle from @ref alp_pwm_open.
 * @param[in] pulse_ns  Pulse width in nanoseconds.  Must be > 0 and
 *                      <= the channel's configured period.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_NOSUPPORT (backend lacks a one-shot timer
 *         primitive -- portable single-pulse is V2N bridge-only
 *         today; the wave-2 GD32 HAL body lands once the firmware
 *         CMD_PWM_SINGLE_PULSE handler is implemented).
 */
alp_status_t alp_pwm_single_pulse(alp_pwm_t *pwm, uint32_t pulse_ns);

/* ================================================================== */
/* Input capture (v0.5, §2B.2)                                         */
/* ================================================================== */

/** Edge polarity selector for @ref alp_pwm_capture_open. */
typedef enum {
	ALP_PWM_CAPTURE_EDGE_RISING  = 0, /**< Capture on rising edges only. */
	ALP_PWM_CAPTURE_EDGE_FALLING = 1, /**< Capture on falling edges only. */
	ALP_PWM_CAPTURE_EDGE_BOTH    = 2, /**< Capture on both edges (pulse-width measurement). */
} alp_pwm_capture_edge_t;

/** Opaque input-capture handle.  Allocate via @ref alp_pwm_capture_open. */
typedef struct alp_pwm_capture alp_pwm_capture_t;

/** Configuration passed to @ref alp_pwm_capture_open. */
typedef struct {
	uint32_t               channel_id; /**< PWM channel index (ALP_E1M_PWM0..PWM7) used as input. */
	alp_pwm_capture_edge_t edge;       /**< Edge polarity selector. */
} alp_pwm_capture_config_t;

/**
 * @brief Reconfigure a PWM channel as an input-capture source.
 *
 * Switches the channel's pin from output mode to input-capture
 * mode, arms the underlying timer to latch the counter on each
 * edge of @p cfg->edge, and returns a handle.  Subsequent calls
 * to @ref alp_pwm_capture_read drain the latest period + pulse-
 * width measurements.  The channel cannot be used as a regular
 * PWM output while capture is active -- close the capture handle
 * (@ref alp_pwm_capture_close) before re-opening for output.
 *
 * Useful for: tachometer-style frequency measurement; fan-tach
 * RPM; quad-encoder index pulse timing; servo PWM input
 * decoding; LIDAR pulse-width return.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 *
 * @return Handle on success, or NULL with @ref alp_last_error set
 *         to ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE (channel_id) /
 *         ALP_ERR_NOSUPPORT (backend lacks an input-capture
 *         primitive -- V2N bridge-only today; the wave-2 GD32
 *         HAL body lands once the firmware
 *         CMD_PWM_CAPTURE_BEGIN handler is implemented) /
 *         ALP_ERR_BUSY (the channel is already open as an
 *         output) / ALP_ERR_NOMEM.
 */
alp_pwm_capture_t *alp_pwm_capture_open(const alp_pwm_capture_config_t *cfg);

/**
 * @brief Read the latest captured period + pulse-width.
 *
 * Reports the period (rising-edge to rising-edge) and the
 * active-level pulse width (rising-edge to next falling-edge) of
 * the most recently sampled cycle on the input pin.  If no edge
 * has been seen since the last call, returns ALP_ERR_NOT_READY.
 *
 * Values are in nanoseconds, capped at the timer's overflow
 * value -- a static signal returns OUT_OF_RANGE on the firmware
 * side once the counter wraps.
 *
 * @param[in]  cap            Handle from @ref alp_pwm_capture_open.
 * @param[out] period_ns_out  Receives the period in ns.  May be NULL
 *                            if the caller only wants the pulse width.
 * @param[out] pulse_ns_out   Receives the active-level pulse width in
 *                            ns.  May be NULL if the caller only wants
 *                            the period.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (no edge yet) / ALP_ERR_INVAL /
 *         ALP_ERR_OUT_OF_RANGE (counter wrapped between edges --
 *         signal too slow for the timer's tick rate) / ALP_ERR_IO /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t
alp_pwm_capture_read(alp_pwm_capture_t *cap, uint32_t *period_ns_out, uint32_t *pulse_ns_out);

/**
 * @brief Close a capture handle and release the channel.
 *
 * Reverts the channel's pin direction (input -> tri-state) so the
 * caller can re-open the channel as an output via
 * @ref alp_pwm_open after this call.  NULL is a no-op.
 *
 * @param[in] cap  Handle from @ref alp_pwm_capture_open, or NULL.
 */
void alp_pwm_capture_close(alp_pwm_capture_t *cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_PWM_H */
