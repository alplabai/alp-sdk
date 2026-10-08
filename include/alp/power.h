/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file power.h
 * @brief Alp SDK low-power-mode abstraction.
 *
 * The E1M standard reserves five logical power modes -- RUN,
 * SLEEP, DEEP_SLEEP, STANDBY, and STOP -- mapped per SoC by the
 * backend.  STOP is the deepest rung (e.g. the Alif Ensemble E8's
 * STOP_0..STOP_5 ladder, Table 5-5: low-uA levels depending on what
 * is retained -- e.g. STOP_2 retains the 4 KB Utility SRAM at
 * ~1.1 uA); @ref alp_power_configure_retention says what to keep
 * before entering it.  Apps select a mode via
 * @ref alp_power_request_sleep after declaring which sources may
 * wake the SoC via @ref alp_power_configure_wake_source.  On wakeup
 * the function returns with the realised mode + the wake source
 * that actually fired so apps can branch (e.g. handle an RTC tick
 * vs a GPIO irq differently).
 *
 * @par STOP and STANDBY may not return
 * SLEEP and DEEP_SLEEP resume the caller where it stopped.  On a
 * backend whose STOP / STANDBY rung powers the core down (the Alif
 * Ensemble family: wake is a COLD BOOT through the Secure Enclave
 * Services), @ref alp_power_request_sleep does NOT return on wake --
 * execution restarts from the reset vector and only retained RAM
 * survives.  Portable code must therefore treat a STOP / STANDBY
 * request as "may never return": keep state worth keeping in a
 * retained region, and recover the wake cause after boot with
 * @ref alp_power_boot_wake_info instead of from the (never-delivered)
 * @ref alp_power_wake_info_t.  A backend that does return reports the
 * realised mode in @ref alp_power_wake_info_t::realised_mode as usual.
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
 *      v0.17 settles the shape while still experimental (#1813): adds
 *      @c ALP_POWER_MODE_STOP, the retention descriptor
 *      (@ref alp_power_configure_retention / @ref alp_power_retain_t),
 *      the @c ALP_POWER_WAKE_COMPARATOR / @c ALP_POWER_WAKE_BROWNOUT
 *      wake bits, @ref alp_power_wake_capabilities (a dedicated
 *      accessor, not an overload of @ref alp_power_capabilities's
 *      shared @c alp_capabilities_t::flags), and replaces the old
 *      "unsupported bits are silently ignored" wake-source contract
 *      with a reported-capability + error contract (see
 *      @ref alp_power_configure_wake_source).
 *      The SoM power-domain surface (@ref alp_power_domain_t,
 *      @ref alp_power_domain_policy_set, @ref alp_power_domain_info,
 *      @ref alp_power_boot_wake_info) is a contract only (#2784 U1):
 *      every backend currently answers @ref ALP_ERR_NOSUPPORT.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_POWER_H
#define ALP_POWER_H

#include <stdbool.h>
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
	ALP_POWER_MODE_STOP       = 4, /**< Deepest rung: main-domain peripherals powered down,
	                                     only the low-power (LP*) island stays alive.  Use
	                                     @ref alp_power_configure_retention to say what of
	                                     the rest to keep; backends without a distinct STOP
	                                     state round down to their deepest supported mode
	                                     and report it honestly in
	                                     @ref alp_power_wake_info_t::realised_mode (the
	                                     monotonic-mode contract documented above). */
} alp_power_mode_t;

/** Wake-source bitmap.  OR together to enable multiple sources.
 *
 *  Contract (settled in #1813 -- previously these bits were silently
 *  dropped when unsupported, which let a caller sleep on a source
 *  that could never fire): @ref alp_power_wake_capabilities on the
 *  opened handle reports exactly the subset of these bits the active
 *  backend can arm.  @ref alp_power_configure_wake_source returns
 *  @ref ALP_ERR_NOSUPPORT for a bitmap containing any bit outside
 *  that set, instead of ALP_OK -- a caller finds out at configuration
 *  time, not after a sleep that never wakes.  (Deliberately a
 *  separate accessor from @ref alp_power_capabilities -- that one's
 *  @c flags field is the cross-class @c alp_instance_cap_t bitmap
 *  every peripheral class shares -- e.g. @c ALP_INSTANCE_CAP_DMA is
 *  bit 0, the same numeric value as @c ALP_POWER_WAKE_RTC below --
 *  so wake-arm capability gets its own uint32_t rather than aliasing
 *  that one.) */
#define ALP_POWER_WAKE_NONE       0x00000000u
#define ALP_POWER_WAKE_RTC        0x00000001u /**< RTC alarm / periodic tick. */
#define ALP_POWER_WAKE_GPIO       0x00000002u /**< Configured GPIO IRQ line. */
#define ALP_POWER_WAKE_UART_RX    0x00000004u /**< UART RX activity. */
#define ALP_POWER_WAKE_TIMER      0x00000008u /**< Free-running timer match. */
#define ALP_POWER_WAKE_USB        0x00000010u /**< USB SOF / VBUS event. */
#define ALP_POWER_WAKE_ETH_LINK   0x00000020u /**< Ethernet link-up / WoL packet. */
#define ALP_POWER_WAKE_COMPARATOR 0x00000040u /**< Analog comparator threshold (e.g. LPCMP). */
#define ALP_POWER_WAKE_BROWNOUT   0x00000080u /**< Brown-out / under-voltage detect. */

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
 * Rejects at THIS call, not at @ref alp_power_request_sleep, any
 * bitmap containing a bit the backend cannot arm -- see the
 * capability + error contract documented on the @c ALP_POWER_WAKE_*
 * bitmap above.  Query @ref alp_power_wake_capabilities first to
 * build a bitmap that is guaranteed to be accepted.
 *
 * @param[in] handle       Handle from @ref alp_power_open.
 * @param[in] wake_bitmap  Bitmap of @c ALP_POWER_WAKE_* macros.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT (the bitmap requests at least one wake
 *         source the backend cannot arm -- see
 *         @ref alp_power_wake_capabilities).
 */
alp_status_t alp_power_configure_wake_source(alp_power_t *handle, uint32_t wake_bitmap);

/** Named RAM-retention footprints for @ref alp_power_configure_retention,
 *  cheapest first.  Deliberately named regions rather than a raw
 *  vendor bitmask (#1813) -- e.g. the Alif E8's
 *  `SERVICES_power_mem_retention_config` bitmask maps @ref
 *  ALP_POWER_RETAIN_TCM onto its TCM retention bits, rounded UP to the
 *  backend's own retention granularity and sized by @ref
 *  alp_power_retain_t::retain_kb.
 *
 *  Once the Alif STOP backend lands (#2784), the E8's 4 KB Utility SRAM
 *  ("BKRAM") is ALWAYS retained and reserved for the SDK (wake record,
 *  domain-restore state): it is not application RAM at any level.  The
 *  lowest floor an app can select is then the Utility-SRAM-retained
 *  rung (STOP_2, 1.1 uA typ., Table 5-5), never the no-retention
 *  STOP_5/4/3 rungs.  Today's backends have no STOP retention and
 *  answer NOSUPPORT for everything but NONE. */
typedef enum {
	ALP_POWER_RETAIN_NONE    = 0, /**< No application RAM retained.  Once the Alif STOP
	                                    backend lands (#2784) only the SDK-reserved boot
	                                    state survives (E8: the 4 KB Utility SRAM, STOP_2,
	                                    ~1.1 uA typ.) and that is the lowest floor the API
	                                    can select.  Size a battery off the mode your
	                                    configured wake source maps to. */
	ALP_POWER_RETAIN_UTILITY = 1, /**< Smallest SoC-guaranteed retained block (e.g. E8's
	                                    4 KB Utility SRAM, STOP_2, ~1.1 uA typ.).  The
	                                    Alif STOP backend (#2784) treats this as
	                                    equivalent to @ref ALP_POWER_RETAIN_NONE -- it
	                                    returns ALP_OK and retains nothing extra.  Today's
	                                    backends still return ALP_ERR_NOSUPPORT. */
	ALP_POWER_RETAIN_TCM     = 2, /**< @ref alp_power_retain_t::retain_kb KiB of
	                                    tightly-coupled memory; the backend rounds UP to
	                                    its own retention granularity (never NOSUPPORT
	                                    for a size that doesn't land exactly). */
	ALP_POWER_RETAIN_FULL    = 3, /**< Every RAM block the requested mode supports
	                                    retaining. */
} alp_power_retain_level_t;

/** Retention request for @ref alp_power_configure_retention. */
typedef struct {
	alp_power_retain_level_t level;     /**< Named footprint; see the enum. */
	uint32_t                 retain_kb; /**< KiB to retain; read only when
	                                          @c level == @ref ALP_POWER_RETAIN_TCM. */
} alp_power_retain_t;

/**
 * @brief Configure how much RAM stays powered across the next sleep.
 *
 * Optional: a handle that never calls this defaults to whatever the
 * backend's minimum for the requested mode is (typically @ref
 * ALP_POWER_RETAIN_NONE, i.e. no application RAM).  Call before
 * @ref alp_power_request_sleep, same ordering as @ref
 * alp_power_configure_wake_source.
 *
 * @param[in] handle  Handle from @ref alp_power_open.
 * @param[in] retain  Requested footprint.  NULL is @ref ALP_ERR_INVAL.
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL @p retain; an out-of-range
 *         @c level; or @c level == @ref ALP_POWER_RETAIN_TCM with
 *         @c retain_kb == 0, which is not a valid spelling of "retain
 *         nothing" -- use @ref ALP_POWER_RETAIN_NONE for that) /
 *         ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT (the backend has no
 *         way to realise this footprint AT ALL -- e.g. it doesn't
 *         implement @ref ALP_POWER_RETAIN_TCM, or @c retain_kb
 *         exceeds the SoC's available TCM).  A @c retain_kb that
 *         simply doesn't land on the backend's retention granularity
 *         is never NOSUPPORT -- the backend rounds it UP instead (see
 *         @ref ALP_POWER_RETAIN_TCM above).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.17 (#1813) -- retention descriptor.
 */
alp_status_t alp_power_configure_retention(alp_power_t *handle, const alp_power_retain_t *retain);

/**
 * @brief Request a sleep transition + block until wake.
 *
 * Synchronous: blocks the calling thread for the duration of the
 * sleep + wake cycle.  The backend reconfigures peripheral clocks,
 * enters the requested mode (rounded to the closest SoC-supported
 * mode), waits for one of the configured wake sources to fire,
 * then re-runs any required post-wake bring-up before returning.
 *
 * @warning STOP and STANDBY may not return: on backends where those
 *          rungs power the core down, wake is a cold boot and this
 *          function never hands control back (see the file-level
 *          note).  The wake cause is then read with
 *          @ref alp_power_boot_wake_info.
 *
 * Before reaching the backend the dispatcher checks the configured
 * wake bitmap against what the REQUESTED @p mode can arm (a source
 * armable in SLEEP may not be armable in STOP) and returns
 * @ref ALP_ERR_NOSUPPORT, with nothing changed, for a mismatch.
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
 *                             non-zero the backend arms a timed wake
 *                             (RTC alarm or low-power timer, whichever
 *                             the mode can use) even if
 *                             @ref ALP_POWER_WAKE_RTC wasn't in the
 *                             configured bitmap.  Where the timed
 *                             wake is a 32-bit low-power timer
 *                             (e.g. the Alif LPTIMER) the value is
 *                             bounded by that counter at its tick
 *                             rate; a backend rejects a longer
 *                             request with @ref ALP_ERR_INVAL.
 * @param[out] info            Optional; receives the realised mode +
 *                             actual wake source + slept duration.
 *                             May be NULL if the caller doesn't
 *                             care.
 *
 * @return ALP_OK / ALP_ERR_INVAL (no wake configured + zero
 *         wake_after_ms; or @c mode == RUN; or a timed wake beyond
 *         the backend's counter; or a domain policy that conflicts
 *         with an armed wake source) / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT (incl. a configured wake bitmap the
 *         requested @p mode cannot arm, and @c wake_after_ms > 0 on
 *         a backend that reports per-mode wake capabilities when the
 *         mode can arm neither @ref ALP_POWER_WAKE_TIMER nor
 *         @ref ALP_POWER_WAKE_RTC) / ALP_ERR_IO (backend transport
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
 * @brief Query the generic per-instance capabilities of an opened
 *        power handle (the @c alp_instance_cap_t bitmap every
 *        peripheral class shares -- DMA / HW_OVERSAMPLE / etc.).
 *
 * The power class has no such capabilities today (no DMA / hardware
 * trigger / oversample concept applies to a sleep-mode handle), so
 * @c flags is always 0 here; it is NOT where wake-source capability
 * lives -- see @ref alp_power_wake_capabilities for that (#1813).
 *
 * @param handle  Handle from @ref alp_power_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p handle is NULL.
 */
const alp_capabilities_t *alp_power_capabilities(const alp_power_t *handle);

/**
 * @brief Query which @c ALP_POWER_WAKE_* bits the active backend can
 *        actually arm.
 *
 * The "reported-capability" half of the wake-source contract
 * documented on the @c ALP_POWER_WAKE_* bitmap above (#1813): a bit
 * absent here always makes @ref alp_power_configure_wake_source
 * reject a bitmap containing it with @ref ALP_ERR_NOSUPPORT.  Zero
 * means the backend cannot arm any wake source (e.g. no real power
 * backend is linked for this build).  Deliberately a dedicated
 * accessor rather than a field on @ref alp_power_capabilities's
 * shared @c alp_capabilities_t -- that struct's @c flags is the
 * cross-class @c alp_instance_cap_t bitmap, whose bit values collide
 * numerically with @c ALP_POWER_WAKE_* (e.g. @c ALP_INSTANCE_CAP_DMA
 * == @c ALP_POWER_WAKE_RTC == bit 0).
 *
 * @param handle  Handle from @ref alp_power_open, or NULL.
 * @return Bitmap of @c ALP_POWER_WAKE_* macros; 0 if @p handle is NULL.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.17 (#1813).
 */
uint32_t alp_power_wake_capabilities(const alp_power_t *handle);

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
	uint32_t rail_mv;       /**< Core DC-DC rail in millivolts.  Per-SoM
	                             window (e.g. E8 / AEN801: 750-850 mV via
	                             the SE aiPM DC-DC); a value outside that
	                             window returns @ref ALP_ERR_INVAL from
	                             @ref alp_power_profile_set without
	                             touching the live operating point. */
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

/* ------------------------------------------------------------------ */
/* SoM power domains + boot wake record (#2784)                        */
/*                                                                     */
/* Before STOP / STANDBY the SDK can quiesce the on-module consumers   */
/* that would otherwise burn power through the sleep (Wi-Fi/BLE        */
/* coprocessor, Ethernet PHY, external flash / RAM, temperature        */
/* sensor, RTC clock-out, backlight) and restore them on the cold-boot */
/* wake.  Domains are named by portable ROLE, not by chip or pad, so   */
/* the same code keeps working when the SoM is swapped within a        */
/* family.  This header is the CONTRACT only: no backend implements it */
/* yet and every call below answers ALP_ERR_NOSUPPORT.                 */
/* ------------------------------------------------------------------ */

/** On-module power domains the SDK can quiesce, by portable role. */
typedef enum {
	ALP_POWER_DOMAIN_WIFI_BLE    = 0, /**< Wi-Fi / BLE coprocessor. */
	ALP_POWER_DOMAIN_ETH_PHY     = 1, /**< Ethernet PHY. */
	ALP_POWER_DOMAIN_EXT_FLASH   = 2, /**< External (XIP / NOR) flash. */
	ALP_POWER_DOMAIN_EXT_RAM     = 3, /**< External (Hyper / PSRAM) RAM. */
	ALP_POWER_DOMAIN_TEMP_SENSOR = 4, /**< On-module temperature sensor. */
	ALP_POWER_DOMAIN_RTC         = 5, /**< On-module RTC (clock-out only; the
	                                       time base is kept alive). */
	ALP_POWER_DOMAIN_BACKLIGHT   = 6, /**< On-module backlight driver. */
	ALP_POWER_DOMAIN_COUNT       = 7, /**< Number of domains; not a valid domain. */
} alp_power_domain_t;

/** Bit for domain @p d in the @c quiesced_domains / @c restored_domains /
 *  @c restore_failed_domains bitmaps of @ref alp_power_boot_info_t. */
#define ALP_POWER_DOMAIN_BIT(d) (1u << (unsigned)(d))

/** What the SDK does to a domain around STOP / STANDBY. */
typedef enum {
	ALP_POWER_DOMAIN_POLICY_AUTO       = 0, /**< Default: the SoM's safe default action for
	                                             the mode (see @ref
	                                             alp_power_domain_info_t::default_action). */
	ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE = 1, /**< Never touched. */
	ALP_POWER_DOMAIN_POLICY_RAIL_OFF   = 2, /**< Explicit supply gate.  Opt-in: refused
	                                             unless the SoM marks it available and its
	                                             build-time gate is enabled. */
} alp_power_domain_policy_t;

/** Actions a domain can support, bits of
 *  @ref alp_power_domain_info_t::supported_actions / @c default_action. */
#define ALP_POWER_ACTION_NONE                0x00000000u
#define ALP_POWER_ACTION_HOLD_RESET          0x00000001u /**< Hold the chip's reset line. */
#define ALP_POWER_ACTION_POWERDOWN_PIN       0x00000002u /**< Drive its power-down pin. */
#define ALP_POWER_ACTION_DEEP_POWER_DOWN_CMD 0x00000004u /**< Deep-power-down command. */
#define ALP_POWER_ACTION_SHUTDOWN_REG        0x00000008u /**< Shutdown bit in the chip. */
#define ALP_POWER_ACTION_RAIL_OFF            0x00000010u /**< Gate the supply. */

/** Carrier-side loads that quiescing a domain also affects, bits of
 *  @ref alp_power_domain_info_t::dependents. */
#define ALP_POWER_DEP_NONE    0x00000000u
#define ALP_POWER_DEP_CAM_LDO 0x00000001u /**< Camera LDO enables. */
#define ALP_POWER_DEP_SD_EN   0x00000002u /**< SD-card supply enable. */

/** Static description of one domain on the running SoM. */
typedef struct {
	bool     present;            /**< Populated on this SKU. */
	bool     holds_through_stop; /**< The quiesce state survives STOP itself
	                                  (false: re-applied at wake). */
	uint32_t supported_actions;  /**< @c ALP_POWER_ACTION_* the domain supports. */
	uint32_t default_action;     /**< The single @c ALP_POWER_ACTION_* bit AUTO applies. */
	uint32_t dependents;         /**< @c ALP_POWER_DEP_* loads affected. */
} alp_power_domain_info_t;

/** Record of the last STOP / STANDBY cycle, read after the cold-boot wake. */
typedef struct {
	bool             valid;                  /**< A wake record from a completed cycle exists;
	                                              false after a plain power-on reset (the
	                                              other fields are then zero). */
	alp_power_mode_t realised_mode;          /**< Mode actually entered. */
	uint32_t         wake_source;            /**< @c ALP_POWER_WAKE_* bit that fired. */
	uint32_t         slept_ms;               /**< Sleep duration (best effort). */
	uint32_t         quiesced_domains;       /**< @ref ALP_POWER_DOMAIN_BIT set quiesced
	                                              before sleep. */
	uint32_t         restored_domains;       /**< Bitmap restored after wake. */
	uint32_t         restore_failed_domains; /**< Bitmap whose restore failed
	                                              (reported, never fatal). */
} alp_power_boot_info_t;

/**
 * @brief Set the quiesce policy of one domain for the next STOP / STANDBY.
 *
 * Takes effect at the next @ref alp_power_request_sleep with mode STOP or
 * STANDBY; SLEEP and DEEP_SLEEP never quiesce domains.  A policy that
 * conflicts with the armed wake sources (e.g. RTC domain RAIL_OFF while
 * @ref ALP_POWER_WAKE_RTC is armed) is rejected at the sleep request with
 * @ref ALP_ERR_INVAL before any change is made.
 *
 * @param[in] handle  Handle from @ref alp_power_open.
 * @param[in] domain  Domain to configure.
 * @param[in] policy  Requested policy.
 *
 * @return ALP_OK / ALP_ERR_INVAL (@p domain or @p policy out of range) /
 *         ALP_ERR_NOT_READY (NULL or closed handle) /
 *         ALP_ERR_NOT_PRESENT_ON_THIS_SOC (domain not populated on this SKU) /
 *         ALP_ERR_NOSUPPORT (no backend implements domains, or RAIL_OFF is
 *         unavailable for this domain or its build-time gate is off).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.17 (#2784) -- contract only; every backend returns
 *      @ref ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_power_domain_policy_set(alp_power_t              *handle,
                                         alp_power_domain_t        domain,
                                         alp_power_domain_policy_t policy);

/**
 * @brief Describe one power domain on the running SoM.
 *
 * Handle-less and read-only.  @p out is zero-filled first, so on any
 * non-OK return it reads as "not present".
 *
 * @param[in]  domain  Domain to describe.
 * @param[out] out     Receives the description.
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL @p out or @p domain out of range) /
 *         ALP_ERR_NOSUPPORT (no backend implements domains).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.17 (#2784) -- contract only.
 */
alp_status_t alp_power_domain_info(alp_power_domain_t domain, alp_power_domain_info_t *out);

/**
 * @brief Read the wake record of the previous STOP / STANDBY cycle.
 *
 * The recovery path for a sleep that did not return (see the file-level
 * note): call it early in boot.  Handle-less and read-only.  @p out is
 * zero-filled first.
 *
 * @param[out] out  Receives the record; @c valid is false after a plain
 *                  power-on reset.
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL @p out) /
 *         ALP_ERR_NOSUPPORT (no backend keeps a wake record).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New in v0.17 (#2784) -- contract only.
 */
alp_status_t alp_power_boot_wake_info(alp_power_boot_info_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_POWER_H */
