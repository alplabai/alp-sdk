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
#include "v2n_supervisor.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#define ALP_COUNTER_HAS_BRIDGE_PATH 1
#else
#define ALP_COUNTER_HAS_BRIDGE_PATH 0
#endif

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

#if ALP_COUNTER_HAS_BRIDGE_PATH
    /* V2N: the GD32 IO MCU exposes one free-running counter via
     * CMD_COUNTER_READ.  Only counter_id == 0 is currently routable
     * across the bridge -- higher indices fail at open(). */
    if (cfg->counter_id >= 1u) {
        return NULL;
    }
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) return NULL;
    alp_z_v2n_supervisor_release();

    struct alp_counter *h = alp_z_counter_pool_acquire();
    if (h == NULL) return NULL;
    h->counter_id = cfg->counter_id;
    h->dev        = NULL;                              /* bridge sentinel */
    return h;
#else
    const struct device *dev = alp_counter_devs[cfg->counter_id];
    if (dev == NULL || !device_is_ready(dev)) return NULL;

    struct alp_counter *h = alp_z_counter_pool_acquire();
    if (h == NULL) return NULL;
    h->counter_id = cfg->counter_id;
    h->dev        = dev;
    return h;
#endif  /* ALP_COUNTER_HAS_BRIDGE_PATH */
}

alp_status_t alp_counter_start(alp_counter_t *counter) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
#if ALP_COUNTER_HAS_BRIDGE_PATH
    if (counter->dev == NULL) {
        /* The bridge counter is free-running on the GD32 side -- no
         * explicit start needed; first read returns the current tick. */
        return ALP_OK;
    }
#endif
    return errno_to_alp(counter_start(counter->dev));
}

alp_status_t alp_counter_stop(alp_counter_t *counter) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
#if ALP_COUNTER_HAS_BRIDGE_PATH
    if (counter->dev == NULL) {
        /* No-op: the bridge does not expose a stop opcode (the counter
         * is shared with firmware-internal timekeeping). */
        return ALP_OK;
    }
#endif
    return errno_to_alp(counter_stop(counter->dev));
}

alp_status_t alp_counter_get_value(alp_counter_t *counter, uint32_t *ticks_out) {
    if (ticks_out == NULL) return ALP_ERR_INVAL;
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
#if ALP_COUNTER_HAS_BRIDGE_PATH
    if (counter->dev == NULL) {
        gd32g553_t *ctx = NULL;
        alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
        if (s != ALP_OK) return s;
        s = gd32g553_counter_read(ctx, (uint8_t)counter->counter_id, ticks_out);
        alp_z_v2n_supervisor_release();
        return s;
    }
#endif
    return errno_to_alp(counter_get_value(counter->dev, ticks_out));
}

alp_status_t alp_counter_us_to_ticks(alp_counter_t *counter,
                                     uint32_t us,
                                     uint32_t *ticks_out) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    if (ticks_out == NULL) return ALP_ERR_INVAL;
#if ALP_COUNTER_HAS_BRIDGE_PATH
    if (counter->dev == NULL) {
        /* The v0.2 bridge protocol does not advertise the counter tick
         * frequency; the host cannot translate us -> ticks without it.
         * v0.3 adds CMD_COUNTER_GET_FREQ which will let this return
         * a real conversion. */
        (void)us;
        *ticks_out = 0u;
        return ALP_ERR_NOSUPPORT;
    }
#endif
    *ticks_out = counter_us_to_ticks(counter->dev, us);
    return ALP_OK;
}

alp_status_t alp_counter_set_alarm(alp_counter_t *counter,
                                   uint32_t ticks_from_now,
                                   alp_counter_alarm_cb_t cb,
                                   void *user) {
    if (counter == NULL || !counter->in_use) return ALP_ERR_NOT_READY;
    if (cb == NULL) return ALP_ERR_INVAL;
#if ALP_COUNTER_HAS_BRIDGE_PATH
    if (counter->dev == NULL) {
        /* The GD32 has no interrupt line back to the Renesas host, so
         * deadline callbacks fired in firmware ISR context can't be
         * relayed across the bridge in bounded time.  Apps that need
         * alarms must either use a host-local timer or poll
         * alp_counter_get_value() and synthesise the callback
         * client-side. */
        (void)ticks_from_now; (void)user;
        return ALP_ERR_NOSUPPORT;
    }
#endif

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
#if ALP_COUNTER_HAS_BRIDGE_PATH
    if (counter->dev == NULL) {
        /* No alarms could have been armed (set_alarm returns NOSUPPORT). */
        return ALP_OK;
    }
#endif
    counter->alarm_cb   = NULL;
    counter->alarm_user = NULL;
    return errno_to_alp(counter_cancel_channel_alarm(counter->dev, 0));
}

void alp_counter_close(alp_counter_t *counter) {
    if (counter == NULL || !counter->in_use) return;
#if ALP_COUNTER_HAS_BRIDGE_PATH
    if (counter->dev == NULL) {
        alp_z_counter_pool_release(counter);
        return;
    }
#endif
    (void)counter_stop(counter->dev);
    alp_z_counter_pool_release(counter);
}
