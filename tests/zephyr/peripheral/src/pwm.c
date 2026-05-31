/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/pwm.h> -- PWM wrapper tests.  Extracted from main.c in §C.16.
 * Covers the basic open/configure NULL+OOB paths, the v0.5 §2B.2
 * single-pulse output, and the v0.5 §2B.2 input-capture surface
 * (alp_pwm_capture_*).  All on a NOSUPPORT-contract footing under
 * native_sim; HW-in-loop validates the real bridge path once the
 * matching firmware HAL bodies land.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"

ZTEST(alp_peripheral, test_pwm_null_cfg_returns_null_and_invalidates) {
    zassert_is_null(alp_pwm_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL,
                  "expected ALP_ERR_INVAL, got %d", (int)alp_last_error());
}

ZTEST(alp_peripheral, test_pwm_out_of_range_channel_id) {
    /* channel_id = 99 exceeds the wrapper's hard array bound (8). */
    alp_pwm_t *p = alp_pwm_open(&(alp_pwm_config_t){
        .channel_id = 99, .period_ns = 1000000, .polarity = ALP_PWM_POLARITY_NORMAL});
    zassert_is_null(p);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_pwm_configure_null_handle_yields_not_ready)
{
    /* The bridge / DT-alias dispatch can't be exercised without real
     * hardware, but the binding-layer NULL check is unconditional and
     * makes sure the new public symbol links. */
    alp_status_t s =
        alp_pwm_configure(NULL, ALP_PWM_ALIGN_EDGE, /* dead_time_ns */ 0u, ALP_PWM_BREAK_NONE);
    zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}

/* ------------------------------------------------------------------ */
/* §2B.2 -- single-pulse output + input capture (NOSUPPORT contract)  */
/*                                                                    */
/* The wave-2 advanced-timer surfaces declare alp_pwm_single_pulse +  */
/* alp_pwm_capture_t with the same NOSUPPORT-with-INVAL-pre-checks    */
/* contract as the prior wave-2 stubs.  Tests verify the contract     */
/* under native_sim (no firmware HAL wired); HW-in-loop validates    */
/* the real bridge path once the corresponding bridge_hw_* HAL bodies */
/* land in the GD32 firmware tree.                                    */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_pwm_single_pulse_null_handle_not_ready)
{
    /* Public surface contract: NULL handle -> NOT_READY regardless of
     * whether the firmware HAL body for CMD_PWM_SINGLE_PULSE is wired. */
    zassert_equal(alp_pwm_single_pulse(NULL, 1000u), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_pwm_capture_open_null_cfg_returns_inval)
{
    alp_pwm_capture_t *cap = alp_pwm_capture_open(NULL);
    zassert_is_null(cap, NULL);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_peripheral, test_pwm_capture_open_channel_out_of_range_returns_range)
{
    const alp_pwm_capture_config_t cfg = {
        .channel_id = 99u, /* > 8 -- the E1M PWM ceiling */
        .edge       = ALP_PWM_CAPTURE_EDGE_RISING,
    };
    alp_pwm_capture_t *cap = alp_pwm_capture_open(&cfg);
    zassert_is_null(cap, NULL);
    zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE, NULL);
}

ZTEST(alp_peripheral, test_pwm_capture_open_valid_args_returns_nosupport)
{
    /* All args valid; backend lacks the input-capture surface today
     * so the open returns NOSUPPORT after the INVAL pre-checks. */
    const alp_pwm_capture_config_t cfg = {
        .channel_id = 0u,
        .edge       = ALP_PWM_CAPTURE_EDGE_BOTH,
    };
    alp_pwm_capture_t *cap = alp_pwm_capture_open(&cfg);
    zassert_is_null(cap, NULL);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, NULL);
}

ZTEST(alp_peripheral, test_pwm_capture_close_null_is_noop)
{
    alp_pwm_capture_close(NULL); /* must not crash */
}

ZTEST(alp_peripheral, test_pwm_capture_read_both_out_null_returns_inval)
{
    /* Documented behaviour: at least one of period_ns_out /
     * pulse_ns_out must be non-NULL.  Both NULL is INVAL. */
    int dummy = 0;
    alp_status_t s =
        alp_pwm_capture_read((alp_pwm_capture_t *)&dummy, NULL, NULL);
    zassert_equal(s, ALP_ERR_INVAL, NULL);
}
