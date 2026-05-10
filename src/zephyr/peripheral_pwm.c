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
 *
 * Conditional spec construction.  PWM_DT_SPEC_GET fails to expand
 * when given DT_INVALID_NODE (it pulls in the `pwms` property
 * unconditionally), so we can't COND_CODE_1 on alias existence.
 * Per-index #if blocks emit either PWM_DT_SPEC_GET or a NULL spec,
 * and the array is built from those.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/util.h>

#include "alp/pwm.h"
#include "alp/soc_caps.h"
#include "handles.h"

#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm0))
#define ALP_PWM_SPEC_0_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm0))
#else
#define ALP_PWM_SPEC_0_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm1))
#define ALP_PWM_SPEC_1_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm1))
#else
#define ALP_PWM_SPEC_1_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm2))
#define ALP_PWM_SPEC_2_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm2))
#else
#define ALP_PWM_SPEC_2_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm3))
#define ALP_PWM_SPEC_3_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm3))
#else
#define ALP_PWM_SPEC_3_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm4))
#define ALP_PWM_SPEC_4_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm4))
#else
#define ALP_PWM_SPEC_4_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm5))
#define ALP_PWM_SPEC_5_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm5))
#else
#define ALP_PWM_SPEC_5_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm6))
#define ALP_PWM_SPEC_6_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm6))
#else
#define ALP_PWM_SPEC_6_INIT  {.dev = NULL}
#endif
#if DT_NODE_EXISTS(DT_ALIAS(alp_pwm7))
#define ALP_PWM_SPEC_7_INIT  PWM_DT_SPEC_GET(DT_ALIAS(alp_pwm7))
#else
#define ALP_PWM_SPEC_7_INIT  {.dev = NULL}
#endif

static const struct pwm_dt_spec alp_pwms[] = {
    ALP_PWM_SPEC_0_INIT,
    ALP_PWM_SPEC_1_INIT,
    ALP_PWM_SPEC_2_INIT,
    ALP_PWM_SPEC_3_INIT,
    ALP_PWM_SPEC_4_INIT,
    ALP_PWM_SPEC_5_INIT,
    ALP_PWM_SPEC_6_INIT,
    ALP_PWM_SPEC_7_INIT,
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
