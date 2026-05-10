/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/wdt.h>.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/util.h>

#include "alp/wdt.h"
#include "alp/soc_caps.h"
#include "handles.h"

#define ALP_WDT_DEV_OR_NULL(idx)                                               \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_wdt, idx))),               \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_wdt, idx)))), (NULL))

static const struct device *const alp_wdt_devs[] = {
    ALP_WDT_DEV_OR_NULL(0),
    ALP_WDT_DEV_OR_NULL(1),
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

alp_wdt_t *alp_wdt_open(uint32_t wdt_id, const alp_wdt_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->timeout_ms == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (wdt_id >= ARRAY_SIZE(alp_wdt_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (wdt_id >= ALP_SOC_WDT_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct device *dev = alp_wdt_devs[wdt_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct alp_wdt *h = alp_z_wdt_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->wdt_id = wdt_id;
    h->dev    = dev;
    h->cfg    = *cfg;

    struct wdt_timeout_cfg zcfg = {
        .window  = { .min = 0, .max = cfg->timeout_ms },
        .callback = NULL,
        .flags = (cfg->on_timeout == ALP_WDT_INTERRUPT_ONLY)
                  ? WDT_FLAG_RESET_NONE
                  : (cfg->on_timeout == ALP_WDT_RESET_CPU
                       ? WDT_FLAG_RESET_CPU_CORE
                       : WDT_FLAG_RESET_SOC),
    };
    int channel_id = wdt_install_timeout(dev, &zcfg);
    if (channel_id < 0) {
        alp_z_wdt_pool_release(h);
        return NULL;
    }
    h->channel_id = channel_id;

    int err = wdt_setup(dev, 0);
    if (err != 0) {
        alp_z_wdt_pool_release(h);
        return NULL;
    }
    return h;
}

alp_status_t alp_wdt_feed(alp_wdt_t *wdt) {
    if (wdt == NULL || !wdt->in_use) return ALP_ERR_NOT_READY;
    return errno_to_alp(wdt_feed(wdt->dev, wdt->channel_id));
}

alp_status_t alp_wdt_disable(alp_wdt_t *wdt) {
    if (wdt == NULL || !wdt->in_use) return ALP_ERR_NOT_READY;
    return errno_to_alp(wdt_disable(wdt->dev));
}

void alp_wdt_close(alp_wdt_t *wdt) {
    if (wdt == NULL || !wdt->in_use) return;
    /* Most M-class watchdogs don't allow disable; ignore error. */
    (void)wdt_disable(wdt->dev);
    alp_z_wdt_pool_release(wdt);
}
