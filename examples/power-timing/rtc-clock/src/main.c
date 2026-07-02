/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rtc-clock — set the wall-clock, sleep, read it back to verify
 * the RTC ticks while the CPU is otherwise occupied.
 *
 * Most apps that need wall-clock time interact with the SoC's RTC
 * exactly this way: set once at boot from a network time source
 * (NTP / GNSS / cellular) and read on demand thereafter.  The
 * battery-backed RTC keeps ticking across resets and (on most
 * SoCs) deep-sleep entries.
 */

#include <stdio.h>

#include "alp/peripheral.h"

#include "alp/rtc.h"

int main(void)
{
	printf("[rtc] open instance 0\n");

	/* Most SoMs expose exactly one RTC; pass 0.  Multi-RTC SoCs
     * (e.g. RZ/V2N has the SoC RTC + a battery-backed PMIC RTC)
     * map to higher ids via the alp-rtcN devicetree alias. */
	alp_rtc_t *rtc = alp_rtc_open(0);
	if (rtc == NULL) {
		printf("[rtc] open failed: alp_last_error=%d "
		       "(expected NOT_READY = -2 on native_sim)\n",
		       (int)alp_last_error());
		printf("[rtc] done\n");
		return 0;
	}

	/* Calendar fields are populated as you'd write them on paper:
     * full year (2026, not 26 / not since-1900), 1-indexed month
     * and day, 24-hour hour, etc.  The driver translates internally
     * into whatever encoding the hardware uses (BCD on most SoCs,
     * Unix-seconds on Linux backends). */
	alp_rtc_time_t set_t = {
		.year   = 2026,
		.month  = 5,
		.day    = 10,
		.hour   = 14,
		.minute = 30,
		.second = 0,
		/* .weekday and .millisecond are optional; 0 means "unknown"
         * for weekday and "1 s resolution" for millisecond. */
	};
	alp_status_t s = alp_rtc_set_time(rtc, &set_t);
	printf("[rtc] set_time(2026-05-10 14:30:00) -> %d\n", (int)s);

	/* Sleep for 250 ms so a real RTC has time to tick at least once
     * before we read back.  On native_sim the readback returns 0s
     * regardless because the host RTC isn't running. */
	alp_delay_ms(250);

	/* Read the current wall-clock.  The got struct is populated only
     * when status == ALP_OK; treat fields as unspecified otherwise. */
	alp_rtc_time_t got;
	s = alp_rtc_get_time(rtc, &got);
	printf("[rtc] get_time -> status=%d, %04u-%02u-%02u %02u:%02u:%02u\n",
	       (int)s,
	       got.year,
	       got.month,
	       got.day,
	       got.hour,
	       got.minute,
	       got.second);

	/* Close releases the handle but doesn't stop the RTC -- it
     * keeps ticking in hardware.  If you need to actually disable
     * the peripheral, use the SoC's PM API after close. */
	alp_rtc_close(rtc);
	printf("[rtc] done\n");
	return 0;
}
