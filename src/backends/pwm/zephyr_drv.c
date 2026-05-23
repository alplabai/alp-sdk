/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr pwm_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/pwm_dispatch.c; the
 * backend's open resolves the alp-pwmN DT alias, primes the
 * channel at 0 % duty, and stashes channel + flags on the portable
 * handle so subsequent set_duty / set_period calls reach pwm_set
 * without re-querying the spec.
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
 *
 * configure / single_pulse / capture_* return ALP_ERR_NOSUPPORT --
 * Zephyr's portable pwm_* driver class doesn't expose dead-time,
 * center-aligned counters, single-pulse primitives, or input
 * capture in a vendor-neutral way.  V2N's bridge backend (separate
 * slice) honours those entries via the GD32 IO MCU.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/pwm.h>
#include <alp/soc_caps.h>

#include "pwm_ops.h"

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

static const struct pwm_dt_spec _specs[] = {
    ALP_PWM_SPEC_0_INIT,
    ALP_PWM_SPEC_1_INIT,
    ALP_PWM_SPEC_2_INIT,
    ALP_PWM_SPEC_3_INIT,
    ALP_PWM_SPEC_4_INIT,
    ALP_PWM_SPEC_5_INIT,
    ALP_PWM_SPEC_6_INIT,
    ALP_PWM_SPEC_7_INIT,
};

static alp_status_t _errno_to_alp(int err) {
    switch (err) {
    case 0:           return ALP_OK;
    case -EINVAL:     return ALP_ERR_INVAL;
    case -EBUSY:      return ALP_ERR_BUSY;
    case -ENOTSUP:
    case -ENOSYS:     return ALP_ERR_NOSUPPORT;
    default:          return ALP_ERR_IO;
    }
}

static alp_status_t z_open(const alp_pwm_config_t *cfg,
                           alp_pwm_backend_state_t *st,
                           alp_capabilities_t *caps_out) {
    if (cfg->channel_id >= ARRAY_SIZE(_specs)) return ALP_ERR_INVAL;
    if (cfg->channel_id >= ALP_SOC_PWM_COUNT)  return ALP_ERR_OUT_OF_RANGE;

    const struct pwm_dt_spec *spec = &_specs[cfg->channel_id];
    if (spec->dev == NULL || !device_is_ready(spec->dev)) {
        return ALP_ERR_NOT_READY;
    }

    /* Recover the full handle via container_of so we can populate the
     * channel / period_ns / flags fields that set_duty / set_period
     * will need. */
    struct alp_pwm *h = CONTAINER_OF(st, struct alp_pwm, state);
    h->channel   = spec->channel;
    h->period_ns = (cfg->period_ns != 0u) ? cfg->period_ns : spec->period;
    h->flags     = (cfg->polarity == ALP_PWM_POLARITY_INVERTED)
                       ? PWM_POLARITY_INVERTED
                       : PWM_POLARITY_NORMAL;

    /* Initialise to 0 % duty (off). */
    int err = pwm_set(spec->dev, h->channel, h->period_ns, 0u, h->flags);
    if (err != 0) return _errno_to_alp(err);

    st->dev        = (void *)spec->dev;
    st->channel_id = cfg->channel_id;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t z_set_duty(alp_pwm_backend_state_t *st, uint32_t pulse_ns) {
    struct alp_pwm *h = CONTAINER_OF(st, struct alp_pwm, state);
    const struct device *dev = (const struct device *)st->dev;
    return _errno_to_alp(pwm_set(dev, h->channel,
                                 h->period_ns, pulse_ns, h->flags));
}

static alp_status_t z_set_period(alp_pwm_backend_state_t *st, uint32_t period_ns) {
    struct alp_pwm *h = CONTAINER_OF(st, struct alp_pwm, state);
    const struct device *dev = (const struct device *)st->dev;
    /* Dispatcher caches period_ns on ALP_OK return; reset duty to 0
     * (mirrors legacy peripheral_pwm.c behaviour and the documented
     * @ref alp_pwm_set_period contract). */
    return _errno_to_alp(pwm_set(dev, h->channel,
                                 period_ns, 0u, h->flags));
}

static alp_status_t z_configure(alp_pwm_backend_state_t *st,
                                alp_pwm_align_t align_mode,
                                uint32_t dead_time_ns,
                                uint8_t break_cfg) {
    /* Zephyr's portable pwm_* driver class doesn't expose dead-time or
     * center-aligned counters in a vendor-neutral way (the STM32
     * driver has the closest extension, but it isn't generic), so the
     * portable surface refuses the call rather than silently accepting
     * it. */
    (void)st; (void)align_mode; (void)dead_time_ns; (void)break_cfg;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_single_pulse(alp_pwm_backend_state_t *st,
                                   uint32_t pulse_ns) {
    /* Portable single-pulse output is bridge-only today (V2N GD32
     * firmware CMD_PWM_SINGLE_PULSE handler).  The wave-2 GD32 HAL
     * backend will register a more specific silicon_ref and override
     * this entry. */
    (void)st; (void)pulse_ns;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_capture_open(const alp_pwm_capture_config_t *cfg,
                                   alp_pwm_backend_state_t *st,
                                   alp_capabilities_t *caps_out) {
    /* Input capture is bridge-only today (V2N GD32 firmware
     * CMD_PWM_CAPTURE_BEGIN handler).  The wave-2 GD32 HAL backend
     * will register a more specific silicon_ref and override this
     * entry. */
    (void)cfg; (void)st; (void)caps_out;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_capture_read(alp_pwm_backend_state_t *st,
                                   uint32_t *period_ns_out,
                                   uint32_t *pulse_ns_out) {
    (void)st;
    if (period_ns_out != NULL) *period_ns_out = 0u;
    if (pulse_ns_out  != NULL) *pulse_ns_out  = 0u;
    return ALP_ERR_NOSUPPORT;
}

static void z_capture_close(alp_pwm_backend_state_t *st) { (void)st; }

static void z_close(alp_pwm_backend_state_t *st) {
    struct alp_pwm *h = CONTAINER_OF(st, struct alp_pwm, state);
    const struct device *dev = (const struct device *)st->dev;
    if (dev != NULL) {
        /* Best-effort: drive output low before releasing. */
        (void)pwm_set(dev, h->channel, h->period_ns, 0u, h->flags);
    }
}

static const alp_pwm_ops_t _ops = {
    .open          = z_open,
    .set_duty      = z_set_duty,
    .set_period    = z_set_period,
    .configure     = z_configure,
    .single_pulse  = z_single_pulse,
    .capture_open  = z_capture_open,
    .capture_read  = z_capture_read,
    .capture_close = z_capture_close,
    .close         = z_close,
};

ALP_BACKEND_REGISTER(pwm, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
