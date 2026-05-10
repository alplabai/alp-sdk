/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/counter.h> — free-running counter half.
 * The quadrature-decoder half lives in peripheral_qenc.c.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys/util.h>

#include "alp/counter.h"
#include "handles.h"

#define ALP_COUNTER_DEV_OR_NULL(idx)                                           \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_counter, idx))),           \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_counter, idx)))), (NULL))

static const struct device *const alp_counter_devs[] = {
    ALP_COUNTER_DEV_OR_NULL(0),
    ALP_COUNTER_DEV_OR_NULL(1),
    ALP_COUNTER_DEV_OR_NULL(2),
    ALP_COUNTER_DEV_OR_NULL(3),
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

static void counter_alarm_trampoline(const struct device *dev,
                                     uint8_t chan_id,
                                     uint32_t ticks,
                                     void *user_data) {
    (void)dev; (void)chan_id;
    struct alp_counter *h = (struct alp_counter *)user_data;
    if (h == NULL || h->alarm_cb == NULL) return;
    h->alarm_cb(h, ticks, h->alarm_user);
}

alp_counter_t *alp_counter_open(const alp_counter_config_t *cfg) {
    if (cfg == NULL) return NULL;
    if (cfg->counter_id >= ARRAY_SIZE(alp_counter_devs)) return NULL;

    const struct device *dev = alp_counter_devs[cfg->counter_id];
    if (dev == NULL || !device_is_ready(dev)) return NULL;

    struct alp_counter *h = alp_z_counter_pool_acquire();
    if (h == NULL) return NULL;
    h->counter_id = cfg->counter_id;
    h->dev        = dev;
    return h;
}

alp_status_t alp_counter_start(alp_counter_t *counter) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    return errno_to_alp(counter_start(counter->dev));
}

alp_status_t alp_counter_stop(alp_counter_t *counter) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    return errno_to_alp(counter_stop(counter->dev));
}

alp_status_t alp_counter_get_value(alp_counter_t *counter, uint32_t *ticks_out) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    if (ticks_out == NULL) return ALP_ERR_INVAL;
    return errno_to_alp(counter_get_value(counter->dev, ticks_out));
}

alp_status_t alp_counter_us_to_ticks(alp_counter_t *counter,
                                     uint32_t us,
                                     uint32_t *ticks_out) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    if (ticks_out == NULL) return ALP_ERR_INVAL;
    *ticks_out = counter_us_to_ticks(counter->dev, us);
    return ALP_OK;
}

alp_status_t alp_counter_set_alarm(alp_counter_t *counter,
                                   uint32_t ticks_from_now,
                                   alp_counter_alarm_cb_t cb,
                                   void *user) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    if (cb == NULL) return ALP_ERR_INVAL;

    counter->alarm_cb   = cb;
    counter->alarm_user = user;

    struct counter_alarm_cfg acfg = {
        .callback  = counter_alarm_trampoline,
        .ticks     = ticks_from_now,
        .user_data = counter,
        .flags     = 0,
    };
    return errno_to_alp(counter_set_channel_alarm(counter->dev, 0, &acfg));
}

alp_status_t alp_counter_cancel_alarm(alp_counter_t *counter) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    counter->alarm_cb   = NULL;
    counter->alarm_user = NULL;
    return errno_to_alp(counter_cancel_channel_alarm(counter->dev, 0));
}

void alp_counter_close(alp_counter_t *counter) {
    if (counter == NULL || !counter->in_use) return;
    (void)counter_stop(counter->dev);
    alp_z_counter_pool_release(counter);
}
