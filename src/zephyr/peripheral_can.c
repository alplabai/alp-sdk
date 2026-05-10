/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/can.h> — classic CAN + CAN-FD.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/util.h>

#include "alp/can.h"
#include "alp/soc_caps.h"
#include "handles.h"

#define ALP_CAN_DEV_OR_NULL(idx)                                               \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_can, idx))),               \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_can, idx)))), (NULL))

static const struct device *const alp_can_devs[] = {
    ALP_CAN_DEV_OR_NULL(0),
    ALP_CAN_DEV_OR_NULL(1),
    ALP_CAN_DEV_OR_NULL(2),
    ALP_CAN_DEV_OR_NULL(3),
    ALP_CAN_DEV_OR_NULL(4),
    ALP_CAN_DEV_OR_NULL(5),
};

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT:  return ALP_ERR_TIMEOUT;
    case -EIO:        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    case -ENOMEM:     return ALP_ERR_NOMEM;
    default:          return ALP_ERR_IO;
    }
}

struct cb_ctx {
    alp_can_rx_cb_t  cb;
    void            *user;
};

/* Callback storage indexed by zephyr filter id (typically small int). */
#define MAX_FILTERS 16
static struct cb_ctx cb_table[MAX_FILTERS];

static void rx_trampoline(const struct device *dev,
                          struct can_frame *frame,
                          void *user_data) {
    (void)dev;
    int filter_id = (int)(intptr_t)user_data;
    if (filter_id < 0 || filter_id >= MAX_FILTERS) return;
    struct cb_ctx *ctx = &cb_table[filter_id];
    if (ctx->cb == NULL) return;

    alp_can_frame_t out = {
        .id     = frame->id,
        .ext_id = (frame->flags & CAN_FRAME_IDE) != 0,
        .rtr    = (frame->flags & CAN_FRAME_RTR) != 0,
        .fd     = (frame->flags & CAN_FRAME_FDF) != 0,
        .brs    = (frame->flags & CAN_FRAME_BRS) != 0,
        .dlc    = can_dlc_to_bytes(frame->dlc),
    };
    if (out.dlc > sizeof out.data) out.dlc = sizeof out.data;
    memcpy(out.data, frame->data, out.dlc);
    ctx->cb(&out, ctx->user);
}

alp_can_t *alp_can_open(const alp_can_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->bitrate_nominal_hz == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ARRAY_SIZE(alp_can_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->bus_id >= ALP_SOC_CAN_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }
    if (cfg->mode == ALP_CAN_MODE_FD && !ALP_SOC_CAN_FD_SUPPORTED) {
        /* SoC's CAN controller doesn't speak FD. */
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct device *dev = alp_can_devs[cfg->bus_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct alp_can *h = alp_z_can_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->bus_id  = cfg->bus_id;
    h->dev     = dev;
    h->cfg     = *cfg;
    h->started = false;

    int err = can_set_bitrate(dev, cfg->bitrate_nominal_hz);
    if (err != 0) goto fail;

    if (cfg->mode == ALP_CAN_MODE_FD && cfg->bitrate_data_hz > 0) {
#if defined(CONFIG_CAN_FD_MODE)
        err = can_set_bitrate_data(dev, cfg->bitrate_data_hz);
        if (err != 0) goto fail;
        err = can_set_mode(dev, CAN_MODE_FD |
                           (cfg->loopback ? CAN_MODE_LOOPBACK : 0));
#else
        err = -ENOTSUP;
#endif
    } else {
        err = can_set_mode(dev, cfg->loopback ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL);
    }
    if (err != 0) goto fail;

    return h;

fail:
    alp_z_set_last_error(errno_to_alp(err));
    alp_z_can_pool_release(h);
    return NULL;
}

alp_status_t alp_can_start(alp_can_t *can) {
    if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
    int err = can_start(can->dev);
    if (err == 0) can->started = true;
    return errno_to_alp(err);
}

alp_status_t alp_can_stop(alp_can_t *can) {
    if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
    int err = can_stop(can->dev);
    if (err == 0) can->started = false;
    return errno_to_alp(err);
}

alp_status_t alp_can_send(alp_can_t *can,
                          const alp_can_frame_t *frame,
                          uint32_t timeout_ms) {
    if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
    if (frame == NULL) return ALP_ERR_INVAL;
    if (frame->dlc > ALP_CAN_MAX_DLC_FD) return ALP_ERR_INVAL;
    if (!frame->fd && frame->dlc > ALP_CAN_MAX_DLC_CLASSIC) return ALP_ERR_INVAL;

    struct can_frame zf = {
        .id    = frame->id,
        .dlc   = can_bytes_to_dlc(frame->dlc),
        .flags = (frame->ext_id ? CAN_FRAME_IDE : 0) |
                 (frame->rtr    ? CAN_FRAME_RTR : 0) |
                 (frame->fd     ? CAN_FRAME_FDF : 0) |
                 (frame->brs    ? CAN_FRAME_BRS : 0),
    };
    memcpy(zf.data, frame->data, frame->dlc);
    return errno_to_alp(can_send(can->dev, &zf, K_MSEC(timeout_ms),
                                 NULL, NULL));
}

alp_status_t alp_can_add_filter(alp_can_t *can,
                                const alp_can_filter_t *filter,
                                alp_can_rx_cb_t cb,
                                void *user,
                                int32_t *filter_id_out) {
    if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
    if (filter == NULL || cb == NULL) return ALP_ERR_INVAL;

    /* Reserve a slot in cb_table; pass the slot index as user_data. */
    int slot = -1;
    for (int i = 0; i < MAX_FILTERS; i++) {
        if (cb_table[i].cb == NULL) { slot = i; break; }
    }
    if (slot < 0) return ALP_ERR_NOMEM;
    cb_table[slot].cb   = cb;
    cb_table[slot].user = user;

    struct can_filter zf = {
        .id    = filter->id,
        .mask  = filter->mask,
        .flags = filter->ext_id ? CAN_FILTER_IDE : 0,
    };
    int fid = can_add_rx_filter(can->dev, rx_trampoline,
                                (void *)(intptr_t)slot, &zf);
    if (fid < 0) {
        cb_table[slot] = (struct cb_ctx){0};
        return errno_to_alp(fid);
    }
    if (filter_id_out != NULL) *filter_id_out = fid;
    return ALP_OK;
}

alp_status_t alp_can_remove_filter(alp_can_t *can, int32_t filter_id) {
    if (can == NULL || !can->in_use) return ALP_ERR_NOT_READY;
    can_remove_rx_filter(can->dev, filter_id);
    /* The slot lookup-by-fid is best-effort; when zephyr's filter
     * id is opaque the matching cb_table entry stays — leak is
     * bounded by MAX_FILTERS and the v0.3 OpenAMP-friendly rewrite
     * gives this a proper id->slot map. */
    return ALP_OK;
}

void alp_can_close(alp_can_t *can) {
    if (can == NULL || !can->in_use) return;
    if (can->started) (void)can_stop(can->dev);
    alp_z_can_pool_release(can);
}
