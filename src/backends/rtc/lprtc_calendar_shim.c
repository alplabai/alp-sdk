/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LPRTC counter -> calendar shim backend for the alp_rtc_* surface on the
 * Alif Ensemble E8 (silicon_ref "alif:ensemble:e8").
 *
 * The E8 always-on LPRTC (lprtc@42000000, compatible "snps,dw-apb-rtc") is a
 * Synopsys DesignWare APB RTC: despite the name it is a Zephyr COUNTER-class
 * device -- a free-running 32-bit up-counter (CCVR) clocked at 32768 Hz from
 * the VBAT-domain LF source.  It has NO hardware date/time registers, so it
 * cannot directly back the calendar alp_rtc_* API (year/month/day/...).
 *
 * This backend bridges the gap entirely in SDK C, over the already-binding
 * Zephyr counter device -- it does NOT introduce a new Zephyr driver (ADR 0017:
 * stay above the vendor SDK; consume the Tier-2 counter_dw_rtc driver as-is).
 *
 * Model (software epoch base, see examples/aen/aen-rtc-regcheck/README.md):
 *   - epoch_base   : UNIX seconds at the instant alp_rtc_set_time() was called.
 *   - tick_snapshot: counter_get_value() captured at that same instant.
 *   - get_time     : epoch_base + (counter_now - tick_snapshot) / freq, where
 *                    freq = counter_get_frequency() (32768 Hz on this node).
 *   - set_time     : recompute epoch_base from the supplied calendar fields and
 *                    re-snapshot the counter.
 * Unsigned 32-bit subtraction (counter_now - tick_snapshot) yields the correct
 * elapsed tick count across a single 32-bit wrap (~36 h at 32768 Hz).
 *
 * Volatility: epoch_base + tick_snapshot live in plain RAM only.  The DW-APB-RTC
 * has no battery-backed scratch to hold them, so the calendar resets to the
 * compiled epoch on every cold boot until set_time is called again.  Persisting
 * (epoch_base, tick_snapshot) into retained storage (VBAT scratch vs MRAM vs a
 * filesystem) is a SoM-policy decision and is deliberately left TBD here; this
 * shim is the in-RAM half that the persistence layer would wrap.
 *
 * BENCH NOTE -- VBAT clock-gate (do NOT poke here): the LPRTC clock is gated
 * from the always-on VBAT domain (VBAT_LPRTC0_CLK_EN, 0x1A609010).  On the
 * bench the gate is already on (the E8 counter regcheck saw counter_start ->
 * -EALREADY and CCVR advancing), so this shim assumes a running counter and
 * does NOT write VBAT.  Enabling that gate, if ever needed, belongs in SoC
 * bring-up with the Alif TRM confirming the bit -- not in this calendar shim.
 * Separate TBD, tracked in the receipt.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rtc.h>
#include <alp/soc_caps.h>

#include "rtc_ops.h"

/* The LPRTC counter that backs the calendar.  Resolved via a dedicated DT alias
 * so the time source is explicit and SoM-overridable: point `alp-lprtc-counter`
 * at the snps,dw-apb-rtc node (lprtc@42000000) in the board overlay.  If the
 * alias is absent the device pointer is NULL and open() returns NOT_READY. */
#define ALP_LPRTC_COUNTER_DEV                                                                      \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(alp_lprtc_counter)),                                       \
	            (DEVICE_DT_GET(DT_ALIAS(alp_lprtc_counter))),                                      \
	            (NULL))

static const struct device *const _lprtc = ALP_LPRTC_COUNTER_DEV;

/* Per-handle shim state: the software epoch base and the matching counter
 * snapshot.  One LPRTC on the E8 (ALP_SOC_RTC_COUNT == 1), so a single static
 * instance suffices -- the dispatcher hands out at most ALP_SOC_RTC_COUNT
 * handles and they all alias the one always-on counter. */
typedef struct {
	int64_t  epoch_base;    /* UNIX seconds at the last set_time. */
	uint32_t tick_snapshot; /* counter_get_value() at the last set_time. */
	uint32_t freq_hz;       /* counter_get_frequency(), cached at open. */
	bool     set;           /* true once set_time has run. */
} lprtc_shim_state_t;

static lprtc_shim_state_t _state;

/*
 * Clean-room civil <-> days-from-epoch conversion (Howard Hinnant's
 * public-domain algorithm, chrono-Compatible-Dates).  Proleptic Gregorian,
 * 1970-01-01 == day 0.  No invented hardware values here -- pure date math.
 */

/* Days from 1970-01-01 to the given y/m/d (m in 1..12, d in 1..31). */
static int64_t _days_from_civil(int64_t y, unsigned m, unsigned d)
{
	y -= (m <= 2);
	const int64_t  era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);                          /* 0..399 */
	const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u; /* 0..365 */
	const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;           /* 0..146096 */
	return era * 146097 + (int64_t)doe - 719468;
}

/* Inverse: decompose days-from-epoch into y/m/d. */
static void _civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
	z += 719468;
	const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = (unsigned)(z - era * 146097);                              /* 0..146096 */
	const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u; /* 0..399 */
	const int64_t  yc  = (int64_t)yoe + era * 400;
	const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); /* 0..365 */
	const unsigned mp  = (5u * doy + 2u) / 153u;                     /* 0..11 */
	*d                 = doy - (153u * mp + 2u) / 5u + 1u;           /* 1..31 */
	*m                 = mp + (mp < 10u ? 3u : -9u);                 /* 1..12 */
	*y                 = (int)(yc + (*m <= 2));
}

/* Weekday for a days-from-epoch value: 1970-01-01 was a Thursday (== 4). */
static uint8_t _weekday_from_days(int64_t z)
{
	return (uint8_t)((unsigned)((z % 7 + 7 + 4) % 7));
}

/* Calendar fields -> UNIX seconds (UTC / wall clock, no timezone). */
static int64_t _calendar_to_unix(const alp_rtc_time_t *t)
{
	const int64_t days = _days_from_civil((int64_t)t->year, t->month, t->day);
	return days * 86400 + (int64_t)t->hour * 3600 + (int64_t)t->minute * 60 + (int64_t)t->second;
}

/* UNIX seconds -> calendar fields (millisecond is set by the caller). */
static void _unix_to_calendar(int64_t unix_s, alp_rtc_time_t *t)
{
	int64_t days = unix_s / 86400;
	int64_t rem  = unix_s % 86400;
	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}
	int      y;
	unsigned m, d;
	_civil_from_days(days, &y, &m, &d);
	t->year    = (uint16_t)y;
	t->month   = (uint8_t)m;
	t->day     = (uint8_t)d;
	t->weekday = _weekday_from_days(days);
	t->hour    = (uint8_t)(rem / 3600);
	t->minute  = (uint8_t)((rem % 3600) / 60);
	t->second  = (uint8_t)(rem % 60);
}

/* Reject out-of-range calendar fields before they reach the epoch math. */
static bool _fields_valid(const alp_rtc_time_t *t)
{
	if (t->month < 1u || t->month > 12u) return false;
	if (t->day < 1u || t->day > 31u) return false;
	if (t->hour > 23u || t->minute > 59u || t->second > 59u) return false;
	if (t->millisecond > 999u) return false;
	return true;
}

static alp_status_t
shim_open(uint32_t rtc_id, alp_rtc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (rtc_id >= ALP_SOC_RTC_COUNT) return ALP_ERR_OUT_OF_RANGE;
	if (_lprtc == NULL || !device_is_ready(_lprtc)) return ALP_ERR_NOT_READY;

	/* The LPRTC lives in the always-on VBAT domain and is normally already
	 * clocked + running (counter_start -> -EALREADY on the bench), so treat
	 * 0 and -EALREADY as success; anything else means the counter would not
	 * advance and the shim cannot keep time. */
	int rc = counter_start(_lprtc);
	if (rc != 0 && rc != -EALREADY) return ALP_ERR_IO;

	_state.freq_hz = counter_get_frequency(_lprtc); /* 32768 Hz on this node */
	if (_state.freq_hz == 0u) return ALP_ERR_NOSUPPORT;

	/* Snapshot now against a compiled-in epoch so get_time is well-defined
	 * before the first set_time (clock reads from the epoch base, monotonic
	 * with the counter -- it just is not wall-accurate until set). */
	_state.epoch_base = 0; /* 1970-01-01T00:00:00Z until set_time runs */
	rc                = counter_get_value(_lprtc, &_state.tick_snapshot);
	if (rc != 0) return ALP_ERR_IO;
	_state.set = false;

	st->dev         = (void *)_lprtc;
	st->rtc_id      = rtc_id;
	st->be_data     = &_state;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t shim_set_time(alp_rtc_backend_state_t *st, const alp_rtc_time_t *t)
{
	lprtc_shim_state_t  *s   = (lprtc_shim_state_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (!_fields_valid(t)) return ALP_ERR_INVAL;

	uint32_t now;
	int      rc = counter_get_value(dev, &now);
	if (rc != 0) return ALP_ERR_IO;

	/* Pin the wall-clock instant to this exact counter snapshot. */
	s->epoch_base    = _calendar_to_unix(t);
	s->tick_snapshot = now;
	s->set           = true;
	return ALP_OK;
}

static alp_status_t shim_get_time(alp_rtc_backend_state_t *st, alp_rtc_time_t *t)
{
	lprtc_shim_state_t  *s   = (lprtc_shim_state_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;

	uint32_t now;
	int      rc = counter_get_value(dev, &now);
	if (rc != 0) return ALP_ERR_IO;

	/* Unsigned 32-bit wrap gives the true elapsed ticks across one CCVR wrap. */
	uint32_t elapsed_ticks = now - s->tick_snapshot;
	int64_t  elapsed_s     = (int64_t)elapsed_ticks / (int64_t)s->freq_hz;
	/* 64-bit intermediate: elapsed_s * freq_hz can exceed UINT32_MAX near the
	 * one-CCVR-wrap boundary (129600 s * 32768 Hz > 4.2e9), so a 32-bit product
	 * would wrap and corrupt rem_ticks. The remainder (< freq_hz) still fits u32. */
	uint32_t rem_ticks = elapsed_ticks - (uint32_t)(elapsed_s * (int64_t)s->freq_hz);

	int64_t unix_s = s->epoch_base + elapsed_s;
	_unix_to_calendar(unix_s, t);
	/* Sub-second from the leftover ticks; 32768 Hz -> ms = ticks*1000/freq. */
	t->millisecond = (uint16_t)(((uint64_t)rem_ticks * 1000u) / s->freq_hz);
	return ALP_OK;
}

static const alp_rtc_ops_t _ops = {
	.open     = shim_open,
	.set_time = shim_set_time,
	.get_time = shim_get_time,
	.close    = NULL,
};

/* Exact silicon_ref "alif:ensemble:e8" wins selection over the wildcard "*"
 * zephyr_drv / sw_fallback backends (tiebreaker rule 2: exact beats wildcard at
 * equal priority), so this shim is the alp_rtc_* backend on the E8 -- where the
 * only RTC peripheral is the counter-class LPRTC and the Zephyr-RTC-class
 * zephyr_drv backend has no alp-rtc alias to bind. */
ALP_BACKEND_REGISTER(rtc,
                     lprtc_calendar_shim,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
