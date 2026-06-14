/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr dac_* driver-class backend.  Used on every SoC
 * the SDK ships unless a vendor-specific backend registers a more
 * specific match.
 *
 * Each studio-resolved channel_id (0..1) maps to the `alp-dacN`
 * devicetree alias on SoMs whose SoC carries a DAC peripheral
 * (Alif Ensemble E3 / E7, etc.).
 *
 * The Zephyr DAC API is write-only; read-back is recovered from the
 * SDK-side `last_mv` cache (set on every write) so apps that want to
 * confirm the programmed setpoint don't have to track it externally.
 *
 * The Zephyr dac_* driver class headers aren't universally present
 * across Zephyr versions, so the DT-alias body only compiles when
 * CONFIG_DAC=y.  When CONFIG_DAC=n the backend still registers (so
 * the class linker section stays non-empty) but open returns
 * NOSUPPORT.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dac.h>
#include <alp/peripheral.h>

#include "dac_ops.h"

#if defined(CONFIG_DAC)
#include <zephyr/drivers/dac.h>

#define ALP_DAC_DEV_OR_NULL(idx)                                                                   \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_dac, idx))),                                   \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_dac, idx)))), (NULL))

static const struct device *const alp_dac_devs[] = {
    ALP_DAC_DEV_OR_NULL(0),
    ALP_DAC_DEV_OR_NULL(1),
};

/* Per-handle setpoint cache.  The Zephyr DAC API is write-only, so
 * read-back returns the last value written through this backend. */
typedef struct {
    uint8_t  channel;
    uint16_t last_mv;
} zephyr_dac_state_t;

#ifndef CONFIG_ALP_SDK_MAX_DAC_HANDLES
#define CONFIG_ALP_SDK_MAX_DAC_HANDLES 2
#endif

static zephyr_dac_state_t  _state_pool[CONFIG_ALP_SDK_MAX_DAC_HANDLES];
static bool                _state_in_use[CONFIG_ALP_SDK_MAX_DAC_HANDLES];

static zephyr_dac_state_t *_alloc_state(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(_state_pool); ++i) {
        if (!_state_in_use[i]) {
            _state_in_use[i] = true;
            _state_pool[i]   = (zephyr_dac_state_t){ 0 };
            return &_state_pool[i];
        }
    }
    return NULL;
}

static void _free_state(zephyr_dac_state_t *s)
{
    for (size_t i = 0; i < ARRAY_SIZE(_state_pool); ++i) {
        if (&_state_pool[i] == s) {
            _state_in_use[i] = false;
            return;
        }
    }
}

static alp_status_t errno_to_alp(int err)
{
    switch (err) {
    case 0:
        return ALP_OK;
    case -EINVAL:
        return ALP_ERR_INVAL;
    case -EBUSY:
        return ALP_ERR_BUSY;
    case -ENOTSUP:
    case -ENOSYS:
        return ALP_ERR_NOSUPPORT;
    default:
        return ALP_ERR_IO;
    }
}

static alp_status_t z_write_mv(alp_dac_backend_state_t *st, uint16_t mv);

static alp_status_t z_open(const alp_dac_config_t *cfg, alp_dac_backend_state_t *st,
                           alp_capabilities_t *caps_out)
{
    if (cfg->channel_id >= ARRAY_SIZE(alp_dac_devs)) {
        return ALP_ERR_INVAL;
    }
    const struct device *dev = alp_dac_devs[cfg->channel_id];
    if (dev == NULL || !device_is_ready(dev)) {
        return ALP_ERR_NOT_READY;
    }

    /* Bring up the channel with the default 12-bit / 3.3 V reference
     * config.  Boards that need a different setup can override via
     * a future cfg->resolution_bits / reference field; today the
     * defaults match every E1M-conformant SoC's documented DAC. */
    const struct dac_channel_cfg dacfg = {
        .channel_id = (uint8_t)cfg->channel_id,
        .resolution = 12u,
        .buffered   = false,
    };
    int err = dac_channel_setup(dev, &dacfg);
    if (err != 0) {
        return errno_to_alp(err);
    }

    zephyr_dac_state_t *bs = _alloc_state();
    if (bs == NULL) {
        return ALP_ERR_NOMEM;
    }
    bs->channel     = (uint8_t)cfg->channel_id;
    bs->last_mv     = 0u;

    st->dev         = (void *)dev;
    st->channel_id  = cfg->channel_id;
    st->be_data     = bs;
    caps_out->flags = 0u;

    /* Apply the initial setpoint so the caller's expected last_mv
     * contract holds on the first read. */
    if (cfg->initial_mv != 0u) {
        alp_status_t s = z_write_mv(st, cfg->initial_mv);
        if (s != ALP_OK) {
            _free_state(bs);
            st->be_data = NULL;
            return s;
        }
    }
    return ALP_OK;
}

static alp_status_t z_write_mv(alp_dac_backend_state_t *st, uint16_t mv)
{
    const struct device *dev = (const struct device *)st->dev;
    zephyr_dac_state_t  *bs  = (zephyr_dac_state_t *)st->be_data;

    /* Convert millivolts to a 12-bit code against the documented
     * 3.3 V reference.  Saturate above 3300 mV (rail-clamped). */
    const uint32_t mv_cap = (mv > 3300u) ? 3300u : (uint32_t)mv;
    const uint32_t code   = (mv_cap * ((1u << 12) - 1u)) / 3300u;
    int            err    = dac_write_value(dev, bs->channel, code);
    if (err != 0) return errno_to_alp(err);
    bs->last_mv = mv;
    return ALP_OK;
}

static alp_status_t z_read_mv(alp_dac_backend_state_t *st, uint16_t *mv_out)
{
    zephyr_dac_state_t *bs = (zephyr_dac_state_t *)st->be_data;
    /* Zephyr's DAC API is write-only -- the SDK reports back the
     * cached setpoint instead of returning NOSUPPORT, on the
     * principle that the rounded hardware value matches the last
     * write to within one LSB and apps that care about exact
     * fidelity can recover the saturated value from the documented
     * 12-bit / 3.3 V transfer function. */
    *mv_out = bs->last_mv;
    return ALP_OK;
}

static void z_close(alp_dac_backend_state_t *st)
{
    if (st->be_data != NULL) {
        _free_state((zephyr_dac_state_t *)st->be_data);
        st->be_data = NULL;
    }
}

static const alp_dac_ops_t _ops = {
    .open     = z_open,
    .write_mv = z_write_mv,
    .read_mv  = z_read_mv,
    .close    = z_close,
};

#else /* !CONFIG_DAC */

static alp_status_t z_open(const alp_dac_config_t *cfg, alp_dac_backend_state_t *st,
                           alp_capabilities_t *caps_out)
{
    (void)cfg;
    (void)st;
    (void)caps_out;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_write_mv(alp_dac_backend_state_t *st, uint16_t mv)
{
    (void)st;
    (void)mv;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_read_mv(alp_dac_backend_state_t *st, uint16_t *mv_out)
{
    (void)st;
    (void)mv_out;
    return ALP_ERR_NOSUPPORT;
}

static const alp_dac_ops_t _ops = {
    .open     = z_open,
    .write_mv = z_write_mv,
    .read_mv  = z_read_mv,
    .close    = NULL,
};

#endif /* CONFIG_DAC */

ALP_BACKEND_REGISTER(dac, zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
