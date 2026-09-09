/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Micro Crystal RV-3028-C7 RTC driver.  See <alp/chips/rv3028c7.h>
 * for the public API.  Register map per RV-3028-C7
 * Application Manual v1.4.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/rv3028c7.h"

#define RV3028_REG_SECONDS   0x00
#define RV3028_REG_MINUTES   0x01
#define RV3028_REG_HOURS     0x02
#define RV3028_REG_WEEKDAY   0x03
#define RV3028_REG_DATE      0x04
#define RV3028_REG_MONTH     0x05
#define RV3028_REG_YEAR      0x06
#define RV3028_REG_ALARM_MIN 0x07
#define RV3028_REG_ALARM_HR  0x08
#define RV3028_REG_ALARM_WD  0x09
#define RV3028_REG_STATUS    0x0E
#define RV3028_REG_CONTROL_1 0x0F
#define RV3028_REG_CONTROL_2 0x10

/* STATUS register bits (RV-3028-C7 Application Manual v1.4, Table 9).
 * Each flag is set by hardware on the corresponding event and stays
 * latched until firmware writes a 0 to that bit (or writes the
 * entire STATUS byte with the bit cleared).  The dispatcher uses
 * the read-modify-write pattern: read STATUS, dispatch handlers for
 * set bits, write back with set bits cleared. */
#define RV3028_STATUS_PORF 0x01 /* Power-on reset flag           */
#define RV3028_STATUS_EVF  0x02 /* External-event flag           */
#define RV3028_STATUS_AF   0x04 /* Alarm flag                    */
#define RV3028_STATUS_TF   0x08 /* Countdown-timer flag          */
#define RV3028_STATUS_UF   0x10 /* Periodic update flag          */
#define RV3028_STATUS_BSF  0x20 /* Backup-switchover flag        */
#define RV3028_STATUS_CLKF 0x40 /* Clock-output sync flag        */
/* Read-only "EEPROM write in flight" bit -- never part of the
 * dispatchable RV3028_STATUS_FLAGS mask below. */
#define RV3028_STATUS_EEBUSY 0x80

/* CONTROL_2 bits (RV-3028-C7 Application Manual Rev. 1.4, Table on
 * p.24).  All defaults are 0 at POR. */
#define RV3028_CTRL2_12_24 0x02 /* Hour format: 0 = 24h, 1 = 12h (p.24) */
#define RV3028_CTRL2_EIE   0x04 /* External-event INT enable     */
#define RV3028_CTRL2_AIE   0x08 /* Alarm INT enable              */
#define RV3028_CTRL2_TIE   0x10 /* Countdown-timer INT enable    */
#define RV3028_CTRL2_UIE   0x20 /* Periodic update INT enable    */
#define RV3028_CTRL2_CLKIE 0x40 /* Clock-out sync INT enable     */
#define RV3028_CTRL2_TSE   0x80 /* Time Stamp Enable (not driven by this driver) */

/* CONTROL_1 / EEPROM_BACKUP additional masks (BSF Backup-switchover
 * INT enable lives in EEPROM_BACKUP at 0x37 bit 6 BSIE). */
#define RV3028_EEPROM_BACKUP_ADDR 0x37u
#define RV3028_EEPROM_BACKUP_BSIE 0x40u

/* EEPROM block for CLKOUT config (0x35); only the bottom 3 bits hold
 * the CLKOUT_FD source selector. */
#define RV3028_EEPROM_CLKOUT_ADDR 0x35u
#define RV3028_EEPROM_CLKOUT_MASK 0x07u

/* EEPROM CMD / address registers for the EEPROM-commit protocol.
 * EEADDR (25h), EEDATA (26h) and EECMD (27h) addresses are per the
 * register overview (Application Manual Rev. 1.4 section 3.2, p.12);
 * section 4.6.3/4.6.5 (pp. 54-55) name what each register does but
 * never give the addresses themselves.
 *
 * RV3028_EE_CMD_UPDATE (0x11) commits the WHOLE RAM mirror block
 * 0x30-0x37 -- not just CLKOUT/OFFSET/BACKUP (0x35-0x37) -- including
 * the password registers EEPWE (0x30) and EEPW0..3 (0x31-0x34)
 * (Application Manual Rev. 1.4 register-27h table p.34, and section
 * 4.6.9 p.57).  Whether it also touches 2Bh (EEPROM RESERVED, "must
 * not be overwritten") is UNRESOLVED: section 3.15 (p.35) lists 2Bh
 * as mirrored alongside 0x30-0x37, but the register-27h table (p.34)
 * and the section 4.6.9 update-all figure (p.57) both describe the
 * copy as 0x30-0x37 only, with no 2Bh.  Do not assert either way.
 * This driver does not use RV3028_EE_CMD_UPDATE for that reason --
 * BSIE / CLKOUT commits go through the single-byte RV3028_EE_CMD_WRITE
 * (0x21) path instead, which touches only the one register changed. */
#define RV3028_REG_EE_ADDR   0x25u
#define RV3028_REG_EE_DATA   0x26u
#define RV3028_REG_EE_CMD    0x27u
#define RV3028_EE_CMD_FIRST  0x00u /* Step 1: arm the handshake                */
#define RV3028_EE_CMD_UPDATE 0x11u /* Bulk update -- unused, see comment above */
#define RV3028_EE_CMD_WRITE  0x21u /* Step 2, single byte: commit EEADDR/EEDATA */
#define RV3028_EE_CMD_READ   0x22u /* Read single EEPROM byte via EEADDR into EEDATA */

/* p.56: wait >= 10 ms after issuing a write (>= 1 ms after a read)
 * before the first EEbusy poll; then poll STATUS bit 7 (EEbusy)
 * until it clears.  Bounded so a wedged part can't hang the caller
 * forever.  The same poll (without the prewait) also serves as the
 * pre-command "is a previous cycle still running" check -- see
 * rv3028_eeprom_wait_not_busy().
 *
 * POLL_MAX * POLL_STEP_MS = 125 ms total budget.  tPREFR -- the
 * longest EEPROM timing this poll has to outlast -- is spec'd p.98
 * TYP 66 ms only, with no MIN/MAX given.  tWRITE is the one EEPROM
 * timing p.98 does give a spread for: MIN 4 / TYP 16 / MAX 30 ms, a
 * ~1.9x MAX/TYP ratio.  125 ms applies that same ~1.9x margin to
 * tPREFR's TYP-only figure (125 / 66 ~= 1.9) instead of trusting a
 * bare 1.44x (100 ms) against a number with no guaranteed ceiling. */
#define RV3028_EEPROM_COMMIT_PREWAIT_MS   10u
#define RV3028_EEPROM_READ_PREWAIT_MS     1u /* p.56: read side of the same wait needs only 1 ms */
#define RV3028_EEPROM_COMMIT_POLL_MAX     26u
#define RV3028_EEPROM_COMMIT_POLL_STEP_MS 5u

/* Endurance (Application Manual Rev. 1.4 p.98, nCYCLE): 10'000 write
 * cycles min at VDD 3.0 V / 25 C, but only 100 cycles min at VDD
 * 5.5 V / 85 C -- the hot-corner figure is the one that governs.  A
 * caller that issues an EEPROM commit unconditionally on every call
 * (e.g. set_int_enable()/route_clkout() on every boot, or every loop
 * iteration) burns through that 100-cycle minimum fast.
 * rv3028c7_set_int_enable() and rv3028c7_route_clkout() below skip
 * the EEPROM commit when the target byte already matches what's
 * committed there -- do NOT "simplify" that guard away.  It reduces
 * the wear, it does not remove it: examples/v2n/v2n-rtc-multi-alarm
 * toggles BSIE true then false every run, so 2 of its 3 EEPROM-
 * touching calls change the byte every time and are never skipped
 * (only its route_clkout() call is idempotent after the first run).
 * That takes the part from ~33 runs to the 100-cycle floor down to
 * ~50 runs, not to "never". */

/* CONTROL_1 bit for EEPROM auto-refresh (EERD); the chip pauses
 * automatic refresh when EERD=1 so firmware can do a clean
 * read-modify-write to the EEPROM-backed config registers. */
#define RV3028_CTRL1_EERD 0x08

/* Alarm enable bits live in the alarm registers themselves (top
 * bit "AE" of each MIN/HR/WD register; 0 = participates in match,
 * 1 = ignored). */
#define RV3028_ALARM_AE 0x80

static uint8_t bcd_to_bin(uint8_t bcd)
{
	return (bcd & 0x0F) + ((bcd >> 4) & 0x0F) * 10;
}
static uint8_t bin_to_bcd(uint8_t bin)
{
	return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

/* Shared field bounds for a decoded rv3028c7_time_t, used both to
 * reject an out-of-range set_time() request and to reject a garbage
 * get_time() decode (see rv3028c7_get_time()'s p.53 bus-timeout
 * note).
 *
 * WEEKDAY (03h) is a raw 3-bit counter -- 0..6, wrapping 6 -> 0, with
 * NO datasheet-fixed day mapping (Application Manual Rev. 1.4 Sec.
 * 3.4 "03h -- Weekday", p.16).  0 is that register's own POR RESET
 * value ("Weekday 1 -- Default value" = 0b000, same page), not a
 * fault code -- a board with no VBACKUP (every power cycle is a cold
 * start, PORF always set) legitimately reads weekday=0 while seconds/
 * minutes/hours are perfectly valid and advancing.  A prior version
 * of this check required weekday in 1..7, which rejected that exact
 * POR-default reading as if it were the same p.53 bus-timeout garbage
 * this function exists to catch -- see aen-evk-demo phase_rtc_temp()
 * (examples/aen/aen-evk-demo), where a bench run on real E1M-AEN803
 * silicon showed get_time() failing both calls (rc0/rc1 both
 * ALP_ERR_IO) while the seconds register measurably advanced. */
static bool time_fields_in_range(const rv3028c7_time_t *t)
{
	return t->second <= 59 && t->minute <= 59 && t->hour <= 23 && t->weekday <= 6 && t->day >= 1 &&
	       t->day <= 31 && t->month >= 1 && t->month <= 12 && t->year >= 2000 && t->year <= 2099;
}

static alp_status_t rv3028_read(rv3028c7_t *ctx, uint8_t reg, uint8_t *buf, size_t len)
{
	return alp_i2c_write_read(ctx->bus, RV3028C7_I2C_ADDR, &reg, 1, buf, len);
}
static alp_status_t rv3028_write(rv3028c7_t *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
	uint8_t scratch[16];
	if (len + 1 > sizeof(scratch)) return ALP_ERR_INVAL;
	scratch[0] = reg;
	memcpy(scratch + 1, data, len);
	return alp_i2c_write(ctx->bus, RV3028C7_I2C_ADDR, scratch, len + 1);
}

static alp_status_t rv3028_write_reg(rv3028c7_t *ctx, uint8_t reg, uint8_t val)
{
	return rv3028_write(ctx, reg, &val, 1);
}

alp_status_t rv3028c7_init(rv3028c7_t *ctx, alp_i2c_t *bus)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus = bus;

	/* Probe by reading STATUS; any I2C error -> NOT_READY. */
	uint8_t      status = 0;
	alp_status_t s      = rv3028_read(ctx, RV3028_REG_STATUS, &status, 1);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;

	/* Record + clear PORF if present.  PORF is this part's only
     * "is the stored time trustworthy?" flag (no oscillator-stop
     * flag exists on the RV-3028-C7); stash it in ctx before
     * clearing so rv3028c7_was_cold_start() can report it. */
	ctx->cold_start = (status & RV3028_STATUS_PORF) != 0;
	if (ctx->cold_start) {
		s = rv3028_write_reg(ctx, RV3028_REG_STATUS, status & ~RV3028_STATUS_PORF);
		if (s != ALP_OK) return s;
	}

	/* Force 24-hour mode.  CONTROL_2 bit 1 (12_24) is 0 = 24h / 1 =
     * 12h (Application Manual Rev. 1.4 p.24) -- the opposite of what
     * a bit previously (mis-)named "24H" would suggest.  This driver
     * always encodes hour as 0..23 (rv3028c7_get_time/set_time), so
     * clear the bit explicitly rather than rely on the POR default
     * (0) staying untouched. */
	uint8_t ctrl2 = 0;
	s             = rv3028_read(ctx, RV3028_REG_CONTROL_2, &ctrl2, 1);
	if (s != ALP_OK) return s;
	ctrl2 &= (uint8_t)~RV3028_CTRL2_12_24;
	s = rv3028_write_reg(ctx, RV3028_REG_CONTROL_2, ctrl2);
	if (s != ALP_OK) return s;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t rv3028c7_was_cold_start(rv3028c7_t *ctx, bool *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	*out = ctx->cold_start;
	return ALP_OK;
}

alp_status_t rv3028c7_get_time(rv3028c7_t *ctx, rv3028c7_time_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;

	uint8_t      buf[7] = { 0 };
	alp_status_t s      = rv3028_read(ctx, RV3028_REG_SECONDS, buf, sizeof(buf));
	if (s != ALP_OK) return s;

	rv3028c7_time_t decoded;
	decoded.second  = bcd_to_bin(buf[0] & 0x7F);
	decoded.minute  = bcd_to_bin(buf[1] & 0x7F);
	decoded.hour    = bcd_to_bin(buf[2] & 0x3F);
	decoded.weekday = (uint8_t)(buf[3] & 0x07);
	decoded.day     = bcd_to_bin(buf[4] & 0x3F);
	decoded.month   = bcd_to_bin(buf[5] & 0x1F);
	decoded.year    = (uint16_t)(2000 + bcd_to_bin(buf[6] & 0xFF));

	/* p.53: a transaction slower than 950 ms trips the part's
     * internal bus timeout, and a subsequent read returns all 0xFF
     * -- which decodes to a well-formed-looking but bogus time
     * (e.g. second=85).  Reject it instead of handing it to the
     * caller as valid. */
	if (!time_fields_in_range(&decoded)) return ALP_ERR_IO;

	*out = decoded;
	return ALP_OK;
}

alp_status_t rv3028c7_set_time(rv3028c7_t *ctx, const rv3028c7_time_t *t)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (t == NULL) return ALP_ERR_INVAL;
	if (!time_fields_in_range(t)) return ALP_ERR_INVAL;

	uint8_t buf[7];
	buf[0] = bin_to_bcd(t->second);
	buf[1] = bin_to_bcd(t->minute);
	buf[2] = bin_to_bcd(t->hour);
	buf[3] = (uint8_t)(t->weekday & 0x07);
	buf[4] = bin_to_bcd(t->day);
	buf[5] = bin_to_bcd(t->month);
	buf[6] = bin_to_bcd((uint8_t)(t->year - 2000));
	return rv3028_write(ctx, RV3028_REG_SECONDS, buf, sizeof(buf));
}

alp_status_t rv3028c7_set_alarm(rv3028c7_t                   *ctx,
                                const rv3028c7_time_t        *when,
                                const rv3028c7_alarm_match_t *match)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (when == NULL || match == NULL) return ALP_ERR_INVAL;

	uint8_t buf[3];
	/* AE bit = 1 disables that field's match comparison. */
	buf[0] = bin_to_bcd(when->minute) | (match->match_minute ? 0 : RV3028_ALARM_AE);
	buf[1] = bin_to_bcd(when->hour) | (match->match_hour ? 0 : RV3028_ALARM_AE);
	if (match->use_weekday) {
		buf[2] = (when->weekday & 0x07) | (match->match_day_or_weekday ? 0 : RV3028_ALARM_AE);
	} else {
		buf[2] = bin_to_bcd(when->day) | (match->match_day_or_weekday ? 0 : RV3028_ALARM_AE);
	}
	/* Set / clear WADA in CONTROL_1 to pick weekday-or-date semantics. */
	uint8_t      ctrl1 = 0;
	alp_status_t s     = rv3028_read(ctx, RV3028_REG_CONTROL_1, &ctrl1, 1);
	if (s != ALP_OK) return s;
	if (match->use_weekday)
		ctrl1 &= ~0x20;
	else
		ctrl1 |= 0x20;
	s = rv3028_write_reg(ctx, RV3028_REG_CONTROL_1, ctrl1);
	if (s != ALP_OK) return s;

	return rv3028_write(ctx, RV3028_REG_ALARM_MIN, buf, sizeof(buf));
}

alp_status_t rv3028c7_alarm_int_enable(rv3028c7_t *ctx, bool enable)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	uint8_t      ctrl2 = 0;
	alp_status_t s     = rv3028_read(ctx, RV3028_REG_CONTROL_2, &ctrl2, 1);
	if (s != ALP_OK) return s;
	if (enable)
		ctrl2 |= RV3028_CTRL2_AIE;
	else
		ctrl2 &= ~RV3028_CTRL2_AIE;
	return rv3028_write_reg(ctx, RV3028_REG_CONTROL_2, ctrl2);
}

alp_status_t rv3028c7_alarm_check_and_clear(rv3028c7_t *ctx, bool *fired)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (fired == NULL) return ALP_ERR_INVAL;
	uint8_t      status = 0;
	alp_status_t s      = rv3028_read(ctx, RV3028_REG_STATUS, &status, 1);
	if (s != ALP_OK) return s;
	*fired = (status & RV3028_STATUS_AF) != 0;
	if (*fired) {
		s = rv3028_write_reg(ctx, RV3028_REG_STATUS, status & ~RV3028_STATUS_AF);
	}
	return s;
}

/* ---------------------------------------------------------------- */
/* Multi-source event handling                                       */
/* ---------------------------------------------------------------- */

/* Map source -> STATUS bit mask + CONTROL_2 enable bit mask + which
 * register holds the enable.  PORF has no enable bit (always
 * latched on POR); CLKF / BSF live in CONTROL_2 / EEPROM_BACKUP. */
struct src_info {
	uint8_t status_bit;
	uint8_t enable_bit;
	uint8_t enable_reg; /* 0 = CONTROL_2, 1 = EEPROM_BACKUP */
};
static const struct src_info src_tab[RV3028C7_SRC_COUNT] = {
	[RV3028C7_SRC_PORF]      = { RV3028_STATUS_PORF, 0, 0 },
	[RV3028C7_SRC_EXT_EVENT] = { RV3028_STATUS_EVF, RV3028_CTRL2_EIE, 0 },
	[RV3028C7_SRC_ALARM]     = { RV3028_STATUS_AF, RV3028_CTRL2_AIE, 0 },
	[RV3028C7_SRC_COUNTDOWN] = { RV3028_STATUS_TF, RV3028_CTRL2_TIE, 0 },
	[RV3028C7_SRC_PERIODIC]  = { RV3028_STATUS_UF, RV3028_CTRL2_UIE, 0 },
	[RV3028C7_SRC_BSF]       = { RV3028_STATUS_BSF, RV3028_EEPROM_BACKUP_BSIE, 1 },
	[RV3028C7_SRC_CLKF]      = { RV3028_STATUS_CLKF, RV3028_CTRL2_CLKIE, 0 },
};

alp_status_t rv3028c7_register_handler(rv3028c7_t            *ctx,
                                       rv3028c7_src_t         src,
                                       rv3028c7_src_handler_t handler,
                                       void                  *user)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)src >= RV3028C7_SRC_COUNT) return ALP_ERR_INVAL;
	ctx->src_handler[src] = (void *)handler;
	ctx->src_user[src]    = user;
	return ALP_OK;
}

/* Dispatchable STATUS bits.  Bit 7 (EEBUSY) is a read-only "EEPROM write
 * in flight" indicator, not an event, and never takes part in an
 * acknowledge write.
 *
 * Passes one rv3028c7_dispatch_irq call will make before it stops
 * draining STATUS -- see the drain comment inside the function. */
#define RV3028_STATUS_FLAGS        0x7Fu
#define RV3028_DISPATCH_MAX_PASSES 4u

alp_status_t rv3028c7_dispatch_irq(rv3028c7_t *ctx, uint8_t *status_seen)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	uint8_t      status = 0;
	alp_status_t s      = rv3028_read(ctx, RV3028_REG_STATUS, &status, 1);
	if (s != ALP_OK) return s;
	if (status_seen != NULL) *status_seen = status;

	status &= RV3028_STATUS_FLAGS;

	/* Drain inside this call instead of trusting a further INT.  The
     * flags preserved by the acknowledge below keep the chip's
     * open-drain INT pin held low, so a host that latched on the
     * falling edge sees no new edge and would never call back in for
     * them.  Bounded, so a source re-arming underneath us (a 1 s
     * PERIODIC tick, a chattering EVI) cannot pin the bottom half
     * here: the leftovers stay latched, INT stays low, and the next
     * dispatch picks them up. */
	for (unsigned pass = 0; status != 0u && pass < RV3028_DISPATCH_MAX_PASSES; ++pass) {
		/* Dispatch handlers for each set source flag. */
		for (unsigned i = 0; i < RV3028C7_SRC_COUNT; ++i) {
			if ((status & src_tab[i].status_bit) == 0) continue;
			rv3028c7_src_handler_t cb = (rv3028c7_src_handler_t)ctx->src_handler[i];
			if (cb != NULL) cb(ctx, (rv3028c7_src_t)i, ctx->src_user[i]);
		}

		/* Acknowledge exactly the flags dispatched above and nothing
         * else.  Each handler does its own I2C work, so the chip has
         * had several bus transactions in which to latch a fresh
         * flag; a flat write of 0x00 clears those late arrivals too,
         * and ALARM / EXT_EVENT / BSF are one-shot -- no "next event"
         * re-latches them, so the event is gone with INT de-asserted
         * behind it and no error anywhere.
         *
         * Re-read instead of writing ~status: the flag bits are
         * plain R/W, so writing a 1 into a position the chip reads
         * as 0 manufactures an event.  Writing back (fresh & ~status)
         * puts a 1 only where the chip just showed us one.  What
         * remains is the single transaction between this read and the
         * write -- the part has no atomic acknowledge, so that is the
         * floor for this register. */
		uint8_t fresh = 0;
		s             = rv3028_read(ctx, RV3028_REG_STATUS, &fresh, 1);
		if (s != ALP_OK) return s;
		fresh &= RV3028_STATUS_FLAGS;

		status = (uint8_t)(fresh & ~status); /* latched while handlers ran */
		s      = rv3028_write_reg(ctx, RV3028_REG_STATUS, status);
		if (s != ALP_OK) return s;

		/* status_seen reports every flag this call found latched, so a
         * diagnostic log shows the late arrival too, not just the
         * first read. */
		if (status_seen != NULL) *status_seen |= status;
	}

	return ALP_OK;
}

/* Bounded wait for EEbusy (STATUS bit 7) to clear.  A set EEbusy bit
 * makes the part "ignore any further commands until the current one
 * is finished" (Application Manual Rev. 1.4 p.56).  Two distinct
 * uses, both drawn from the p.56 EEbusy flowchart:
 *
 *   1. Required pre-check, called by the CALLER right after setting
 *      EERD = 1 and BEFORE it reads the RAM mirror it is about to
 *      read-modify-write ("Set EERD = 1 / Check for EEbusy = 0:
 *      Access EEPROM only if not busy" is the flowchart's first two
 *      steps, in that order).  This is the call that protects the
 *      mirror RMW: skip it and a POR refresh still in flight
 *      (~66 ms tPREFR) can finish *after* the RMW has read the
 *      pre-refresh mirror and *before* the commit that follows,
 *      silently overwriting the mirror with the refreshed value so
 *      EECMD 0x21 commits a byte derived from data that's no longer
 *      what was intended.
 *
 *   2. Post-command poll inside rv3028_eeprom_write_byte() /
 *      rv3028_eeprom_read_byte(), which also re-runs this same check
 *      first as a secondary guard against stacking on a cycle left
 *      running by a *previous*, unrelated commit.  By the time that
 *      internal check runs, any mirror read/write the caller already
 *      did has already happened -- it does NOT protect the RMW; only
 *      use (1), at the caller, does.
 *
 * Bounded so a wedged EEbusy bit can't hang the caller forever. */
static alp_status_t rv3028_eeprom_wait_not_busy(rv3028c7_t *ctx)
{
	for (unsigned poll = 0; poll < RV3028_EEPROM_COMMIT_POLL_MAX; ++poll) {
		uint8_t      status = 0;
		alp_status_t s      = rv3028_read(ctx, RV3028_REG_STATUS, &status, 1);
		if (s != ALP_OK) return s;
		if ((status & RV3028_STATUS_EEBUSY) == 0) return ALP_OK;
		/* No point sleeping before the loop exits without re-reading --
         * that just adds POLL_STEP_MS of pure latency to a timeout the
         * caller is about to receive anyway. */
		if (poll + 1 < RV3028_EEPROM_COMMIT_POLL_MAX)
			alp_delay_ms(RV3028_EEPROM_COMMIT_POLL_STEP_MS);
	}
	return ALP_ERR_TIMEOUT;
}

/* Commit exactly one byte of the RAM-shadow config (e.g. EEPROM_CLKOUT
 * 0x35 or EEPROM_BACKUP 0x37) to the EEPROM backing store, per "Write
 * To One EEPROM Byte" (Application Manual Rev. 1.4 section 4.6.5,
 * p.55): load EEADDR + EEDATA, then EECMD = 0x00, then EECMD = 0x21,
 * wait >= 10 ms before the first EEbusy poll, then poll STATUS bit 7
 * (EEbusy) until it clears.  Deliberately narrower than the bulk
 * "Update All" (EECMD = 0x11) commit -- that opcode rewrites the
 * whole RAM mirror 0x30-0x37 including the password registers, which
 * is far more than a single BSIE/CLKOUT change needs (see the
 * RV3028_EE_CMD_UPDATE comment above).  Caller is responsible for the
 * EERD gate around this (set before the RAM write, restored after
 * this returns).
 *
 * Without this, a BSIE / CLKOUT change written to the RAM mirror
 * only is silently reverted by the next automatic EEPROM refresh
 * (24 h periodic, or ~66 ms tPREFR at power-on) -- the function
 * names on the callers imply a durable setting, so make it one. */
static alp_status_t rv3028_eeprom_write_byte(rv3028c7_t *ctx, uint8_t addr, uint8_t data)
{
	alp_status_t s = rv3028_eeprom_wait_not_busy(ctx);
	if (s != ALP_OK) return s;

	s = rv3028_write_reg(ctx, RV3028_REG_EE_ADDR, addr);
	if (s != ALP_OK) return s;
	s = rv3028_write_reg(ctx, RV3028_REG_EE_DATA, data);
	if (s != ALP_OK) return s;

	s = rv3028_write_reg(ctx, RV3028_REG_EE_CMD, RV3028_EE_CMD_FIRST);
	if (s != ALP_OK) return s;
	s = rv3028_write_reg(ctx, RV3028_REG_EE_CMD, RV3028_EE_CMD_WRITE);
	if (s != ALP_OK) return s;

	alp_delay_ms(RV3028_EEPROM_COMMIT_PREWAIT_MS);
	return rv3028_eeprom_wait_not_busy(ctx);
}

/* Read exactly one byte of the EEPROM backing store (not the RAM
 * mirror) into *out, per "Read One EEPROM Byte" (Application Manual
 * Rev. 1.4 section 4.6.6, p.55): load EEADDR, then EECMD = 0x00, then
 * EECMD = 0x22, wait >= 1 ms before the first EEbusy poll (the read
 * side of the p.56 wait needs only 1 ms, not the 10 ms the write side
 * needs), then poll STATUS bit 7 (EEbusy) until it clears, then read
 * EEDATA.  Costs zero write-cycle endurance -- p.98's nCYCLE entry is
 * tied to tWRITE / EECMD 0x21, not to tREAD / EECMD 0x22 -- so it is
 * safe to call on every rv3028c7_set_int_enable() /
 * rv3028c7_route_clkout() invocation as the endurance guard's source
 * of truth.  Caller is responsible for the EERD gate around this,
 * same as rv3028_eeprom_write_byte(). */
static alp_status_t rv3028_eeprom_read_byte(rv3028c7_t *ctx, uint8_t addr, uint8_t *out)
{
	alp_status_t s = rv3028_eeprom_wait_not_busy(ctx);
	if (s != ALP_OK) return s;

	s = rv3028_write_reg(ctx, RV3028_REG_EE_ADDR, addr);
	if (s != ALP_OK) return s;

	s = rv3028_write_reg(ctx, RV3028_REG_EE_CMD, RV3028_EE_CMD_FIRST);
	if (s != ALP_OK) return s;
	s = rv3028_write_reg(ctx, RV3028_REG_EE_CMD, RV3028_EE_CMD_READ);
	if (s != ALP_OK) return s;

	alp_delay_ms(RV3028_EEPROM_READ_PREWAIT_MS);
	s = rv3028_eeprom_wait_not_busy(ctx);
	if (s != ALP_OK) return s;

	return rv3028_read(ctx, RV3028_REG_EE_DATA, out, 1);
}

alp_status_t rv3028c7_set_int_enable(rv3028c7_t *ctx, rv3028c7_src_t src, bool enable)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)src >= RV3028C7_SRC_COUNT) return ALP_ERR_INVAL;
	if (src == RV3028C7_SRC_PORF) return ALP_ERR_NOSUPPORT;

	const struct src_info *info = &src_tab[src];

	if (info->enable_reg == 0) {
		/* CONTROL_2 path: simple RMW. */
		uint8_t      ctrl2 = 0;
		alp_status_t s     = rv3028_read(ctx, RV3028_REG_CONTROL_2, &ctrl2, 1);
		if (s != ALP_OK) return s;
		if (enable)
			ctrl2 |= info->enable_bit;
		else
			ctrl2 &= (uint8_t)~info->enable_bit;
		return rv3028_write_reg(ctx, RV3028_REG_CONTROL_2, ctrl2);
	}

	/* EEPROM_BACKUP path -- the BSIE bit lives in a non-volatile
     * register that auto-refreshes from EEPROM.  Pause auto-refresh
     * (set EERD in CONTROL_1), wait for any refresh/commit already in
     * flight to finish -- p.56 EEbusy flowchart step 2, and this MUST
     * happen before the RAM-mirror read below: a POR refresh that's
     * still copying EEPROM -> RAM can otherwise complete mid-RMW and
     * clobber it (see rv3028_eeprom_wait_not_busy()) -- then update
     * the byte in the RAM mirror, commit it to EEPROM
     * (rv3028_eeprom_write_byte -- else the next auto-refresh
     * silently reverts it), restore EERD. */
	uint8_t      ctrl1 = 0;
	alp_status_t s     = rv3028_read(ctx, RV3028_REG_CONTROL_1, &ctrl1, 1);
	if (s != ALP_OK) return s;
	uint8_t orig_eerd = ctrl1 & RV3028_CTRL1_EERD;
	if (orig_eerd == 0) {
		s = rv3028_write_reg(ctx, RV3028_REG_CONTROL_1, ctrl1 | RV3028_CTRL1_EERD);
		if (s != ALP_OK) return s;
	}
	s = rv3028_eeprom_wait_not_busy(ctx);

	if (s == ALP_OK) {
		uint8_t bkup = 0;
		s            = rv3028_read(ctx, RV3028_EEPROM_BACKUP_ADDR, &bkup, 1);
		if (s == ALP_OK) {
			uint8_t new_bkup = bkup;
			if (enable)
				new_bkup |= info->enable_bit;
			else
				new_bkup &= (uint8_t)~info->enable_bit;
			s = rv3028_write_reg(ctx, RV3028_EEPROM_BACKUP_ADDR, new_bkup);
			/* Endurance guard (p.98 nCYCLE, see comment above the
             * RV3028_EEPROM_COMMIT_* constants).  Compare against the
             * byte actually committed in EEPROM -- read back via
             * RV3028_EE_CMD_READ, zero endurance cost -- rather than
             * the RAM mirror we just wrote: after a failed
             * rv3028_eeprom_write_byte(), a retry's mirror
             * read-then-compute lands on the same new_bkup as the
             * failed attempt, so a mirror-to-mirror compare would
             * falsely read "unchanged" and skip the commit forever,
             * leaving EEPROM holding the stale byte until the 24 h
             * auto-refresh reverts the mirror back to it. */
			uint8_t ee_bkup = 0;
			if (s == ALP_OK) s = rv3028_eeprom_read_byte(ctx, RV3028_EEPROM_BACKUP_ADDR, &ee_bkup);
			if (s == ALP_OK && ee_bkup != new_bkup) {
				s = rv3028_eeprom_write_byte(ctx, RV3028_EEPROM_BACKUP_ADDR, new_bkup);
			}
		}
	}

	if (orig_eerd == 0 && s != ALP_ERR_TIMEOUT) {
		/* Don't clear EERD (re-enable auto-refresh) over a cycle that
         * may still be in flight -- p.56 recommends clearing EERD
         * only after the access completes; if `s` is ALP_ERR_TIMEOUT,
         * EEbusy may still be set and this write would collide with
         * whatever the part is still doing. */
		alp_status_t r = rv3028_write_reg(ctx, RV3028_REG_CONTROL_1, ctrl1);
		if (s == ALP_OK) s = r;
	}
	return s;
}

alp_status_t rv3028c7_route_clkout(rv3028c7_t *ctx, rv3028c7_clkout_src_t src)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if ((unsigned)src > (unsigned)RV3028C7_CLKOUT_LOW) return ALP_ERR_INVAL;

	/* Same EERD-protected RMW pattern as the BSIE handling above --
     * CLKOUT source bits live in EEPROM_CLKOUT (0x35) bits[2:0] --
     * committed via rv3028_eeprom_write_byte() so the change survives
     * the next auto-refresh instead of being silently reverted.  See
     * rv3028c7_set_int_enable() for why the busy-wait has to run
     * before the RAM-mirror read, and why the endurance guard reads
     * EEPROM back instead of comparing the mirror to itself. */
	uint8_t      ctrl1 = 0;
	alp_status_t s     = rv3028_read(ctx, RV3028_REG_CONTROL_1, &ctrl1, 1);
	if (s != ALP_OK) return s;
	uint8_t orig_eerd = ctrl1 & RV3028_CTRL1_EERD;
	if (orig_eerd == 0) {
		s = rv3028_write_reg(ctx, RV3028_REG_CONTROL_1, ctrl1 | RV3028_CTRL1_EERD);
		if (s != ALP_OK) return s;
	}
	s = rv3028_eeprom_wait_not_busy(ctx);

	if (s == ALP_OK) {
		uint8_t v = 0;
		s         = rv3028_read(ctx, RV3028_EEPROM_CLKOUT_ADDR, &v, 1);
		if (s == ALP_OK) {
			uint8_t new_v = (uint8_t)((v & ~RV3028_EEPROM_CLKOUT_MASK) |
			                          ((uint8_t)src & RV3028_EEPROM_CLKOUT_MASK));
			s             = rv3028_write_reg(ctx, RV3028_EEPROM_CLKOUT_ADDR, new_v);
			/* Endurance guard (p.98 nCYCLE, see comment above the
             * RV3028_EEPROM_COMMIT_* constants). */
			uint8_t ee_v = 0;
			if (s == ALP_OK) s = rv3028_eeprom_read_byte(ctx, RV3028_EEPROM_CLKOUT_ADDR, &ee_v);
			if (s == ALP_OK && ee_v != new_v) {
				s = rv3028_eeprom_write_byte(ctx, RV3028_EEPROM_CLKOUT_ADDR, new_v);
			}
		}
	}

	if (orig_eerd == 0 && s != ALP_ERR_TIMEOUT) {
		/* See rv3028c7_set_int_enable(): don't clear EERD over a
         * cycle that may still be in flight. */
		alp_status_t r = rv3028_write_reg(ctx, RV3028_REG_CONTROL_1, ctrl1);
		if (s == ALP_OK) s = r;
	}
	return s;
}

void rv3028c7_deinit(rv3028c7_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
	for (unsigned i = 0; i < RV3028C7_SRC_COUNT; ++i) {
		ctx->src_handler[i] = NULL;
		ctx->src_user[i]    = NULL;
	}
}
