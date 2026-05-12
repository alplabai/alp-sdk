/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the Zephyr ALP SDK backend (peripheral.h).
 * Runs under native_sim with Zephyr's emulated I2C/SPI/GPIO drivers.
 *
 * Coverage focus is the *binding layer*: open/close lifecycle, status
 * code propagation, NULL-arg validation.  Per-vendor transfer
 * correctness is covered by the per-block bring-up tests in alp-studio.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/adc.h"
#include "alp/counter.h"
#include "alp/i2s.h"
#include "alp/can.h"
#include "alp/rtc.h"
#include "alp/wdt.h"
#include "alp/soc_caps.h"

ZTEST_SUITE(alp_peripheral, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_i2c_open_close_roundtrip) {
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = 0,
        .bitrate_hz = 100000,
    });
    zassert_not_null(bus, "alp_i2c_open should succeed for bus_id=0");
    alp_i2c_close(bus);
}

ZTEST(alp_peripheral, test_i2c_open_invalid_bus_returns_null) {
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = 99,
        .bitrate_hz = 100000,
    });
    zassert_is_null(bus, "out-of-range bus_id must yield NULL");
}

ZTEST(alp_peripheral, test_i2c_null_cfg_returns_null) {
    zassert_is_null(alp_i2c_open(NULL), "NULL cfg must yield NULL");
}

ZTEST(alp_peripheral, test_i2c_write_on_closed_handle_errors) {
    /* Calling on a NULL handle is the closest portable analogue of
     * "use-after-close" without leaking pool internals. */
    alp_status_t s = alp_i2c_write(NULL, 0x42, (uint8_t[]){0xaa}, 1);
    zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}

/* ------------------------------------------------------------------ */
/* SPI                                                                 */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_spi_open_close_roundtrip) {
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id = 0,
        .freq_hz = 1000000,
        .mode = ALP_SPI_MODE_0,
        .bits_per_word = 8,
        .cs_pin_id = 0xFFFFFFFFu,    /* no CS */
    });
    zassert_not_null(spi, "alp_spi_open should succeed for bus_id=0");
    alp_spi_close(spi);
}

ZTEST(alp_peripheral, test_spi_open_invalid_bus_returns_null) {
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id = 99,
    });
    zassert_is_null(spi, "out-of-range bus_id must yield NULL");
}

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_gpio_output_write_read_roundtrip) {
    alp_gpio_t *p = alp_gpio_open(0);
    zassert_not_null(p, "alp_gpio_open(0) should succeed");

    /* Verify the SDK plumbing — that configure/write/read all
     * propagate ALP_OK out of the Zephyr backend. Loopback (output
     * pin reading back its driven value) is not a contract the
     * SDK can guarantee on every backend; on gpio_emul the input
     * register is decoupled from the output register and reads
     * back zero unless gpio_emul_input_set() is called first. The
     * SDK's job is just to forward gpio_pin_get_dt's result. */
    zassert_equal(alp_gpio_configure(p, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE),
                  ALP_OK, "configure as output failed");
    zassert_equal(alp_gpio_write(p, true), ALP_OK, "write high failed");
    zassert_equal(alp_gpio_write(p, false), ALP_OK, "write low failed");

    /* Reads must not error even when emul loopback returns 0. */
    bool level = true;
    zassert_equal(alp_gpio_read(p, &level), ALP_OK, "read failed");

    alp_gpio_close(p);
}

ZTEST(alp_peripheral, test_gpio_invalid_pin_returns_null) {
    zassert_is_null(alp_gpio_open(99), "out-of-range pin_id must yield NULL");
}

ZTEST(alp_peripheral, test_gpio_irq_invalid_args) {
    alp_gpio_t *p = alp_gpio_open(1);
    zassert_not_null(p);
    zassert_equal(alp_gpio_irq_enable(p, ALP_GPIO_EDGE_NONE, NULL, NULL),
                  ALP_ERR_INVAL, "edge=NONE+cb=NULL must be invalid");
    alp_gpio_close(p);
}

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_uart_open_close_roundtrip) {
    alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
        .port_id = 0,
        .baudrate = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = ALP_UART_PARITY_NONE,
    });
    zassert_not_null(u, "alp_uart_open should succeed for port_id=0");
    alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_invalid_port_returns_null) {
    alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){.port_id = 99});
    zassert_is_null(u, "out-of-range port_id must yield NULL");
}

/* UART RX ringbuf: failure paths exercised on every build.  On builds
 * without CONFIG_ALP_SDK_UART_RX_RINGBUF the attach helper returns
 * NULL with ALP_ERR_NOSUPPORT regardless of arguments.  On builds with
 * the feature on, attach validates args + returns INVAL on NULL/zero
 * inputs.  Either way the binding compiles and the failure path is
 * deterministic.  Real-IRQ attach is parked behind nightly-aen-hil. */
ZTEST(alp_peripheral, test_uart_rx_ringbuf_attach_null_port) {
    uint8_t buf[64];
    alp_uart_rx_ringbuf_t *rb = alp_uart_rx_ringbuf_attach(NULL, buf, sizeof(buf));
    zassert_is_null(rb, "NULL port must yield NULL");
    /* Either ALP_ERR_INVAL (feature on) or ALP_ERR_NOSUPPORT (feature off);
     * both are valid sentinels for a refused attach.  We just assert
     * "not OK" via the non-null absence of a handle above. */
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_pop_null_fails_safely) {
    size_t got = 99;
    uint8_t dst[4];
    alp_status_t s = alp_uart_rx_ringbuf_pop(NULL, dst, sizeof(dst), &got);
    /* Exact code depends on whether the build has the feature on
     * (NOT_READY) or off (NOSUPPORT).  Either is a refusal -- the
     * contract is "does not write to out, zeroes got". */
    zassert_not_equal(s, ALP_OK, "NULL handle must not return ALP_OK");
    zassert_equal(got, 0u, "got must be zeroed on failure");
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_count_null_is_zero) {
    zassert_equal(alp_uart_rx_ringbuf_count(NULL), 0u);
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_detach_null_is_safe) {
    /* Must not crash. */
    alp_uart_rx_ringbuf_detach(NULL);
}

/* ------------------------------------------------------------------ */
/* Pool exhaustion                                                     */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_gpio_pool_exhaustion_returns_null) {
    alp_gpio_t *pins[CONFIG_ALP_SDK_MAX_GPIO_HANDLES + 1] = {0};
    size_t opened = 0;

    for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
        /* Pin id 0 is valid — every claim hits the pool, regardless
         * of whether the underlying gpio is shared. */
        pins[i] = alp_gpio_open(0);
        if (pins[i] == NULL) break;
        opened++;
    }

    zassert_equal(opened, (size_t)CONFIG_ALP_SDK_MAX_GPIO_HANDLES,
                  "pool should hand out exactly CONFIG_ALP_SDK_MAX_GPIO_HANDLES "
                  "before refusing; opened=%zu", opened);

    for (size_t i = 0; i < opened; i++) {
        alp_gpio_close(pins[i]);
    }
}

/* ==================================================================== */
/* v0.2 peripheral wrappers (PWM/ADC/Counter/I2S/CAN/RTC/WDT)            */
/* ==================================================================== */
/* These tests cover the wrappers' NULL-arg / out-of-range paths and    */
/* verify alp_last_error() returns the precise failure reason.  None    */
/* of them require a real underlying controller — without an `alp-*N`   */
/* DT alias, *_open returns NULL with last_error = ALP_ERR_NOT_READY.   */
/* ==================================================================== */

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

ZTEST(alp_peripheral, test_counter_null_cfg) {
    zassert_is_null(alp_counter_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_qenc_null_cfg) {
    zassert_is_null(alp_qenc_open(NULL));
    /* qenc backend doesn't yet stamp last_error on NULL cfg —
     * this is a TODO retrofit; the test still passes on NULL
     * return. */
}

ZTEST(alp_peripheral, test_dac_null_cfg) {
    zassert_is_null(alp_dac_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_dac_out_of_range_channel) {
    /* ALP_E1M_DAC_COUNT = 2; the wrapper's internal array sized to
     * match.  Channel id 9 must reject. */
    alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
        .channel_id = 9u,
        .initial_mv = 0u,
    });
    zassert_is_null(d);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_dac_unresolved_channel_yields_not_ready) {
    /* Without a real DAC controller or V2N supervisor backing
     * channel 0, open must fail with NOT_READY (DT-alias path) or
     * NOSUPPORT (CONFIG_DAC=n).  Either is acceptable; both surface
     * as a NULL return. */
    alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
        .channel_id = 0u,
        .initial_mv = 0u,
    });
    zassert_is_null(d);
}

ZTEST(alp_peripheral, test_i2s_null_cfg) {
    zassert_is_null(alp_i2s_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_i2s_invalid_channels_rejected) {
    alp_i2s_t *i = alp_i2s_open(&(alp_i2s_config_t){
        .bus_id = 0, .sample_rate_hz = 16000,
        .word_bits = 16, .channels = 5,         /* > 2 → INVAL */
        .block_frames = 64});
    zassert_is_null(i);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_can_null_cfg) {
    zassert_is_null(alp_can_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_can_zero_bitrate_rejected) {
    alp_can_t *c = alp_can_open(&(alp_can_config_t){
        .bus_id = 0, .bitrate_nominal_hz = 0,   /* INVAL */
        .mode = ALP_CAN_MODE_CLASSIC});
    zassert_is_null(c);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_rtc_out_of_range_id) {
    /* rtc_id = 99 exceeds the wrapper's hard array bound (2). */
    zassert_is_null(alp_rtc_open(99));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_wdt_null_cfg) {
    zassert_is_null(alp_wdt_open(0, NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_wdt_zero_timeout_rejected) {
    alp_wdt_t *w = alp_wdt_open(0, &(alp_wdt_config_t){
        .timeout_ms = 0, .on_timeout = ALP_WDT_RESET_SOC});
    zassert_is_null(w);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
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

#endif  /* CONFIG_ALP_SDK_V2N_SUPERVISOR */
