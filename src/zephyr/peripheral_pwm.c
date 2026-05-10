/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/pwm.h>.
 *
 * Each studio-resolved channel_id (0..7) maps to the `alp-pwmN` DT
 * alias.  The alias must point at a node that has a `pwms` phandle
 * property (the canonical Zephyr "pwm-leds" pattern):
 *
 *     pwm_user0: pwm_user_0 {
 *         pwms = <&pwm0 0 PWM_USEC(1000) PWM_POLARITY_NORMAL>;
 *     };
 *     aliases { alp-pwm0 = &pwm_user0; };
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/util.h>

#include "alp/pwm.h"
#include "alp/soc_caps.h"
#include "handles.h"

#define ALP_PWM_SPEC_OR_NULL(idx)                                              \
    COND_CODE_1(DT_NODE_HAS_PROP(DT_ALIAS(_CONCAT(alp_pwm, idx)), pwms),       \
                (PWM_DT_SPEC_GET(DT_ALIAS(_CONCAT(alp_pwm, idx)))),            \
                ((struct pwm_dt_spec){.dev = NULL}))

static const struct pwm_dt_spec alp_pwms[] = {
    ALP_PWM_SPEC_OR_NULL(0),
    ALP_PWM_SPEC_OR_NULL(1),
    ALP_PWM_SPEC_OR_NULL(2),
    ALP_PWM_SPEC_OR_NULL(3),
    ALP_PWM_SPEC_OR_NULL(4),
    ALP_PWM_SPEC_OR_NULL(5),
    ALP_PWM_SPEC_OR_NULL(6),
    ALP_PWM_SPEC_OR_NULL(7),
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

alp_pwm_t *alp_pwm_open(const alp_pwm_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->channel_id >= ARRAY_SIZE(alp_pwms)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->channel_id >= ALP_SOC_PWM_COUNT) {
        /* Active SoC's PWM bank doesn't reach this channel. */
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct pwm_dt_spec *spec = &alp_pwms[cfg->channel_id];
    if (spec->dev == NULL || !device_is_ready(spec->dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct alp_pwm *h = alp_z_pwm_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    h->channel_id = cfg->channel_id;
    h->dev        = spec->dev;
    h->channel    = spec->channel;
    h->period_ns  = (cfg->period_ns != 0) ? cfg->period_ns : spec->period;
    h->flags      = (cfg->polarity == ALP_PWM_POLARITY_INVERTED)
                      ? PWM_POLARITY_INVERTED
                      : PWM_POLARITY_NORMAL;

    /* Initialise to 0 % duty (off). */
    int err = pwm_set(h->dev, h->channel, h->period_ns, 0u, h->flags);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        alp_z_pwm_pool_release(h);
        return NULL;
    }
    return h;
}

alp_status_t alp_pwm_set_duty(alp_pwm_t *pwm, uint32_t pulse_ns) {
    if (pwm == NULL || !pwm->in_use) return ALP_ERR_NOT_READY;
    if (pulse_ns > pwm->period_ns) return ALP_ERR_INVAL;
    return errno_to_alp(pwm_set(pwm->dev, pwm->channel,
                                pwm->period_ns, pulse_ns, pwm->flags));
}

alp_status_t alp_pwm_set_period(alp_pwm_t *pwm, uint32_t period_ns) {
    if (pwm == NULL || !pwm->in_use) return ALP_ERR_NOT_READY;
    if (period_ns == 0) return ALP_ERR_INVAL;
    pwm->period_ns = period_ns;
    return errno_to_alp(pwm_set(pwm->dev, pwm->channel,
                                period_ns, 0u, pwm->flags));
}

void alp_pwm_close(alp_pwm_t *pwm) {
    if (pwm == NULL || !pwm->in_use) return;
    /* Best-effort: drive output low before releasing. */
    (void)pwm_set(pwm->dev, pwm->channel, pwm->period_ns, 0u, pwm->flags);
    alp_z_pwm_pool_release(pwm);
}
