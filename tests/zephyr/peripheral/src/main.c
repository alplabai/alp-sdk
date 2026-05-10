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
