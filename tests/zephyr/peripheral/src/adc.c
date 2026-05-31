/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/adc.h> -- ADC wrapper tests.  Extracted from main.c in §C.16.
 * Covers the basic open NULL+OOB paths and the streaming ADC surface
 * (alp_adc_stream_*).  Default native_sim builds have no streaming
 * backend so streaming open returns NOSUPPORT for valid configs --
 * the NULL-cfg path stays INVAL on every backend.
 */

#include <zephyr/ztest.h>

#include "alp/adc.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_adc_null_cfg) {
    zassert_is_null(alp_adc_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_adc_unresolved_channel_yields_not_ready) {
    /* channel_id=0 is in-range; with no `alp-adc0` alias on this
     * test's overlay the spec is NULL → ALP_ERR_NOT_READY. */
    alp_adc_t *a = alp_adc_open(&(alp_adc_config_t){
        .channel_id = 0, .resolution_bits = 12});
    zassert_is_null(a);
#if defined(CONFIG_ALP_SOC_NONE)
    /* With no SoC selected, capability checks pass through and we
     * land on the device-not-ready path. */
    zassert_equal(alp_last_error(), ALP_ERR_NOT_READY);
#endif
}

/* ------------------------------------------------------------------ */
/* Streaming ADC                                                       */
/*                                                                     */
/* Default native_sim builds have no streaming backend (the Zephyr     */
/* adc_* class doesn't expose a portable streaming primitive matching  */
/* alp_adc_stream_*), so open() returns NULL + ALP_ERR_NOSUPPORT for   */
/* any non-NULL config.  The NULL-cfg path returns INVAL regardless,   */
/* and the read/close NULL-arg paths must always be safe.              */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_adc_stream_null_cfg)
{
    zassert_is_null(alp_adc_stream_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_adc_stream_read_null_handle_yields_not_ready)
{
    uint16_t     buf[4];
    size_t       got = 99u;
    alp_status_t s   = alp_adc_stream_read(NULL, buf, ARRAY_SIZE(buf), &got);
    zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
    zassert_equal(got, 0u, "got must be zeroed on failure");
}

ZTEST(alp_peripheral, test_adc_stream_read_null_got_is_inval)
{
    uint16_t     buf[4];
    alp_status_t s = alp_adc_stream_read(NULL, buf, ARRAY_SIZE(buf), NULL);
    zassert_equal(s, ALP_ERR_INVAL, "NULL got pointer must be INVAL, got %d", (int)s);
}

ZTEST(alp_peripheral, test_adc_stream_close_null_safe)
{
    /* Must not crash. */
    alp_adc_stream_close(NULL);
}

#if !defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
ZTEST(alp_peripheral, test_adc_stream_open_no_backend_yields_nosupport)
{
    /* Default build has no streaming backend.  Any well-formed cfg
     * lands on the NOSUPPORT sentinel; this catches accidental
     * regressions where a future backend silently links in. */
    alp_adc_stream_t *s = alp_adc_stream_open(&(alp_adc_stream_config_t){
        .channel_id     = 0u,
        .sample_rate_hz = 100000u,
    });
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}
#endif
