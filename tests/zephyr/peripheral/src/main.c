/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the Zephyr ALP SDK backend (peripheral.h).
 * Runs under native_sim with Zephyr's emulated I2C/SPI/GPIO drivers.
 *
 * The 902-line monolith that originally lived here was split per
 * peripheral in §C.16.  Per-peripheral files (i2c.c, spi.c, gpio.c,
 * uart.c, pwm.c, adc.c, dac.c, counter.c, qenc.c, i2s.c, can.c,
 * rtc.c, wdt.c) sit alongside this file in src/ and all declare
 * their ZTESTs against the same ZTEST_SUITE(alp_peripheral) defined
 * below.  main.c keeps the suite declaration plus the cross-cutting
 * test sections that don't pair 1:1 with a single peripheral header:
 *
 *   - TMU primitives (libm-fallback + NULL-arg contracts)
 *   - Power-mode (alp_power_*) NOSUPPORT contract
 *   - AEN audit gap surfaces (gpu2d / camera ISP / storage AES)
 *   - Portable delay helpers (alp_delay_us / alp_delay_ms)
 *   - SoC capability validation (gated on CONFIG_ALP_SOC_*)
 *   - V2N supervisor cross-peripheral dispatch (gated on
 *     CONFIG_ALP_SDK_V2N_SUPERVISOR)
 *   - PSA-Crypto entropy round-trip via the GD32 TRNG
 *
 * Coverage focus across all files is the *binding layer*: open/close
 * lifecycle, status-code propagation, NULL-arg validation.  Per-vendor
 * transfer correctness is covered by the per-block bring-up tests in
 * alp-studio.
 */

#include <math.h>

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/adc.h"
#include "alp/camera.h"
#include "alp/counter.h"
#include "alp/gpu2d.h"
#include "alp/i2s.h"
#include "alp/can.h"
#include "alp/rtc.h"
#include "alp/storage.h"
#include "alp/tmu.h"
#include "alp/power.h"
#include "alp/wdt.h"
#include "alp/security.h"
#include "alp/soc_caps.h"

ZTEST_SUITE(alp_peripheral, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* <alp/tmu.h> -- CORDIC math accelerator with libm fallback           */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_tmu_sin_null_out_yields_inval)
{
    /* Single-input function: NULL @c out must be rejected before the
     * supervisor / libm dispatch.  Same shape for every primitive --
     * one representative test is enough. */
    zassert_equal(alp_tmu_sin(1.0f, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_tmu_atan2_null_out_yields_inval)
{
    /* Two-input function: separate test so the regex audit can see
     * that alp_tmu_atan2 is covered. */
    zassert_equal(alp_tmu_atan2(1.0f, 1.0f, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_tmu_hypot_null_out_yields_inval)
{
    zassert_equal(alp_tmu_hypot(3.0f, 4.0f, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_tmu_all_primitives_null_out_yield_inval)
{
    /* Belt-and-braces: every public alp_tmu_* primitive must reject a
     * NULL @c out before dispatch.  Lists each primitive once so the
     * coverage audit can pair the symbol with at least one mention. */
    zassert_equal(alp_tmu_cos(0.5f, NULL),         ALP_ERR_INVAL);
    zassert_equal(alp_tmu_tan(0.5f, NULL),         ALP_ERR_INVAL);
    zassert_equal(alp_tmu_atan(0.5f, NULL),        ALP_ERR_INVAL);
    zassert_equal(alp_tmu_log(1.0f, NULL),         ALP_ERR_INVAL);
    zassert_equal(alp_tmu_exp(0.0f, NULL),         ALP_ERR_INVAL);
    zassert_equal(alp_tmu_sinh(0.5f, NULL),        ALP_ERR_INVAL);
    zassert_equal(alp_tmu_cosh(0.5f, NULL),        ALP_ERR_INVAL);
    zassert_equal(alp_tmu_tanh(0.5f, NULL),        ALP_ERR_INVAL);
}

#if !defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* Non-V2N builds always use the libm fallback -- the call must
 * succeed regardless of bus configuration and the result must match
 * the libm reference within a few ulps. */

ZTEST(alp_peripheral, test_tmu_sqrt_libm_fallback_ok)
{
    float out = 0.0f;
    zassert_equal(alp_tmu_sqrt(4.0f, &out), ALP_OK);
    /* sqrtf(4) is exact in binary32; allow a tiny tolerance for
     * platforms that internally route through a polynomial. */
    zassert_true(fabsf(out - 2.0f) < 1.0e-6f, "got %f", (double)out);
}

ZTEST(alp_peripheral, test_tmu_sin_libm_fallback_ok)
{
    float out = 0.0f;
    /* sin(0) == 0 exactly. */
    zassert_equal(alp_tmu_sin(0.0f, &out), ALP_OK);
    zassert_true(fabsf(out) < 1.0e-6f, "got %f", (double)out);
}

ZTEST(alp_peripheral, test_tmu_hypot_libm_fallback_ok)
{
    float out = 0.0f;
    zassert_equal(alp_tmu_hypot(3.0f, 4.0f, &out), ALP_OK);
    /* 3-4-5 triangle: sqrt(9+16) == 5. */
    zassert_true(fabsf(out - 5.0f) < 1.0e-5f, "got %f", (double)out);
}
#endif  /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* V2N builds dispatch through the supervisor singleton; with both
 * bus IDs left at the default -1 the supervisor surfaces NOT_READY
 * and the alp_tmu_* call propagates it.  One representative
 * primitive is enough -- the dispatch path is shared. */
ZTEST(alp_peripheral, test_tmu_sqrt_not_ready_without_buses)
{
    float        out = -1.0f;
    alp_status_t s   = alp_tmu_sqrt(4.0f, &out);
    zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}
#endif  /* CONFIG_ALP_SDK_V2N_SUPERVISOR */

/* ------------------------------------------------------------------ */
/* §2B.3 -- system-wide power-mode transition surface                 */
/*                                                                    */
/* alp_power_open / configure_wake_source / request_sleep / close are */
/* the wave-2 sleep-mode surface in <alp/power.h>.  The wave-2 HAL    */
/* body for CMD_POWER_MODE_SET (opcode 0x28) is the gating dep on the */
/* GD32 firmware side; Zephyr's pm_policy_* hookup lands as a later   */
/* per-SoC commit.  These NOSUPPORT-contract tests verify the public  */
/* surface stays well-behaved on every native_sim build.              */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_power_open_returns_handle)
{
    alp_power_t *p = alp_power_open();
    zassert_not_null(p, NULL);
    alp_power_close(p);
}

ZTEST(alp_peripheral, test_power_configure_wake_source_records_bitmap)
{
    alp_power_t *p = alp_power_open();
    zassert_not_null(p, NULL);
    /* The configure call itself returns OK on every backend -- bits
     * are evaluated lazily at request_sleep time. */
    alp_status_t s = alp_power_configure_wake_source(
        p, ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_GPIO);
    zassert_equal(s, ALP_OK, NULL);
    alp_power_close(p);
}

ZTEST(alp_peripheral, test_power_request_sleep_run_mode_returns_inval)
{
    alp_power_t *p = alp_power_open();
    zassert_not_null(p, NULL);
    alp_power_configure_wake_source(p, ALP_POWER_WAKE_RTC);
    alp_status_t s =
        alp_power_request_sleep(p, ALP_POWER_MODE_RUN, 100u, NULL);
    zassert_equal(s, ALP_ERR_INVAL, "got %d", (int)s);
    alp_power_close(p);
}

ZTEST(alp_peripheral, test_power_request_sleep_no_wake_no_timeout_returns_inval)
{
    alp_power_t *p = alp_power_open();
    zassert_not_null(p, NULL);
    /* Zero wake bitmap + zero wake_after_ms -- SoC would never wake. */
    alp_power_configure_wake_source(p, ALP_POWER_WAKE_NONE);
    alp_status_t s =
        alp_power_request_sleep(p, ALP_POWER_MODE_DEEP_SLEEP, 0u, NULL);
    zassert_equal(s, ALP_ERR_INVAL, "got %d", (int)s);
    alp_power_close(p);
}

ZTEST(alp_peripheral, test_power_request_sleep_valid_args_returns_nosupport)
{
    /* Backend HAL not wired yet -- valid args still NOSUPPORT, but
     * the call must not crash and the optional info struct gets
     * populated with the realised_mode echo. */
    alp_power_t *p = alp_power_open();
    zassert_not_null(p, NULL);
    alp_power_configure_wake_source(p, ALP_POWER_WAKE_RTC);
    alp_power_wake_info_t info = { 0 };
    alp_status_t          s    =
        alp_power_request_sleep(p, ALP_POWER_MODE_DEEP_SLEEP, 1000u, &info);
    zassert_equal(s, ALP_ERR_NOSUPPORT, "got %d", (int)s);
    zassert_equal(info.realised_mode, ALP_POWER_MODE_DEEP_SLEEP, NULL);
    alp_power_close(p);
}

ZTEST(alp_peripheral, test_power_close_null_is_noop)
{
    alp_power_close(NULL); /* must not crash */
}

/* ------------------------------------------------------------------ */
/* AEN audit top-five gaps -- NOSUPPORT-contract tests for the new    */
/* portable surfaces (gpu2d, camera ISP, storage inline-AES).         */
/* All three return NOSUPPORT on the active V2N test build (no on-die */
/* 2D / ISP / SecAES blocks) but honour INVAL pre-checks first.       */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_gpu2d_open_returns_nosupport_no_vendor_hal)
{
    /* No vendor HAL wired -- open returns NULL with NOSUPPORT. */
    alp_gpu2d_t *g = alp_gpu2d_open();
    zassert_is_null(g, NULL);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, NULL);
}

ZTEST(alp_peripheral, test_gpu2d_fill_rect_null_handle_not_ready)
{
    const alp_gpu2d_surface_t dst = {
        .base         = (void *)0x10000000,
        .width        = 16,
        .height       = 16,
        .stride_bytes = 64,
        .format       = ALP_GPU2D_FMT_ARGB8888,
    };
    alp_status_t s = alp_gpu2d_fill_rect(NULL, &dst, 0, 0, 16, 16, 0xFF0000FFu);
    zassert_equal(s, ALP_ERR_NOT_READY, NULL);
}

ZTEST(alp_peripheral, test_camera_configure_isp_null_returns_inval)
{
    /* No camera handle needed for the INVAL pre-check: a NULL cfg
     * surfaces as INVAL before any handle access. */
    int          dummy = 0;
    alp_status_t s     = alp_camera_configure_isp((alp_camera_t *)&dummy, NULL);
    zassert_equal(s, ALP_ERR_INVAL, NULL);
}

ZTEST(alp_peripheral, test_storage_configure_inline_aes_null_returns_inval)
{
    int          dummy = 0;
    alp_status_t s     = alp_storage_configure_inline_aes((alp_storage_t *)&dummy, NULL);
    zassert_equal(s, ALP_ERR_INVAL, NULL);
}

ZTEST(alp_peripheral, test_storage_configure_inline_aes_bad_mode_returns_inval)
{
    const alp_storage_aes_config_t cfg = {
        .mode      = (alp_storage_aes_mode_t)99, /* unknown mode */
        .key       = (const uint8_t *)"0123456789abcdef",
        .key_bytes = 16u,
        .iv        = (const uint8_t *)"0123456789abcdef",
        .iv_bytes  = 16u,
    };
    int          dummy = 0;
    alp_status_t s     = alp_storage_configure_inline_aes((alp_storage_t *)&dummy, &cfg);
    zassert_equal(s, ALP_ERR_INVAL, NULL);
}

ZTEST(alp_peripheral, test_storage_configure_inline_aes_bad_key_bytes_returns_inval)
{
    const alp_storage_aes_config_t cfg = {
        .mode      = ALP_STORAGE_AES_XTS,
        .key       = (const uint8_t *)"0123456789ab",
        .key_bytes = 12u, /* not in {16, 24, 32} */
        .iv        = (const uint8_t *)"0123456789abcdef",
        .iv_bytes  = 16u,
    };
    int          dummy = 0;
    alp_status_t s     = alp_storage_configure_inline_aes((alp_storage_t *)&dummy, &cfg);
    zassert_equal(s, ALP_ERR_INVAL, NULL);
}

/* ------------------------------------------------------------------ */
/* alp_delay_us / alp_delay_ms portable busy-wait + sleep primitives  */
/*                                                                    */
/* These are foundational helpers (CC3501E §5.5 reset-timing needs    */
/* them; deepx_dxm1_bring_up also calls in).  Tests verify the 0=no-op */
/* contract and that non-zero delays actually elapse at least the     */
/* requested wall-clock time.                                         */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_delay_us_zero_is_noop)
{
    /* 0 = no-op contract.  Must not crash; should return immediately. */
    alp_delay_us(0u);
}

ZTEST(alp_peripheral, test_delay_ms_zero_is_noop)
{
    alp_delay_ms(0u);
}

ZTEST(alp_peripheral, test_delay_ms_elapses_at_least_requested)
{
    /* alp_delay_ms wraps k_msleep on Zephyr -- yields, so the
     * wall-clock elapsed time may be slightly LONGER than the
     * request (scheduler granularity) but must NEVER be less. */
    const int64_t before = k_uptime_get();
    alp_delay_ms(20u);
    const int64_t elapsed = k_uptime_get() - before;
    zassert_true(elapsed >= 20, "delay_ms(20) returned after only %lld ms", elapsed);
}

ZTEST(alp_peripheral, test_delay_us_short_spin_returns)
{
    /* alp_delay_us wraps k_busy_wait -- doesn't yield.  Verifying
     * precise sub-millisecond timing under native_sim is unreliable
     * (the simulator's busy-wait calibration varies), so this test
     * only checks the call returns at all for a short spin. */
    alp_delay_us(100u);
}

/* ------------------------------------------------------------------ */
/* SoC capability validation (only meaningful with a SoC choice set)  */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E3)

ZTEST(alp_peripheral, test_adc_resolution_exceeds_soc_max) {
    /* Alif Ensemble E3's documented maximum ADC resolution is 24
     * bits (one 24-bit channel + three 12-bit channels).  Asking
     * for 25 bits exceeds the SoC's documented cap and must be
     * rejected at open() with ALP_ERR_OUT_OF_RANGE — before any
     * I/O hits the device. */
    alp_adc_t *a = alp_adc_open(&(alp_adc_config_t){
        .channel_id = 0,
        .resolution_bits = 25,
        .reference = ALP_ADC_REF_INTERNAL,
    });
    zassert_is_null(a);
    zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE,
                  "expected OUT_OF_RANGE for 25-bit ADC on E3 (max=%d), got %d",
                  ALP_SOC_ADC_MAX_RESOLUTION_BITS,
                  (int)alp_last_error());
}

ZTEST(alp_peripheral, test_can_fd_on_soc_with_fd_support) {
    /* E3 declares CAN-FD support per metadata (can_fd: 1 in the
     * peripherals block).  Opening with FD mode must clear the
     * capability check; failure here means the soc_caps table is
     * out of sync with metadata. */
    zassert_equal(ALP_SOC_CAN_FD_SUPPORTED, 1,
                  "E3 metadata declares CAN-FD; soc_caps must agree");
}

ZTEST(alp_peripheral, test_soc_ref_str_matches_choice) {
    /* Sanity check: the gen_soc_caps.py output picked the right
     * #ifdef branch for our Kconfig choice. */
    zassert_str_equal(ALP_SOC_REF_STR, "alif:ensemble:e3");
}

#endif  /* CONFIG_ALP_SOC_ALIF_ENSEMBLE_E3 */

/* ------------------------------------------------------------------ */
/* V2N supervisor bridge dispatch (only meaningful when the supervisor */
/* singleton is compiled in).  Asserts that the bridge branches in     */
/* peripheral_pwm.c / _adc.c / _qenc.c / _counter.c / _dac.c reach the */
/* supervisor and surface NOT_READY when no bus IDs are configured.    */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)

ZTEST(alp_peripheral, test_v2n_supervisor_pwm_open_not_ready_without_buses) {
    /* Default Kconfig: SPI_BUS_ID = -1, I2C_BUS_ID = -1.  The
     * supervisor surfaces ALP_ERR_NOT_READY because neither bus is
     * configured.  alp_pwm_open must propagate that to last_error. */
    alp_pwm_t *p = alp_pwm_open(&(alp_pwm_config_t){
        .channel_id = 0u,
        .period_ns  = 1000000u,
        .polarity   = ALP_PWM_POLARITY_NORMAL,
    });
    zassert_is_null(p);
    zassert_equal(alp_last_error(), ALP_ERR_NOT_READY,
                  "expected NOT_READY, got %d", (int)alp_last_error());
}

ZTEST(alp_peripheral, test_v2n_supervisor_adc_open_not_ready_without_buses) {
    alp_adc_t *a = alp_adc_open(&(alp_adc_config_t){
        .channel_id      = 0u,
        .resolution_bits = 12u,
        .reference       = ALP_ADC_REF_INTERNAL,
    });
    zassert_is_null(a);
    zassert_equal(alp_last_error(), ALP_ERR_NOT_READY,
                  "expected NOT_READY, got %d", (int)alp_last_error());
}

ZTEST(alp_peripheral, test_v2n_supervisor_dac_open_not_ready_without_buses) {
    alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
        .channel_id = 0u,
        .initial_mv = 0u,
    });
    zassert_is_null(d);
    zassert_equal(alp_last_error(), ALP_ERR_NOT_READY,
                  "expected NOT_READY, got %d", (int)alp_last_error());
}

ZTEST(alp_peripheral, test_v2n_supervisor_qenc_open_not_ready_without_buses) {
    alp_qenc_t *q = alp_qenc_open(&(alp_qenc_config_t){
        .encoder_id     = 0u,
        .pulses_per_rev = 24u,
    });
    zassert_is_null(q);
    /* qenc doesn't stamp last_error today; NULL is enough to prove
     * the bridge branch reached the supervisor and folded the
     * NOT_READY back to NULL. */
}

ZTEST(alp_peripheral, test_v2n_supervisor_counter_open_not_ready_without_buses) {
    alp_counter_t *c = alp_counter_open(&(alp_counter_config_t){
        .counter_id = 0u,
    });
    zassert_is_null(c);
}

ZTEST(alp_peripheral, test_v2n_supervisor_counter_high_id_rejected) {
    /* The bridge advertises only one counter; counter_id >= 1 must
     * reject at open without touching the supervisor singleton. */
    alp_counter_t *c = alp_counter_open(&(alp_counter_config_t){
        .counter_id = 2u,
    });
    zassert_is_null(c);
}

ZTEST(alp_peripheral, test_v2n_supervisor_adc_stream_open_not_ready_without_buses)
{
    /* Same shape as the alp_adc_open test above: with both bus ids
     * left at -1, the supervisor surfaces ALP_ERR_NOT_READY and the
     * stream-slot bitmap MUST be rolled back so a later open can
     * still claim slot 0. */
    alp_adc_stream_t *s = alp_adc_stream_open(&(alp_adc_stream_config_t){
        .channel_id     = 0u,
        .sample_rate_hz = 100000u,
    });
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_NOT_READY, "expected NOT_READY, got %d",
                  (int)alp_last_error());
}

ZTEST(alp_peripheral, test_v2n_supervisor_adc_stream_open_channel_out_of_range)
{
    /* Channel range is checked before the supervisor acquire so the
     * test does not depend on bus configuration. */
    alp_adc_stream_t *s = alp_adc_stream_open(&(alp_adc_stream_config_t){
        .channel_id     = 8u,
        .sample_rate_hz = 100000u,
    });
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE);
}

ZTEST(alp_peripheral, test_v2n_supervisor_adc_stream_open_zero_rate_inval)
{
    alp_adc_stream_t *s = alp_adc_stream_open(&(alp_adc_stream_config_t){
        .channel_id     = 0u,
        .sample_rate_hz = 0u,
    });
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* §2A.3 -- GD32 TRNG as a PSA Crypto entropy source.                  */
/*                                                                     */
/* On V2N, src/backends/security/zephyr_drv.c registers an            */
/* mbedtls_hardware_poll() that drains the supervisor's GD32G553 TRNG  */
/* into mbedtls's platform entropy callback (gated on                  */
/* CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY).  The full PSA round-trip */
/* requires a running mbedtls stack which the peripheral scenario      */
/* doesn't pull in; what we *can* check here is that the portable      */
/* alp_random_bytes() surface itself stays well-behaved on the         */
/* NULL-arg path under the V2N supervisor build -- i.e. the link       */
/* of the new entropy code path didn't accidentally regress the        */
/* public surface's contract.                                          */
/* ------------------------------------------------------------------ */
ZTEST(alp_peripheral, test_v2n_supervisor_random_bytes_null_invalid)
{
    /* Public surface contract: NULL out + non-zero len -> INVAL,
     * regardless of whether the entropy source is the GD32 TRNG. */
    zassert_equal(alp_random_bytes(NULL, 16), ALP_ERR_INVAL);
}

#endif  /* CONFIG_ALP_SDK_V2N_SUPERVISOR */
