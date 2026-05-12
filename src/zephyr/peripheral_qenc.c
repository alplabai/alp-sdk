/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/counter.h> — quadrature-decoder half.
 *
 * Zephyr surfaces QDEC-class peripherals through the sensor API
 * (`SENSOR_CHAN_ROTATION`).  Each studio-resolved encoder_id (0..3)
 * maps to the `alp-qencN` DT alias, which must point at a node with
 * `compatible = "...,qdec"` (e.g. `nordic,nrf-qdec`,
 * `st,stm32-qdec`, `silabs,gecko-qdec`).
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/util.h>

#include "alp/counter.h"
#include "handles.h"
#include "v2n_supervisor.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#define ALP_QENC_HAS_BRIDGE_PATH 1
#else
#define ALP_QENC_HAS_BRIDGE_PATH 0
#endif

#define ALP_QENC_DEV_OR_NULL(idx)                                              \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_qenc, idx))),              \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_qenc, idx)))), (NULL))

static const struct device *const alp_qenc_devs[] = {
    ALP_QENC_DEV_OR_NULL(0),
    ALP_QENC_DEV_OR_NULL(1),
    ALP_QENC_DEV_OR_NULL(2),
    ALP_QENC_DEV_OR_NULL(3),
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

alp_qenc_t *alp_qenc_open(const alp_qenc_config_t *cfg) {
    if (cfg == NULL) return NULL;
    if (cfg->encoder_id >= ARRAY_SIZE(alp_qenc_devs)) return NULL;

#if ALP_QENC_HAS_BRIDGE_PATH
    /* V2N: every E1M encoder rides the GD32 IO MCU bridge.  Probe the
     * supervisor up-front so open() surfaces a bus-open failure
     * instead of deferring it to the first read. */
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) return NULL;
    alp_z_v2n_supervisor_release();

    struct alp_qenc *h = alp_z_qenc_pool_acquire();
    if (h == NULL) return NULL;
    h->encoder_id    = cfg->encoder_id;
    h->dev           = NULL;                              /* bridge sentinel */
    h->last_position = 0;
    return h;
#else
    const struct device *dev = alp_qenc_devs[cfg->encoder_id];
    if (dev == NULL || !device_is_ready(dev)) return NULL;

    struct alp_qenc *h = alp_z_qenc_pool_acquire();
    if (h == NULL) return NULL;
    h->encoder_id    = cfg->encoder_id;
    h->dev           = dev;
    h->last_position = 0;
    return h;
#endif  /* ALP_QENC_HAS_BRIDGE_PATH */
}

alp_status_t alp_qenc_get_position(alp_qenc_t *enc, int32_t *pos_out) {
    if (enc == NULL || !enc->in_use) return ALP_ERR_NOT_READY;
    if (pos_out == NULL) return ALP_ERR_INVAL;

#if ALP_QENC_HAS_BRIDGE_PATH
    if (enc->dev == NULL) {
        gd32g553_t *ctx = NULL;
        alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
        if (s != ALP_OK) return s;
        s = gd32g553_qenc_read(ctx, (uint8_t)enc->encoder_id, pos_out);
        alp_z_v2n_supervisor_release();
        if (s == ALP_OK) enc->last_position = *pos_out;
        return s;
    }
#endif

    int err = sensor_sample_fetch(enc->dev);
    if (err != 0) return errno_to_alp(err);

    struct sensor_value v;
    err = sensor_channel_get(enc->dev, SENSOR_CHAN_ROTATION, &v);
    if (err != 0) return errno_to_alp(err);

    /* Sensor value is degrees in val1 + microdegrees in val2; we
     * accumulate raw integer degrees as a position proxy.  Real
     * pulse counts come via the v0.3 input-subsystem fast-path. */
    enc->last_position += v.val1;
    *pos_out = enc->last_position;
    return ALP_OK;
}

alp_status_t alp_qenc_reset_position(alp_qenc_t *enc) {
    if (enc == NULL || !enc->in_use) return ALP_ERR_NOT_READY;
#if ALP_QENC_HAS_BRIDGE_PATH
    if (enc->dev == NULL) {
        gd32g553_t *ctx = NULL;
        alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
        if (s != ALP_OK) return s;
        s = gd32g553_qenc_reset(ctx, (uint8_t)enc->encoder_id);
        alp_z_v2n_supervisor_release();
        if (s == ALP_OK) enc->last_position = 0;
        return s;
    }
#endif
    enc->last_position = 0;
    return ALP_OK;
}

void alp_qenc_close(alp_qenc_t *enc) {
    alp_z_qenc_pool_release(enc);
}
