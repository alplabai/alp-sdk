/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rv3028c7.h
 * @brief Micro Crystal RV-3028-C7 32.768 kHz extreme-low-power RTC.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * 1 PPM TCXO accuracy, 100 nA typical I_DD at 3.0 V, integrated
 * trickle charger, programmable alarm with INT pin.  On the
 * E1M-AEN module the RTC sits on Alif's LPI2C bus; its INT line
 * routes to the Alif's `RTC_ALARM` pin (`P15_0_FLEX`) so the
 * application can wake from the alarm.
 *
 * I2C address is fixed at **0x52** (7-bit).
 *
 * Date / time registers are BCD-encoded per datasheet table 4:
 *   0x00  Seconds   (00..59)
 *   0x01  Minutes   (00..59)
 *   0x02  Hours     (00..23 -- 24h mode used here)
 *   0x03  Weekday   (1..7)
 *   0x04  Date      (01..31)
 *   0x05  Month     (01..12)
 *   0x06  Year      (00..99 -- 2-digit, year 2000..2099)
 *
 * The driver reads / writes the seven date-time bytes in one
 * transaction so the RTC's internal latch keeps the values
 * coherent across the rollover boundary.
 */

#ifndef ALP_CHIPS_RV3028C7_H
#define ALP_CHIPS_RV3028C7_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RV3028C7_I2C_ADDR 0x52u

/** Wall-clock representation; same shape as <time.h>'s tm but
 *  packed for I2C transport with no padding. */
typedef struct {
	uint8_t  second;  /**< 0..59 */
	uint8_t  minute;  /**< 0..59 */
	uint8_t  hour;    /**< 0..23 */
	uint8_t  weekday; /**< 1..7 (1 = Sunday by convention) */
	uint8_t  day;     /**< 1..31 */
	uint8_t  month;   /**< 1..12 */
	uint16_t year;    /**< Full year 2000..2099 */
} rv3028c7_time_t;

/** @brief Alarm match mask -- which fields participate in the comparison.
 *  A field with its flag clear is treated as "don't care", so the alarm
 *  fires on every rollover of that field. */
typedef struct {
	bool match_minute;         /**< Compare the minute field. */
	bool match_hour;           /**< Compare the hour field. */
	bool match_day_or_weekday; /**< Compare day-of-month or weekday (see @ref use_weekday). */
	bool use_weekday;          /**< true => match by weekday; false => match by day-of-month. */
} rv3028c7_alarm_match_t;

/** @brief Driver context for one RV-3028-C7 RTC. Caller-allocated; populated by
 *  @ref rv3028c7_init. Not thread-safe: serialise access to a single instance. */
typedef struct {
	bool       initialised; /**< True once @ref rv3028c7_init succeeds. */
	alp_i2c_t *bus;         /**< Caller-opened I2C bus the RTC sits on (not owned). */
	/* Per-source handler table.  Indexed by rv3028c7_src_t.  NULL
     * means "source not registered -- ignore on dispatch".  Default
     * after rv3028c7_init() is all-NULL; the legacy alarm helpers
     * keep working without registering a handler because they
     * bypass the dispatcher entirely. */
	void *src_user[7]; /* RV3028C7_SRC_COUNT */
	/* Stored as void* to avoid pulling in the handler typedef before
     * its declaration; cast happens at call site. */
	void *src_handler[7]; /* rv3028c7_src_handler_t */
} rv3028c7_t;

/** @brief Probe the RTC, clear the oscillator-stop flag (set on
 *         power-on), and configure 24-hour mode.
 *  @param ctx Driver context (output); caller-allocated.
 *  @param bus Caller-opened I2C bus the RTC sits on (not owned, must outlive @p ctx).
 *  @return ALP_OK on success, ALP_ERR_INVAL on NULL args, ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_init(rv3028c7_t *ctx, alp_i2c_t *bus);

/** @brief Read the current wall-clock time.
 *  @param ctx Initialised driver context.
 *  @param out [out] decoded time (BCD-to-binary conversion done internally).
 *  @return ALP_OK on success, ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_get_time(rv3028c7_t *ctx, rv3028c7_time_t *out);

/** @brief Write a wall-clock time (24-hour mode).
 *  @param ctx Initialised driver context.
 *  @param t   Time to set; fields must be within the ranges in @ref rv3028c7_time_t.
 *  @return ALP_OK on success, ALP_ERR_INVAL on out-of-range fields, ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_set_time(rv3028c7_t *ctx, const rv3028c7_time_t *t);

/** @brief Configure the alarm registers + match mask.
 *  @param ctx   Initialised driver context.
 *  @param when  Alarm time; only the fields selected by @p match are compared.
 *  @param match Which fields participate in the match.
 *  @return ALP_OK on success, ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_set_alarm(rv3028c7_t                   *ctx,
                                const rv3028c7_time_t        *when,
                                const rv3028c7_alarm_match_t *match);

/** @brief Enable (or disable) the alarm-flag -> INT pin routing.
 *  @param ctx    Initialised driver context.
 *  @param enable true to route the alarm flag to the INT pin, false to mask it.
 *  @return ALP_OK on success, ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_alarm_int_enable(rv3028c7_t *ctx, bool enable);

/** @brief Read + clear the alarm-fired flag.  Returns *fired = true
 *         iff the alarm has triggered since the last clear.
 *  @param ctx   Initialised driver context.
 *  @param fired [out] true if the alarm latched since the last clear.
 *  @return ALP_OK on success, ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_alarm_check_and_clear(rv3028c7_t *ctx, bool *fired);

/* ---------------------------------------------------------------- */
/* Multi-source event handling                                       */
/* ---------------------------------------------------------------- */

/**
 * The RV-3028-C7 has a single hardware `INT` pin but **multiple
 * latched event sources**.  The `STATUS` register at `0x0E`
 * surfaces a flag per source; the driver provides a registration
 * surface so callers can attach a handler per source and dispatch
 * inside the ISR with a single I2C read.
 *
 * The chip also exposes the `CLKOUT` pin which can be configured to
 * emit pulses on certain events (per Micro Crystal AN
 * "Multiple Interrupt Lines with RV-3028-C7"), giving boards a
 * **second physical output line** that fires independently of `INT`
 * when wired through an event-routing config.  The driver doesn't
 * directly toggle CLKOUT routing -- that's a board-design question
 * carried in the board overlay -- but `rv3028c7_route_clkout` lets
 * firmware reprogram the CLKOUT source bits when the board
 * supports it.
 */

/** Latched event sources surfaced in the `STATUS` register. */
typedef enum {
	RV3028C7_SRC_PORF      = 0, /**< Power-on reset flag (STATUS bit 0). */
	RV3028C7_SRC_EXT_EVENT = 1, /**< External-event flag from EVI pin (bit 1). */
	RV3028C7_SRC_ALARM     = 2, /**< Alarm match (bit 2). */
	RV3028C7_SRC_COUNTDOWN = 3, /**< Countdown-timer underflow (bit 3). */
	RV3028C7_SRC_PERIODIC  = 4, /**< Periodic update flag, 1 s / 1 min (bit 4). */
	RV3028C7_SRC_BSF       = 5, /**< Backup-switchover (VBAT vs Vdd) (bit 5). */
	RV3028C7_SRC_CLKF      = 6, /**< Clock-output sync flag (bit 6). */
	RV3028C7_SRC_COUNT          /**< Number of event sources; sizes the handler table. */
} rv3028c7_src_t;

/** Per-source handler callback.  Runs in the same context that calls
 *  `rv3028c7_dispatch_irq` (typically the application's bottom-half,
 *  not the ISR proper -- the helper is I2C-bound and must not run in
 *  hard-IRQ context).  `user` is the cookie supplied at registration. */
typedef void (*rv3028c7_src_handler_t)(rv3028c7_t *ctx, rv3028c7_src_t src, void *user);

/** @brief Register (or replace) the handler for a specific event source.
 *
 *  @param ctx        RV-3028-C7 driver context (must be initialised first).
 *  @param src        Event source.
 *  @param handler    Callback.  Pass NULL to unregister.
 *  @param user       Cookie passed through to the callback. */
alp_status_t rv3028c7_register_handler(rv3028c7_t            *ctx,
                                       rv3028c7_src_t         src,
                                       rv3028c7_src_handler_t handler,
                                       void                  *user);

/**
 * @brief Read `STATUS`, dispatch registered handlers for each set
 *        bit, and write-back the cleared bits to acknowledge.
 *
 * Call from a bottom-half / work-queue context after the INT line
 * asserts.  Returns the raw `STATUS` byte read (post-dispatch) for
 * diagnostic logging; the actual clear-write happens internally.
 *
 * @param ctx          RV-3028-C7 driver context (must be initialised first).
 * @param status_seen  Output: the latched STATUS value before the
 *                     clear.  May be NULL if the caller doesn't care.
 * @return ALP_OK on a clean dispatch + clear cycle, ALP_ERR_IO on
 *         a transport failure.
 */
alp_status_t rv3028c7_dispatch_irq(rv3028c7_t *ctx, uint8_t *status_seen);

/** Source selector for the `CLKOUT` pin.  These map to the
 *  `CLKOUT_FD[2:0]` field in `EEPROM_CLKOUT` (0x35); writing the
 *  EEPROM image takes effect after the next refresh.  Subset of the
 *  full table -- the practical "use CLKOUT as a second IRQ line"
 *  modes are the periodic-timer + countdown-timer routes. */
typedef enum {
	RV3028C7_CLKOUT_32_768_HZ = 0, /**< 32.768 kHz square wave. */
	RV3028C7_CLKOUT_8192_HZ   = 1, /**< 8192 Hz square wave. */
	RV3028C7_CLKOUT_1024_HZ   = 2, /**< 1024 Hz square wave. */
	RV3028C7_CLKOUT_64_HZ     = 3, /**< 64 Hz square wave. */
	RV3028C7_CLKOUT_32_HZ     = 4, /**< 32 Hz square wave. */
	RV3028C7_CLKOUT_1_HZ      = 5, /**< 1 Hz square wave. */
	RV3028C7_CLKOUT_PERIODIC  = 6, /**< Pulses on Periodic-update (= bit 4 of STATUS) */
	RV3028C7_CLKOUT_LOW       = 7, /**< CLKOUT driven low (effectively disabled). */
} rv3028c7_clkout_src_t;

/** @brief Reprogram the CLKOUT pin's source.  Used by boards that
 *         wire CLKOUT as a second interrupt line (Micro Crystal AN
 *         "Multiple Interrupt Lines with RV-3028-C7").
 *  @param ctx Initialised driver context.
 *  @param src CLKOUT frequency / event source to select.
 *  @return ALP_OK on success, ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_route_clkout(rv3028c7_t *ctx, rv3028c7_clkout_src_t src);

/** @brief Enable / disable specific interrupt sources at the chip
 *         level (mask in CONTROL_2 register).  Per-source mask bits:
 *         EIE (ext event), AIE (alarm), TIE (countdown), UIE
 *         (periodic update), BSIE (backup-switchover), CLKIE
 *         (clock-out sync).
 *  @param ctx    Initialised driver context.
 *  @param src    Event source whose interrupt-enable bit to change.
 *  @param enable true to enable the source's interrupt, false to disable.
 *  @return ALP_OK on success, ALP_ERR_INVAL if @p src has no mask bit,
 *          ALP_ERR_IO on bus error. */
alp_status_t rv3028c7_set_int_enable(rv3028c7_t *ctx, rv3028c7_src_t src, bool enable);

/** @brief Release resources.  Idempotent.
 *  @param ctx Driver context (may be NULL, in which case the call is a no-op). */
void rv3028c7_deinit(rv3028c7_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_RV3028C7_H */
