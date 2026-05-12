/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for the DAC half of <alp/adc.h>.
 *
 * Each studio-resolved channel_id (0..1) maps to the `alp-dacN`
 * devicetree alias on SoMs whose SoC carries a DAC peripheral
 * (Alif Ensemble E3 / E7, etc.).  On V2N
 * (CONFIG_ALP_SOC_RENESAS_RZV2N_N44) the Renesas SoC has no DAC; the
 * carrier routes E1M `DAC0` + `DAC1` to the GD32 IO MCU's PA4 / PA6
 * pads, and the SDK dispatches through the supervisor singleton.
 *
 * The Zephyr DAC API is write-only; read-back is recovered from the
 * SDK-side `last_mv` cache (set on every write) so apps that want to
 * confirm the programmed setpoint don't have to track it externally.
 * The bridge path runs `gd32g553_dac_get` for a true read-back when
 * the firmware supports it.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include "alp/adc.h"
#include "alp/peripheral.h"
#include "handles.h"
#include "v2n_supervisor.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#define ALP_DAC_HAS_BRIDGE_PATH 1
#else
#define ALP_DAC_HAS_BRIDGE_PATH 0
#endif

/* The Zephyr dac_* driver class headers aren't universally present
 * across Zephyr versions (Zephyr 3.7's dac_channel_cfg sits under
 * <zephyr/drivers/dac.h>).  We only compile the DT-alias path when
 * CONFIG_DAC=y; bridge-only V2N builds don't need it. */
#if defined(CONFIG_DAC)
#include <zephyr/drivers/dac.h>

#define ALP_DAC_DEV_OR_NULL(idx)                                               \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_dac, idx))),               \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_dac, idx)))), (NULL))

static const struct device *const alp_dac_devs[] = {
    ALP_DAC_DEV_OR_NULL(0),
    ALP_DAC_DEV_OR_NULL(1),
};
#else
static const struct device *const alp_dac_devs[] = { NULL, NULL };
#endif  /* CONFIG_DAC */

#if defined(CONFIG_DAC)
/* Only compiled when the Zephyr dac_* path is active -- every caller
 * sits inside a `#if defined(CONFIG_DAC)` block, so leaving this
 * defined unconditionally trips -Werror=unused-function on
 * bridge-only V2N builds (CONFIG_DAC=n, ALP_DAC_HAS_BRIDGE_PATH=1). */
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
#endif  /* CONFIG_DAC */

alp_dac_t *alp_dac_open(const alp_dac_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->channel_id >= ARRAY_SIZE(alp_dac_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

#if ALP_DAC_HAS_BRIDGE_PATH
    /* V2N: dispatch through the GD32 supervisor singleton. */
    gd32g553_t *ctx = NULL;
    alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
    if (s != ALP_OK) {
        alp_z_set_last_error(s);
        return NULL;
    }

    struct alp_dac *h = alp_z_dac_pool_acquire();
    if (h == NULL) {
        alp_z_v2n_supervisor_release();
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->channel_id = cfg->channel_id;
    h->dev        = NULL;                              /* bridge sentinel */
    h->channel    = (uint8_t)cfg->channel_id;
    h->last_mv    = cfg->initial_mv;

    s = gd32g553_dac_set(ctx, h->channel, cfg->initial_mv);
    alp_z_v2n_supervisor_release();
    if (s != ALP_OK) {
        alp_z_set_last_error(s);
        alp_z_dac_pool_release(h);
        return NULL;
    }
    return h;
#else
    const struct device *dev = alp_dac_devs[cfg->channel_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

#if defined(CONFIG_DAC)
    /* Bring up the channel with the default 12-bit / 3.3 V reference
     * config.  Boards that need a different setup can override via
     * a future cfg->resolution_bits / reference field; today the
     * defaults match every E1M-conformant SoC's documented DAC. */
    const struct dac_channel_cfg dacfg = {
        .channel_id  = (uint8_t)cfg->channel_id,
        .resolution  = 12u,
        .buffered    = false,
    };
    int err = dac_channel_setup(dev, &dacfg);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        return NULL;
    }
#endif

    struct alp_dac *h = alp_z_dac_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->channel_id = cfg->channel_id;
    h->dev        = dev;
    h->channel    = (uint8_t)cfg->channel_id;
    h->last_mv    = 0u;

    /* Apply the initial setpoint via the public write helper so the
     * caller's expected last_mv contract holds on the first read. */
    if (cfg->initial_mv != 0u) {
        alp_status_t s = alp_dac_write_mv(h, cfg->initial_mv);
        if (s != ALP_OK) {
            alp_z_set_last_error(s);
            alp_z_dac_pool_release(h);
            return NULL;
        }
    }
    return h;
#endif  /* ALP_DAC_HAS_BRIDGE_PATH */
}

alp_status_t alp_dac_write_mv(alp_dac_t *dac, uint16_t mv) {
    if (dac == NULL || !dac->in_use) return ALP_ERR_NOT_READY;

#if ALP_DAC_HAS_BRIDGE_PATH
    if (dac->dev == NULL) {
        gd32g553_t *ctx = NULL;
        alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
        if (s != ALP_OK) return s;
        s = gd32g553_dac_set(ctx, dac->channel, mv);
        alp_z_v2n_supervisor_release();
        if (s == ALP_OK) dac->last_mv = mv;
        return s;
    }
#endif

#if defined(CONFIG_DAC)
    /* Convert millivolts to a 12-bit code against the documented
     * 3.3 V reference.  Saturate above 3300 mV (rail-clamped). */
    const uint32_t mv_cap = (mv > 3300u) ? 3300u : (uint32_t)mv;
    const uint32_t code   = (mv_cap * ((1u << 12) - 1u)) / 3300u;
    int err = dac_write_value(dac->dev, dac->channel, code);
    if (err != 0) return errno_to_alp(err);
    dac->last_mv = mv;
    return ALP_OK;
#else
    (void)mv;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_dac_read_mv(alp_dac_t *dac, uint16_t *mv_out) {
    if (dac == NULL || !dac->in_use) return ALP_ERR_NOT_READY;
    if (mv_out == NULL) return ALP_ERR_INVAL;

#if ALP_DAC_HAS_BRIDGE_PATH
    if (dac->dev == NULL) {
        gd32g553_t *ctx = NULL;
        alp_status_t s = alp_z_v2n_supervisor_acquire(&ctx);
        if (s != ALP_OK) return s;
        s = gd32g553_dac_get(ctx, dac->channel, mv_out);
        alp_z_v2n_supervisor_release();
        if (s == ALP_OK) dac->last_mv = *mv_out;
        return s;
    }
#endif

    /* Zephyr's DAC API is write-only -- the SDK reports back the
     * cached setpoint instead of returning NOSUPPORT, on the
     * principle that the rounded hardware value matches the last
     * write to within one LSB and apps that care about exact
     * fidelity can recover the saturated value from the documented
     * 12-bit / 3.3 V transfer function. */
    *mv_out = dac->last_mv;
    return ALP_OK;
}

void alp_dac_close(alp_dac_t *dac) {
    alp_z_dac_pool_release(dac);
}
