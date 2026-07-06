/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file power.h
 * @brief Alp SDK low-power-mode abstraction.
 *
 * The E1M standard reserves four logical power modes -- RUN,
 * SLEEP, DEEP_SLEEP, and STANDBY -- mapped per SoC by the
 * backend.  Apps select a mode via @ref alp_power_request_sleep
 * after declaring which sources may wake the SoC via
 * @ref alp_power_configure_wake_source.  On wakeup the function
 * returns with the realised mode + the wake source that actually
 * fired so apps can branch (e.g. handle an RTC tick vs a GPIO
 * irq differently).
 *
 * Backends:
 *   - Zephyr   : Zephyr's `pm_policy_*` API + per-SoC `pm_state`
 *                tables.  Wake sources resolve via the per-source
 *                vendor HAL (rtc_alarm / gpio_interrupt / lpuart).
 *   - V2N      : routes through the GD32G553 supervisor singleton
 *                via `CMD_POWER_MODE_SET` (opcode 0x28, reserved
 *                at protocol v0.5).  The supervisor wakes the
 *                Renesas SoC, then re-runs its own handshake so
 *                the bridge stays usable after deep-sleep cycles.
 *   - Yocto    : `/sys/power/state` write + `/sys/class/rtc/rtcN/
 *                wakealarm` for timed wakes.
 *   - Baremetal: vendor HAL low-power primitives.
 *
 * Sleep modes follow a coarse low-power ladder; the exact wake
 * latency + retained-state guarantees are SoC-defined and
 * documented in the per-SoM HW reference.  Customers writing
 * portable code should treat the modes as monotonic (deeper =
 * lower power + longer wake) and rely on the wake-source
 * configuration rather than the mode name for correctness-
 * critical state retention.
 *
 * Typical usage:
 * @code
 *     alp_power_t *p = alp_power_open();
 *     alp_power_configure_wake_source(p,
 *         ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_GPIO);
 *     alp_power_wake_info_t info = { 0 };
 *     alp_power_request_sleep(p, ALP_POWER_MODE_DEEP_SLEEP,
 *                             30 * 1000u, &info);
 *     // ...wake-up handler...
 *     alp_power_close(p);
 * @endcode
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.5 new -- system-power-mode surface (sleep / deep-sleep / standby + wake-source bitmaps).
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_POWER_H
#define ALP_POWER_H

#include <stdint.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Logical power mode selectors.  Backends round to the closest
 *  SoC-supported mode; the realised mode is reported back in
 *  @ref alp_power_wake_info_t::realised_mode. */
typedef enum {
	ALP_POWER_MODE_RUN        = 0, /**< Normal running mode (no sleep). */
	ALP_POWER_MODE_SLEEP      = 1, /**< CPU clock-gated; peripherals + RAM live. */
	ALP_POWER_MODE_DEEP_SLEEP = 2, /**< Clocks gated; RAM retained; vendor wake sources only. */
	ALP_POWER_MODE_STANDBY    = 3, /**< Lowest power; RAM NOT retained; vendor wake only. */
} alp_power_mode_t;

/** Wake-source bitmap.  OR together to enable multiple sources.
 *  Backends honour the subset their hardware supports; unsupported
 *  bits are silently ignored. */
#define ALP_POWER_WAKE_NONE     0x00000000u
#define ALP_POWER_WAKE_RTC      0x00000001u /**< RTC alarm / periodic tick. */
#define ALP_POWER_WAKE_GPIO     0x00000002u /**< Configured GPIO IRQ line. */
#define ALP_POWER_WAKE_UART_RX  0x00000004u /**< UART RX activity. */
#define ALP_POWER_WAKE_TIMER    0x00000008u /**< Free-running timer match. */
#define ALP_POWER_WAKE_USB      0x00000010u /**< USB SOF / VBUS event. */
#define ALP_POWER_WAKE_ETH_LINK 0x00000020u /**< Ethernet link-up / WoL packet. */

/** Information returned by @ref alp_power_request_sleep about how the
 *  sleep round-trip resolved. */
typedef struct {
	alp_power_mode_t realised_mode; /**< Mode the backend actually entered. */
	uint32_t         wake_source;   /**< Wake-source bit that fired (one of
                                          the @c ALP_POWER_WAKE_* macros).  Zero
                                          if the call returned without sleeping
                                          (e.g. wake_after_ms == 0). */
	uint32_t         slept_ms;      /**< Wall-clock duration of the sleep cycle
                                          (best-effort; SoC-defined precision). */
} alp_power_wake_info_t;

/** Opaque handle.  Allocate via @ref alp_power_open. */
typedef struct alp_power alp_power_t;

/**
 * @brief Acquire the system-wide power-management handle.
 *
 * The handle is single-instance per process: a second open() call
 * returns the same underlying handle (or @ref ALP_ERR_BUSY via
 * NULL + alp_last_error, depending on backend).
 *
 * @return Handle on success, NULL with @ref alp_last_error set on
 *         backend-specific failure.
 */
alp_power_t *alp_power_open(void);

/**
 * @brief Configure which wake sources may exit a sleep.
 *
 * Replaces (does NOT add to) the active wake-source bitmap.  Must
 * be called before @ref alp_power_request_sleep -- a sleep request
 * with no configured wake sources returns @ref ALP_ERR_INVAL
 * (because the SoC would not wake without the watchdog firing).
 *
 * @param[in] handle       Handle from @ref alp_power_open.
 * @param[in] wake_bitmap  Bitmap of @c ALP_POWER_WAKE_* macros.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT (backend can't honour any of the
 *         requested sources).
 */
alp_status_t alp_power_configure_wake_source(alp_power_t *handle, uint32_t wake_bitmap);

/**
 * @brief Request a sleep transition + block until wake.
 *
 * Synchronous: blocks the calling thread for the duration of the
 * sleep + wake cycle.  The backend reconfigures peripheral clocks,
 * enters the requested mode (rounded to the closest SoC-supported
 * mode), waits for one of the configured wake sources to fire,
 * then re-runs any required post-wake bring-up before returning.
 *
 * On the V2N family the call routes through the GD32G553
 * supervisor's `CMD_POWER_MODE_SET` opcode; the supervisor wakes
 * the Renesas SoC and the singleton re-runs its handshake so the
 * bridge stays usable across deep-sleep cycles.
 *
 * @param[in]  handle          Handle from @ref alp_power_open.
 * @param[in]  mode            Requested mode (RUN is invalid here;
 *                             use @ref alp_power_close to release).
 * @param[in]  wake_after_ms   Max wall-clock wait, or 0 for "wake
 *                             only on a non-timer source".  When
 *                             non-zero the backend sets up an RTC
 *                             alarm even if @ref ALP_POWER_WAKE_RTC
 *                             wasn't in the configured bitmap
 *                             (timer is implicit when wake_after_ms
 *                             > 0).
 * @param[out] info            Optional; receives the realised mode +
 *                             actual wake source + slept duration.
 *                             May be NULL if the caller doesn't
 *                             care.
 *
 * @return ALP_OK / ALP_ERR_INVAL (no wake configured + zero
 *         wake_after_ms; or @c mode == RUN) / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO (backend transport
 *         failure mid-cycle).
 */
alp_status_t alp_power_request_sleep(alp_power_t           *handle,
                                     alp_power_mode_t       mode,
                                     uint32_t               wake_after_ms,
                                     alp_power_wake_info_t *info);

/**
 * @brief Release the power-management handle.  NULL is a no-op.
 *
 * @param[in] handle  Handle from @ref alp_power_open, or NULL.
 */
void alp_power_close(alp_power_t *handle);

/**
 * @brief Query the capabilities of an opened power handle.
 *
 * @param handle  Handle from @ref alp_power_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p handle is NULL.
 */
const alp_capabilities_t *alp_power_capabilities(const alp_power_t *handle);

/* ------------------------------------------------------------------ */
/* Operating-point profiles                                            */
/*                                                                     */
/* Some SoCs keep the power/clock tree behind a system-controller      */
/* firmware: the application core never pokes PLL / DC-DC / power-     */
/* domain registers directly, it reads and writes a per-core PROFILE   */
/* instead.  This handle-less surface exposes the two profiles every   */
/* such platform keeps -- the active RUN operating point and the       */
/* STANDBY (off/retention) operating point -- through portable fields. */
/* ------------------------------------------------------------------ */

/** Which operating-point profile to read/write. */
typedef enum {
	ALP_POWER_PROFILE_RUN     = 0, /**< The active operating point. */
	ALP_POWER_PROFILE_STANDBY = 1, /**< The standby/off operating point +
	                                    wake configuration. */
} alp_power_profile_id_t;

/**
 * @brief One operating-point profile in portable units.
 *
 * Frequencies are Hz and rails are millivolts.  The domain / memory /
 * wake bitmasks are IMPLEMENTATION-DEFINED: their bit legends are SoC
 * business documented in the per-SoM HW reference, and portable code
 * must treat them as opaque tokens (read-modify-write them, log them,
 * compare them -- never construct them from portable constants).
 *
 * A zero field means "unknown / not reported" on @ref
 * alp_power_profile_get and "keep the current value" on @ref
 * alp_power_profile_set.
 */
typedef struct alp_power_profile_t {
	uint32_t cpu_clk_hz;    /**< Core clock of the calling core's domain
	                             in Hz.  RUN: the active CPU clock;
	                             STANDBY: the standby-scaled clock. */
	uint32_t rail_mv;       /**< Core DC-DC rail in millivolts. */
	uint32_t power_domains; /**< Implementation-defined bitmask of power
	                             domains held on in this profile. */
	uint32_t memory_blocks; /**< Implementation-defined bitmask of memory
	                             blocks retained/powered in this profile. */
	uint32_t wake_events;   /**< Implementation-defined wake-event bitmask.
	                             Meaningful for STANDBY only; 0 for RUN. */
	uint32_t io_mv;         /**< Flexible-IO bank rail in millivolts
	                             (e.g. 3300 or 1800); 0 when the SoC has
	                             no switchable IO rail. */
} alp_power_profile_t;

/**
 * @brief Read an operating-point profile.
 *
 * Read-only: nothing about the live operating point changes.  On SoCs
 * whose profiles live behind a system-controller firmware this is a
 * bounded mailbox round-trip; the call never hangs.
 *
 * @param[in]  which  @ref ALP_POWER_PROFILE_RUN or
 *                    @ref ALP_POWER_PROFILE_STANDBY.
 * @param[out] out    Zero-filled, then populated with every field the
 *                    backend can source.
 *
 * @return  @ref ALP_OK on success.
 *          @ref ALP_ERR_INVAL on NULL @p out or an invalid @p which.
 *          @ref ALP_ERR_NOSUPPORT when the active build has no
 *                                 profile-capable backend.
 *          @ref ALP_ERR_NOT_READY when the controller is
 *                                 asleep/unreachable (retryable).
 *          @ref ALP_ERR_IO on a transport fault.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.9 -- operating-point profile surface.
 */
alp_status_t alp_power_profile_get(alp_power_profile_id_t which, alp_power_profile_t *out);

/**
 * @brief Change an operating-point profile (read-modify-write).
 *
 * @warning This call CHANGES the live power/clock operating point.  A
 *          wrong rail voltage or clock selection can brown out the
 *          core or stall the SoC; treat every call like a firmware
 *          update -- know the target values from the per-SoM HW
 *          reference and have a recovery plan before running it on
 *          hardware.
 *
 * The backend reads the current profile, overwrites exactly the
 * fields the caller set to a NON-ZERO value, and writes the result
 * back -- so a partial update ({ .rail_mv = 800 }) touches nothing
 * else.  Frequencies must match a value the silicon supports exactly;
 * the backend never rounds (a mismatch returns @ref ALP_ERR_INVAL).
 *
 * @param[in] which    @ref ALP_POWER_PROFILE_RUN or
 *                     @ref ALP_POWER_PROFILE_STANDBY.
 * @param[in] profile  Fields to apply; zero fields keep their current
 *                     value.  @c wake_events is writable only on the
 *                     STANDBY profile.
 *
 * @return  @ref ALP_OK when the controller accepted the new profile.
 *          @ref ALP_ERR_INVAL on NULL @p profile, an invalid
 *                             @p which, or a value the silicon cannot
 *                             realise exactly.
 *          @ref ALP_ERR_NOSUPPORT when the active build has no
 *                                 profile-capable backend.
 *          @ref ALP_ERR_NOT_READY / @ref ALP_ERR_IO as for
 *          @ref alp_power_profile_get.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.9 -- operating-point profile surface.
 */
alp_status_t alp_power_profile_set(alp_power_profile_id_t     which,
                                   const alp_power_profile_t *profile);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_POWER_H */
