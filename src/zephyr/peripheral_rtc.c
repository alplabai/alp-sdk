/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/rtc.h>.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/util.h>

#include "alp/rtc.h"
#include "alp/soc_caps.h"
#include "handles.h"

#define ALP_RTC_DEV_OR_NULL(idx)                                               \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_rtc, idx))),               \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_rtc, idx)))), (NULL))

static const struct device *const alp_rtc_devs[] = {
    ALP_RTC_DEV_OR_NULL(0),
    ALP_RTC_DEV_OR_NULL(1),
};

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

alp_rtc_t *alp_rtc_open(uint32_t rtc_id) {
    alp_z_clear_last_error();

    if (rtc_id >= ARRAY_SIZE(alp_rtc_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (rtc_id >= ALP_SOC_RTC_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }
    const struct device *dev = alp_rtc_devs[rtc_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct alp_rtc *h = alp_z_rtc_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->rtc_id = rtc_id;
    h->dev    = dev;
    return h;
}

alp_status_t alp_rtc_set_time(alp_rtc_t *rtc, const alp_rtc_time_t *time) {
    if (time == NULL) return ALP_ERR_INVAL;
    if (rtc == NULL || !rtc->in_use) return ALP_ERR_NOT_READY;

    struct rtc_time zt = {
        .tm_year = (int)time->year - 1900,
        .tm_mon  = (int)time->month - 1,
        .tm_mday = (int)time->day,
        .tm_wday = (int)time->weekday,
        .tm_hour = (int)time->hour,
        .tm_min  = (int)time->minute,
        .tm_sec  = (int)time->second,
        .tm_nsec = (int)time->millisecond * 1000000,
    };
    return errno_to_alp(rtc_set_time(rtc->dev, &zt));
}

alp_status_t alp_rtc_get_time(alp_rtc_t *rtc, alp_rtc_time_t *time) {
    if (time == NULL) return ALP_ERR_INVAL;
    if (rtc == NULL || !rtc->in_use) return ALP_ERR_NOT_READY;

    struct rtc_time zt;
    int err = rtc_get_time(rtc->dev, &zt);
    if (err != 0) return errno_to_alp(err);

    time->year        = (uint16_t)(zt.tm_year + 1900);
    time->month       = (uint8_t)(zt.tm_mon + 1);
    time->day         = (uint8_t)zt.tm_mday;
    time->weekday     = (uint8_t)zt.tm_wday;
    time->hour        = (uint8_t)zt.tm_hour;
    time->minute      = (uint8_t)zt.tm_min;
    time->second      = (uint8_t)zt.tm_sec;
    time->millisecond = (uint16_t)(zt.tm_nsec / 1000000);
    return ALP_OK;
}

void alp_rtc_close(alp_rtc_t *rtc) {
    alp_z_rtc_pool_release(rtc);
}
