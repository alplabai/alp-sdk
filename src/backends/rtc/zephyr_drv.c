/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr rtc_* driver-class backend.  Used on every SoC
 * the SDK ships unless a vendor-specific backend registers a more
 * specific match.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rtc.h>
#include <alp/soc_caps.h>

#include "rtc_ops.h"

#define ALP_RTC_DEV_OR_NULL(idx) \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_rtc, idx))), \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_rtc, idx)))), (NULL))

static const struct device *const _devs[] = {
    ALP_RTC_DEV_OR_NULL(0),
    ALP_RTC_DEV_OR_NULL(1),
};

static alp_status_t _errno_to_alp(int err) {
    switch (err) {
    case 0:        return ALP_OK;
    case -EINVAL:  return ALP_ERR_INVAL;
    case -EBUSY:   return ALP_ERR_BUSY;
    case -ENOTSUP:
    case -ENOSYS:  return ALP_ERR_NOSUPPORT;
    default:       return ALP_ERR_IO;
    }
}

static alp_status_t z_open(uint32_t rtc_id,
                           alp_rtc_backend_state_t *st,
                           alp_capabilities_t *caps_out) {
    if (rtc_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
    if (rtc_id >= ALP_SOC_RTC_COUNT) return ALP_ERR_OUT_OF_RANGE;
    const struct device *dev = _devs[rtc_id];
    if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
    st->dev = (void *)dev;
    st->rtc_id = rtc_id;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t z_set_time(alp_rtc_backend_state_t *st,
                               const alp_rtc_time_t *t) {
    const struct device *dev = (const struct device *)st->dev;
    struct rtc_time zt = {
        .tm_year = (int)t->year - 1900,
        .tm_mon  = (int)t->month - 1,
        .tm_mday = (int)t->day,
        .tm_wday = (int)t->weekday,
        .tm_hour = (int)t->hour,
        .tm_min  = (int)t->minute,
        .tm_sec  = (int)t->second,
        .tm_nsec = (int)t->millisecond * 1000000,
    };
    return _errno_to_alp(rtc_set_time(dev, &zt));
}

static alp_status_t z_get_time(alp_rtc_backend_state_t *st,
                               alp_rtc_time_t *t) {
    const struct device *dev = (const struct device *)st->dev;
    struct rtc_time zt;
    int err = rtc_get_time(dev, &zt);
    if (err != 0) return _errno_to_alp(err);
    t->year        = (uint16_t)(zt.tm_year + 1900);
    t->month       = (uint8_t)(zt.tm_mon + 1);
    t->day         = (uint8_t)zt.tm_mday;
    t->weekday     = (uint8_t)zt.tm_wday;
    t->hour        = (uint8_t)zt.tm_hour;
    t->minute      = (uint8_t)zt.tm_min;
    t->second      = (uint8_t)zt.tm_sec;
    t->millisecond = (uint16_t)(zt.tm_nsec / 1000000);
    return ALP_OK;
}

static const alp_rtc_ops_t _ops = {
    .open     = z_open,
    .set_time = z_set_time,
    .get_time = z_get_time,
    .close    = NULL,
};

ALP_BACKEND_REGISTER(rtc, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
