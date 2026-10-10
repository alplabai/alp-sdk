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
	uint8_t  weekday; /**< 0..6, raw WEEKDAY (03h) counter value -- resets to 0 at
	                    *   POR, no fixed day mapping (App. Manual Rev. 1.4 Sec.
	                    *   3.4 "03h -- Weekday", p.16). */
	uint8_t  day;     /**< 1..31 */
	uint8_t  month;   /**< 1..12 */
	uint16_t year;    /**< Full year 2000..2099 */
} rv3028c7_time_t;

/** Alarm match mask -- which fields participate in the comparison. */
typedef struct {
	bool match_minute;
	bool match_hour;
	bool match_day_or_weekday;
	bool use_weekday; /**< false => match by day-of-month */
} rv3028c7_alarm_match_t;

typedef struct {
	bool initialised;
	/* PORF (Power-On-Reset Flag, STATUS bit 0) as last seen -- and
     * cleared -- by rv3028c7_init().  This part has no separate
     * oscillator-stop flag; PORF is the datasheet's authoritative
     * "is the stored time trustworthy" indicator (Application
     * Manual Rev. 1.4 p.22).  Query via rv3028c7_was_cold_start(). */
	bool       cold_start;
	alp_i2c_t *bus;
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

/** @brief Probe the RTC, record + clear PORF (Power-On-Reset Flag --
 *         this part has no separate oscillator-stop flag; PORF is
 *         set at every power-on and stays latched until firmware
 *         clears it, Application Manual Rev. 1.4 p.22), and force
 *         24-hour mode.
 *
 *  A stale External Event flag (EVF) is cleared too, but only while the
 *  External-event interrupt enable (EIE) is off; with EIE on, EVF is a live
 *  event (possibly the cause of the wake being decoded) and is left set.
 *
 *  Call @ref rv3028c7_was_cold_start afterwards to learn whether
 *  PORF was set going in, i.e. whether the time this RTC is holding
 *  should be treated as trustworthy. */
alp_status_t rv3028c7_init(rv3028c7_t *ctx, alp_i2c_t *bus);

/** @brief Report whether PORF was set at the most recent
 *         @ref rv3028c7_init call.
 *
 *  rv3028c7_init() always clears PORF as part of bring-up, so this
 *  accessor is the only way to learn whether it was set going in --
 *  i.e. whether this is a cold start (RTC just powered up, time not
 *  yet trustworthy) or a warm one (RTC has been running, time is
 *  whatever was last set).
 *
 *  @param ctx  RV-3028-C7 driver context (must be initialised).
 *  @param out  Output: true if PORF was set at init time.
 *  @return ALP_OK, ALP_ERR_INVAL (@p out NULL), or ALP_ERR_NOT_READY. */
alp_status_t rv3028c7_was_cold_start(rv3028c7_t *ctx, bool *out);

/** @brief Read the current wall-clock time.
 *
 *  @return ALP_OK, ALP_ERR_NOT_READY, ALP_ERR_INVAL, or ALP_ERR_IO.
 *          ALP_ERR_IO also covers a stalled-bus read: Application
 *          Manual Rev. 1.4 p.53 -- a transaction slower than 950 ms
 *          trips the part's internal bus timeout and a subsequent
 *          read returns all 0xFF; this function range-checks the
 *          decoded fields and reports that condition as ALP_ERR_IO
 *          rather than handing back a bogus but well-formed-looking
 *          time. */
alp_status_t rv3028c7_get_time(rv3028c7_t *ctx, rv3028c7_time_t *out);

/** @brief Write a wall-clock time (24-hour mode). */
alp_status_t rv3028c7_set_time(rv3028c7_t *ctx, const rv3028c7_time_t *t);

/** @brief Configure the alarm registers + match mask. */
alp_status_t rv3028c7_set_alarm(rv3028c7_t                   *ctx,
                                const rv3028c7_time_t        *when,
                                const rv3028c7_alarm_match_t *match);

/** @brief Enable (or disable) the alarm-flag -> INT pin routing. */
alp_status_t rv3028c7_alarm_int_enable(rv3028c7_t *ctx, bool enable);

/** @brief Read + clear the alarm-fired flag.  Returns *fired = true
 *         iff the alarm has triggered since the last clear. */
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
	RV3028C7_SRC_COUNT
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
 *        bit, and acknowledge only what was actually dispatched.
 *
 * Call from a bottom-half / work-queue context after the INT line
 * asserts.  This may perform more than one internal STATUS
 * read/dispatch/clear round trip: a source that latches while an
 * earlier handler is still doing its own I2C work (or during the
 * acknowledge write itself) is not lost -- it gets its own dispatch
 * pass on this same call instead of being silently cleared unseen.
 *
 * @param ctx          RV-3028-C7 driver context (must be initialised first).
 * @param status_seen  Output: the latched STATUS value from the
 *                     first read, before any dispatch or clear.  May
 *                     be NULL if the caller doesn't care.
 * @return ALP_OK on a clean dispatch + clear cycle, ALP_ERR_IO on a
 *         transport failure, ALP_ERR_NOT_READY if @p ctx has not
 *         been initialised.
 */
alp_status_t rv3028c7_dispatch_irq(rv3028c7_t *ctx, uint8_t *status_seen);

/** Source selector for the `CLKOUT` pin.  These map to the
 *  `CLKOUT_FD[2:0]` field in `EEPROM_CLKOUT` (0x35).
 *  `rv3028c7_route_clkout()` writes the RAM mirror first, and the RAM
 *  mirror -- not the EEPROM backing store -- is the active zone
 *  (Application Manual Rev. 1.4 section 4.6.9, p.57), so the new
 *  source takes effect immediately; the EEPROM commit only makes it
 *  survive the next refresh.  Subset of the full table -- the
 *  practical "use CLKOUT as a second IRQ line" modes are the
 *  periodic-timer + countdown-timer routes. */
typedef enum {
	RV3028C7_CLKOUT_32_768_HZ = 0,
	RV3028C7_CLKOUT_8192_HZ   = 1,
	RV3028C7_CLKOUT_1024_HZ   = 2,
	RV3028C7_CLKOUT_64_HZ     = 3,
	RV3028C7_CLKOUT_32_HZ     = 4,
	RV3028C7_CLKOUT_1_HZ      = 5,
	RV3028C7_CLKOUT_PERIODIC  = 6, /**< Pulses on the predefined periodic countdown-timer
	                                 *   interrupt (TF, STATUS bit 3) -- not UF/bit 4 (p.37). */
	RV3028C7_CLKOUT_LOW       = 7, /**< CLKOUT driven low (effectively disabled). */
} rv3028c7_clkout_src_t;

/** @brief Reprogram the CLKOUT pin's source.  Used by boards that
 *         wire CLKOUT as a second interrupt line (Micro Crystal AN
 *         "Multiple Interrupt Lines with RV-3028-C7"). */
alp_status_t rv3028c7_route_clkout(rv3028c7_t *ctx, rv3028c7_clkout_src_t src);

/** @brief Enable / disable specific interrupt sources at the chip
 *         level (mask in CONTROL_2 register).  Per-source mask bits:
 *         EIE (ext event), AIE (alarm), TIE (countdown), UIE
 *         (periodic update), BSIE (backup-switchover), CLKIE
 *         (clock-out sync). */
alp_status_t rv3028c7_set_int_enable(rv3028c7_t *ctx, rv3028c7_src_t src, bool enable);

/* ---------------------------------------------------------------- */
/* Wake-source services: countdown timer, alarm, flag service        */
/* ---------------------------------------------------------------- */

/**
 * @name Wake-source services (STOP / low-power wake on INT)
 *
 * These calls arm and service the two timed wake sources of the part
 * (Periodic Countdown Timer and Alarm) on the `INT` pin.  Register and
 * bit citations are to the RV-3028-C7 Application Manual Rev. 1.4
 * (November 2021): Timer Value 0/1 (0Ah/0Bh) Sec. 3.6 p.20, Timer
 * Status 0/1 (0Ch/0Dh) p.21, Status (0Eh) p.22, Control 1 (0Fh)
 * TRPT/WADA/EERD/TE/TD p.23, Control 2 (10h) TIE/AIE p.24, Alarm
 * registers 07h..09h Sec. 3.5 pp.18-19, countdown procedure Sec. 4.8.2
 * p.63, first-period table Sec. 4.8.3 p.65.
 *
 * **No EEPROM access.**  Nothing in this group reads or writes the
 * EEPROM or any register of the EEPROM RAM mirror (0x30..0x37,
 * including EEPROM_BACKUP 0x37 and its backup-switchover mode BSM
 * field), and none touches EEADDR/EEDATA/EECMD (0x25..0x27) or the
 * EERD bit.  Every register they use (0x07..0x0F, 0x10) is a plain
 * RAM register.  This is deliberate: an EEPROM write wears the part
 * (nCYCLE endurance, p.98, as low as 100 cycles at the hot corner)
 * and a changed backup-switchover mode changes how the part is
 * powered, which a wake-timing path must never do as a side effect.
 * This is also why the bench power application uses this driver and
 * not Zephyr's `rtc_rv3028` driver, whose init writes the
 * backup-switch mode.
 * @{
 */

/** Longest one-shot countdown rounded to the part's coarsest tick:
 *  4095 x 60 s (TD = 11, 1/60 Hz; Sec. 4.8.2 table p.63). */
#define RV3028C7_TIMER_MAX_SECONDS (4095u * 60u)

/** Longest countdown that keeps 1 s resolution (TD = 10, 1 Hz). */
#define RV3028C7_TIMER_MAX_SECONDS_1HZ 4095u

/** Countdown state, as returned by @ref rv3028c7_timer_read. */
typedef struct {
	bool     running;      /**< TE (Control 1 bit 2) is set: a countdown is in progress. */
	bool     expired;      /**< TF (Status bit 3) is latched. */
	uint32_t preset_ms;    /**< Timer Value (0Ah/0Bh) preset, in ms of the active TD tick. */
	uint32_t remaining_ms; /**< Timer Status (0Ch/0Dh) current value, in ms. */
	uint32_t elapsed_ms;   /**< preset_ms - remaining_ms (saturating at 0). */
} rv3028c7_timer_state_t;

/**
 * @brief Start a one-shot countdown of @p seconds with INT enabled.
 *
 * Picks the Timer Clock Frequency (TD, Control 1 bits 1:0) from the
 * range: 1 Hz (TD = 10) for 1..4095 s, 1/60 Hz (TD = 11) above that.
 * Longer requests are rounded UP to a whole number of minutes so the
 * wake is never early.  The request must be 1..@ref
 * RV3028C7_TIMER_MAX_SECONDS.
 *
 * Follows the Sec. 4.8.2 procedure (p.63): TE, TIE and TF are cleared
 * in that order first (no stray INT), then TRPT = 0 (single mode) and
 * TD are written, then the 12-bit Timer Value, then TIE = 1, and
 * finally TE 0 -> 1 starts the countdown.  WADA, USEL and EERD in
 * Control 1 and the other Control 2 enables are preserved.  Other
 * latched flags in Status (including PORF) are left set.
 *
 * Timing: the first period may run up to 15.625 ms long (Sec. 4.8.3
 * p.65); after expiry TF latches, INT goes low (cleared after tRTN1 =
 * 7.813 ms, or when TF is cleared) and TE self-clears (single mode).
 *
 * @param ctx      Initialised driver context.
 * @param seconds  Requested delay, 1..RV3028C7_TIMER_MAX_SECONDS.
 * @param actual_s Output: the delay actually programmed, in seconds.
 *                 May be NULL.
 * @return ALP_OK, ALP_ERR_NOT_READY, ALP_ERR_INVAL (0 or too long),
 *         or ALP_ERR_IO.
 */
alp_status_t rv3028c7_timer_start(rv3028c7_t *ctx, uint32_t seconds, uint32_t *actual_s);

/**
 * @brief Stop the countdown and silence it: TE = 0, TIE = 0, TF
 *        cleared.  The preset in Timer Value is left alone (writing 0
 *        to a running Timer Value is documented as harmful, p.65).
 *        Idempotent.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK, ALP_ERR_NOT_READY (@p ctx not initialised), or
 *         ALP_ERR_IO on a transport failure.
 */
alp_status_t rv3028c7_timer_stop(rv3028c7_t *ctx);

/**
 * @brief Read the countdown state: running, expired, preset, remaining
 *        and elapsed.
 *
 * Remaining time comes from Timer Status 0/1 (0Ch/0Dh), which hold
 * the live count while TE = 1 and the last value after TE = 0 (p.21).
 * Reading 0Ch first latches 0Dh, so both bytes are read in one
 * transaction.  Units follow the active TD tick (4096 Hz, 64 Hz, 1 Hz
 * or 1/60 Hz), expressed in whole milliseconds.
 *
 * @param ctx  Initialised driver context.
 * @param out  Output: countdown state.
 * @return ALP_OK, ALP_ERR_NOT_READY, ALP_ERR_INVAL (@p out NULL), or
 *         ALP_ERR_IO.
 */
alp_status_t rv3028c7_timer_read(rv3028c7_t *ctx, rv3028c7_timer_state_t *out);

/**
 * @brief Arm the alarm: disable AIE, clear AF, write the 07h..09h alarm
 *        registers (AE bits from @p match, see @ref rv3028c7_set_alarm),
 *        then enable AIE.  Fields not selected in @p match do not
 *        participate in the comparison (AE_x = 1, pp.18-19).
 *
 * @param ctx    Initialised driver context.
 * @param when   Alarm time (minute, hour, and day or weekday).
 * @param match  Which fields participate and whether 09h is a date or a
 *               weekday.
 * @return ALP_OK, ALP_ERR_NOT_READY, ALP_ERR_INVAL (a pointer NULL),
 *         or ALP_ERR_IO.
 */
alp_status_t rv3028c7_alarm_arm(rv3028c7_t                   *ctx,
                                const rv3028c7_time_t        *when,
                                const rv3028c7_alarm_match_t *match);

/**
 * @brief Disarm the alarm: AIE = 0, AE_M/AE_H/AE_WD = 1 (all match
 *        fields disabled, the POR state, pp.18-19) and AF cleared.
 *        Other latched flags are left alone.  Idempotent.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK, ALP_ERR_NOT_READY, or ALP_ERR_IO.
 */
alp_status_t rv3028c7_alarm_clear(rv3028c7_t *ctx);

/** Wake flags reported by @ref rv3028c7_wake_service (bit masks). */
#define RV3028C7_WAKE_TF 0x08u /**< Countdown timer expired (Status bit 3). */
#define RV3028C7_WAKE_AF 0x04u /**< Alarm matched (Status bit 2). */
#define RV3028C7_WAKE_UF 0x10u /**< Periodic update (Status bit 4). */

/**
 * @brief Interrupt service for the wake path: read Status and Control 2,
 *        report the enabled wake sources that are latched, and clear
 *        every latched TF / AF / UF flag.
 *
 * Reported (@p flags) is only a source whose interrupt enable is on:
 * TF needs TIE, AF needs AIE, UF needs UIE (Control 2, p.24).  UF in
 * particular latches every second regardless of UIE (p.22, Sec. 4.9),
 * so it is never a wake cause unless UIE is set.  A latched but
 * unreported flag is still cleared.
 *
 * The acknowledge is a constant-mask STATUS write, not a
 * read-modify-write: 0 for the flags being cleared and 1 for every
 * other latchable flag.  The same convention as
 * rv3028c7_alarm_check_and_clear().  PORF, EVF, BSF and CLKF are
 * therefore preserved, and EEbusy (read-only) is never written.  No
 * handlers are invoked.
 *
 * @note Writing 1 to a STATUS flag has no effect and only writing 0
 *       clears it -- bench-verified on E1M-AEN803 (2026-10-08) for
 *       PORF/EVF/AF/TF/BSF/CLKF; App Manual p.22 states only
 *       "retained until a 0 is written".  The constant mask is kept
 *       over a read-back write because a flag that latches between
 *       the read and the write is not lost.  The strategy lives in one
 *       helper in rv3028c7.c (RV3028_STATUS_WRITE1_IGNORED).
 *
 * @param ctx   Initialised driver context.
 * @param flags Output: OR of RV3028C7_WAKE_* for enabled, latched
 *              sources.  May be NULL.
 * @return ALP_OK, ALP_ERR_NOT_READY, or ALP_ERR_IO.  No STATUS write is
 *         issued when no TF/AF/UF flag is latched.
 */
alp_status_t rv3028c7_wake_service(rv3028c7_t *ctx, uint8_t *flags);

/** @} */

/** @brief Release resources.  Idempotent. */
void rv3028c7_deinit(rv3028c7_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_RV3028C7_H */
