/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software RTC fallback.  Returns a deterministic clock that ticks
 * one second per get_time call from 2026-01-01 00:00:00.  For
 * native_sim build / test only.
 *
 * @par Cost: ROM ~600 B, RAM 16 B per handle (the alp_rtc_time_t cursor).
 * @par Performance: O(1) per call; no system clock access.  Frozen-clock
 *      semantics are intentional -- tests assert exact field values.
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rtc.h>

#include "rtc_ops.h"

static alp_rtc_time_t _cursor = {
	.year        = 2026,
	.month       = 1,
	.day         = 1,
	.weekday     = 4,
	.hour        = 0,
	.minute      = 0,
	.second      = 0,
	.millisecond = 0,
};

static alp_status_t sw_open(uint32_t rtc_id, alp_rtc_backend_state_t *st,
                            alp_capabilities_t *caps_out)
{
	(void)rtc_id;
	st->dev         = NULL;
	st->rtc_id      = rtc_id;
	st->be_data     = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t sw_set_time(alp_rtc_backend_state_t *st, const alp_rtc_time_t *t)
{
	(void)st;
	_cursor = *t;
	return ALP_OK;
}

static alp_status_t sw_get_time(alp_rtc_backend_state_t *st, alp_rtc_time_t *t)
{
	(void)st;
	*t             = _cursor;
	_cursor.second = (uint8_t)((_cursor.second + 1u) % 60u);
	return ALP_OK;
}

static const alp_rtc_ops_t _ops = {
	.open     = sw_open,
	.set_time = sw_set_time,
	.get_time = sw_get_time,
	.close    = NULL,
};

ALP_BACKEND_REGISTER(rtc, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
