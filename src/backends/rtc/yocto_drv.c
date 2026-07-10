/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto rtc_* driver-class backend.  Binds the alp_rtc
 * dispatcher's ops vtable to the kernel RTC character device
 * (/dev/rtcN) via the RTC_RD_TIME / RTC_SET_TIME ioctls from
 * <linux/rtc.h>.  Registered at priority 100 with vendor "linux";
 * the sw_fallback backend (priority 0) still wins on non-Linux
 * native_sim builds where this TU compiles to an empty object.
 *
 * Selected on any silicon (silicon_ref "*") because the kernel RTC
 * ABI is SoC-agnostic; the device-tree / kernel decides which
 * physical RTC backs /dev/rtcN.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/rtc.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rtc.h>

#include "rtc_ops.h"
#include "common/alp_errno.h"

/* Per-handle backend data: the open RTC chardev fd.  Boxed onto the
 * heap so the void* be_data slot in alp_rtc_backend_state_t owns it. */
typedef struct {
	int fd;
} y_rtc_data_t;

/**
 * @brief Open /dev/rtc<rtc_id> and stash the fd in the handle state.
 *
 * The kernel RTC has no queryable capability surface beyond presence,
 * so caps stay 0.  Errors from open() route through the shared
 * @ref alp_status_from_posix_errno baseline (#630) -- a missing
 * /dev/rtcN (ENOENT/ENODEV) now correctly surfaces as
 * ALP_ERR_NOT_READY instead of falling through to ALP_ERR_IO, which
 * this backend's now-removed local mapping switch didn't cover.
 */
static alp_status_t
y_open(uint32_t rtc_id, alp_rtc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	char path[32];
	int  n = snprintf(path, sizeof(path), "/dev/rtc%u", (unsigned)rtc_id);
	if (n < 0 || (size_t)n >= sizeof(path)) return ALP_ERR_INVAL;

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return alp_status_from_posix_errno(errno);

	y_rtc_data_t *d = (y_rtc_data_t *)malloc(sizeof(*d));
	if (d == NULL) {
		close(fd);
		return ALP_ERR_NOMEM;
	}
	d->fd = fd;

	st->dev         = NULL;
	st->rtc_id      = rtc_id;
	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/**
 * @brief Write the wall-clock via RTC_SET_TIME.
 *
 * struct rtc_time mirrors struct tm: tm_year is years-since-1900 and
 * tm_mon is 0..11, whereas alp_rtc_time_t carries the full year and
 * 1..12 months.  Convert in both directions.  millisecond has no
 * struct rtc_time field (1 s resolution) and is dropped on set.
 */
static alp_status_t y_set_time(alp_rtc_backend_state_t *st, const alp_rtc_time_t *t)
{
	y_rtc_data_t *d = (y_rtc_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;

	struct rtc_time rt;
	memset(&rt, 0, sizeof(rt));
	rt.tm_year = (int)t->year - 1900;
	rt.tm_mon  = (int)t->month - 1;
	rt.tm_mday = (int)t->day;
	rt.tm_wday = (int)t->weekday;
	rt.tm_hour = (int)t->hour;
	rt.tm_min  = (int)t->minute;
	rt.tm_sec  = (int)t->second;
	/* tm_yday / tm_isdst are ignored by the kernel RTC core. */

	if (ioctl(d->fd, RTC_SET_TIME, &rt) < 0) return alp_status_from_posix_errno(errno);
	return ALP_OK;
}

/**
 * @brief Read the wall-clock via RTC_RD_TIME.
 *
 * Inverse field conversion to @ref y_set_time.  The kernel RTC has
 * 1 s resolution, so millisecond is always reported as 0.
 */
static alp_status_t y_get_time(alp_rtc_backend_state_t *st, alp_rtc_time_t *t)
{
	y_rtc_data_t *d = (y_rtc_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;

	struct rtc_time rt;
	memset(&rt, 0, sizeof(rt));
	if (ioctl(d->fd, RTC_RD_TIME, &rt) < 0) return alp_status_from_posix_errno(errno);

	t->year        = (uint16_t)(rt.tm_year + 1900);
	t->month       = (uint8_t)(rt.tm_mon + 1);
	t->day         = (uint8_t)rt.tm_mday;
	t->weekday     = (uint8_t)rt.tm_wday;
	t->hour        = (uint8_t)rt.tm_hour;
	t->minute      = (uint8_t)rt.tm_min;
	t->second      = (uint8_t)rt.tm_sec;
	t->millisecond = 0u;
	return ALP_OK;
}

/** @brief Close the chardev fd and free the per-handle box. */
static void y_close(alp_rtc_backend_state_t *st)
{
	y_rtc_data_t *d = (y_rtc_data_t *)st->be_data;
	if (d != NULL) {
		if (d->fd >= 0) close(d->fd);
		free(d);
		st->be_data = NULL;
	}
}

static const alp_rtc_ops_t _ops = {
	.open     = y_open,
	.set_time = y_set_time,
	.get_time = y_get_time,
	.close    = y_close,
};

ALP_BACKEND_REGISTER(rtc,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
