/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the v0.1 chip drivers (lsm6dso, ssd1306, button_led).
 *
 * Coverage focus: lifecycle, NULL-arg validation, status-code
 * propagation, and the layer of plumbing between the chip driver
 * and the SDK's <alp/peripheral.h> surface.  Real I2C transfers
 * against an emulated bus go nowhere — these are smoke tests, not
 * functional verification.  Per-chip functional tests against a
 * real ICM-42670-P / SSD1306 / button live with the EVK HW-in-loop
 * suite (v0.1.x once the EVK board file is published).
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/ssd1306.h"
#include "alp/chips/button_led.h"
#include "alp/chips/bme280.h"
#include "alp/chips/lis2dw12.h"
#include "alp/chips/ssd1331.h"
#include "alp/chips/ov5640.h"
#include "alp/chips/pdm_mic.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/bmi323.h"
#include "alp/chips/bmp581.h"
#include "alp/chips/gd32g553.h"
#include "alp/chips/rtl8211fdi.h"

#include "fakes.h"

ZTEST_SUITE(alp_chips, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* lsm6dso                                                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_lsm6dso_init_null_args)
{
    lsm6dso_t  dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    zassert_equal(lsm6dso_init(NULL, bus, LSM6DSO_I2C_ADDR_LOW), ALP_ERR_INVAL,
                  "NULL ctx must be invalid");
    zassert_equal(lsm6dso_init(&dev, NULL, LSM6DSO_I2C_ADDR_LOW), ALP_ERR_INVAL,
                  "NULL bus must be invalid");
    zassert_equal(lsm6dso_init(&dev, bus, 0), ALP_ERR_INVAL, "addr=0 must be invalid");

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lsm6dso_post_init_calls_reject_uninitialised)
{
    /* Without a real chip behind the emul controller, init's
     * WHO_AM_I check will not return 0x6C.  We expect lsm6dso_init
     * to fail; subsequent reads must report NOT_READY.
     *
     * If the WHO_AM_I check returns ALP_OK against the emul (e.g.
     * if a future revision fakes it), the test still passes
     * because reads on a successfully-initialised driver return
     * ALP_OK and the assertion below is conditional. */
    lsm6dso_t  dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    alp_status_t s = lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        /* Expected path on a bare emul controller. */
        lsm6dso_axes_t axes;
        zassert_equal(lsm6dso_read_accel(&dev, &axes), ALP_ERR_NOT_READY,
                      "reads on a failed-init driver must be NOT_READY");
    }

    lsm6dso_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* ssd1306                                                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ssd1306_init_invalid_geometry)
{
    ssd1306_t  dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 96, 48), ALP_ERR_NOSUPPORT,
                  "v0.1 supports only 128x64 or 128x32");
    zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 0, 0), ALP_ERR_NOSUPPORT,
                  "zero-sized geometry must be invalid");

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ssd1306_clear_and_pixel_safe_without_init)
{
    ssd1306_t dev = {0};
    /* These two functions must be NULL-safe and tolerate an
     * uninitialised context — they only touch the in-memory
     * framebuffer, not the panel. */
    ssd1306_clear(&dev);
    ssd1306_draw_pixel(&dev, 1000, 1000, true); /* OOB silently ignored */
    ssd1306_clear(NULL);                        /* NULL is a no-op */
    ssd1306_draw_pixel(NULL, 0, 0, true);
}

ZTEST(alp_chips, test_ssd1306_display_rejects_uninitialised)
{
    ssd1306_t dev = {0};
    zassert_equal(ssd1306_display(&dev), ALP_ERR_NOT_READY,
                  "display() on uninitialised driver must be NOT_READY");
}

/* ------------------------------------------------------------------ */
/* button_led                                                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_button_led_init_null_args)
{
    alp_button_led_t bl;
    zassert_equal(alp_button_led_init(NULL, NULL), ALP_ERR_INVAL, "all-NULL must be invalid");
    zassert_equal(alp_button_led_init(&bl, NULL), ALP_ERR_INVAL, "NULL cfg must be invalid");
}

ZTEST(alp_chips, test_button_led_init_valid_pair)
{
    /* Overlay wires alp,pin-array index 0 → button (pull-up) and
     * index 1 → LED on the chips test's gpio_emul. */
    alp_button_led_t bl;
    alp_status_t     s = alp_button_led_init(&bl, &(alp_button_led_config_t){
                                                      .button_pin_id     = 0,
                                                      .led_pin_id        = 1,
                                                      .active_low_button = true,
                                                  });
    zassert_equal(s, ALP_OK, "init failed: %d", (int)s);

    /* Verify is_pressed() returns ALP_OK and writes a defined
     * value.  Don't assert WHICH value — gpio_emul's input
     * register defaults to 0, which an active-low button reports
     * as "pressed."  Driving the input register to a known value
     * needs gpio_emul_input_set(), which would couple this smoke
     * test to a Zephyr emul-specific API. */
    bool pressed;
    zassert_equal(alp_button_led_is_pressed(&bl, &pressed), ALP_OK);
    (void)pressed;

    zassert_equal(alp_button_led_set(&bl, true), ALP_OK);
    zassert_equal(alp_button_led_set(&bl, false), ALP_OK);
    zassert_equal(alp_button_led_toggle(&bl), ALP_OK);

    alp_button_led_deinit(&bl);
}

ZTEST(alp_chips, test_button_led_calls_reject_uninitialised)
{
    alp_button_led_t bl = {0};
    bool             pressed;
    zassert_equal(alp_button_led_is_pressed(&bl, &pressed), ALP_ERR_NOT_READY);
    zassert_equal(alp_button_led_set(&bl, true), ALP_ERR_NOT_READY);
    zassert_equal(alp_button_led_toggle(&bl), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* ssd1306 framebuffer logic                                           */
/*                                                                     */
/* These tests exercise the pure pixel-buffer manipulation path —      */
/* no I2C transfers, no emulator fixture.  The framebuffer is part     */
/* of the public struct, so we can drive draw_pixel / clear directly   */
/* and inspect the bytes the panel would receive via display().        */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ssd1306_draw_pixel_sets_correct_bit)
{
    /* Geometry filled in manually so init's I2C side-effects don't
     * matter — we're testing framebuffer math. */
    ssd1306_t dev = {.width = 128, .height = 64};

    ssd1306_draw_pixel(&dev, 0, 0, true);
    /* y=0 → page 0, bit 0 of column 0 → fb[0] bit 0 */
    zassert_equal(dev.fb[0], 0x01u, "got 0x%02x", dev.fb[0]);

    ssd1306_draw_pixel(&dev, 0, 7, true);
    /* y=7 → still page 0, bit 7 of column 0 → fb[0] bit 7 */
    zassert_equal(dev.fb[0], 0x81u, "got 0x%02x", dev.fb[0]);

    ssd1306_draw_pixel(&dev, 0, 8, true);
    /* y=8 → page 1, bit 0 of column 0 → fb[width] bit 0 */
    zassert_equal(dev.fb[128], 0x01u, "got 0x%02x", dev.fb[128]);

    ssd1306_draw_pixel(&dev, 127, 63, true);
    /* y=63 → page 7, bit 7 of column 127 → fb[7*128 + 127] bit 7 */
    zassert_equal(dev.fb[7 * 128 + 127], 0x80u);
}

ZTEST(alp_chips, test_ssd1306_draw_pixel_clears_bit)
{
    ssd1306_t dev = {.width = 128, .height = 64};
    ssd1306_draw_pixel(&dev, 5, 3, true);
    zassert_equal(dev.fb[5], 0x08u, "set first");

    ssd1306_draw_pixel(&dev, 5, 3, false);
    zassert_equal(dev.fb[5], 0x00u, "clear should mask the bit");
}

ZTEST(alp_chips, test_ssd1306_draw_pixel_oob_silently_ignored)
{
    ssd1306_t dev = {.width = 128, .height = 64};
    /* These must not write outside the framebuffer. */
    ssd1306_draw_pixel(&dev, 128, 0, true);     /* x at width */
    ssd1306_draw_pixel(&dev, 0, 64, true);      /* y at height */
    ssd1306_draw_pixel(&dev, 9999, 9999, true); /* far OOB */

    for (size_t i = 0; i < sizeof dev.fb; i++) {
        zassert_equal(dev.fb[i], 0u, "fb[%zu] = 0x%02x; OOB writes should be ignored", i,
                      dev.fb[i]);
    }
}

ZTEST(alp_chips, test_ssd1306_clear_wipes_only_fb)
{
    ssd1306_t dev = {.width = 128, .height = 64, .addr = 0x3C};
    for (size_t i = 0; i < sizeof dev.fb; i++)
        dev.fb[i] = 0xAA;

    ssd1306_clear(&dev);

    for (size_t i = 0; i < sizeof dev.fb; i++) {
        zassert_equal(dev.fb[i], 0u, "fb[%zu] not cleared", i);
    }
    /* Other fields preserved. */
    zassert_equal(dev.width, 128u);
    zassert_equal(dev.height, 64u);
    zassert_equal(dev.addr, 0x3Cu);
}

/* ------------------------------------------------------------------ */
/* bme280 (v0.2 chip)                                                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bme280_init_null_args)
{
    bme280_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(bme280_init(NULL, bus, BME280_I2C_ADDR_LOW), ALP_ERR_INVAL,
                  "NULL ctx must be invalid");
    zassert_equal(bme280_init(&dev, NULL, BME280_I2C_ADDR_LOW), ALP_ERR_INVAL,
                  "NULL bus must be invalid");
    zassert_equal(bme280_init(&dev, bus, 0), ALP_ERR_INVAL, "addr=0 must be invalid");

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_bme280_post_init_calls_reject_uninitialised)
{
    /* No real chip behind the emul controller — CHIP_ID won't be 0x60,
     * so init fails and downstream reads must report NOT_READY. */
    bme280_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    alp_status_t s = bme280_init(&dev, bus, BME280_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        bme280_raw_t raw;
        zassert_equal(bme280_read_raw(&dev, &raw), ALP_ERR_NOT_READY,
                      "read_raw on a failed-init driver must be NOT_READY");
        zassert_equal(bme280_set_sampling(&dev, BME280_OVERSAMPLING_X1, BME280_OVERSAMPLING_X1,
                                          BME280_OVERSAMPLING_X1, BME280_MODE_NORMAL,
                                          BME280_STANDBY_125_MS, BME280_FILTER_OFF),
                      ALP_ERR_NOT_READY);
    }

    bme280_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* lis2dw12 (v0.2 chip)                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_lis2dw12_init_null_args)
{
    lis2dw12_t dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    zassert_equal(lis2dw12_init(NULL, bus, LIS2DW12_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(lis2dw12_init(&dev, NULL, LIS2DW12_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(lis2dw12_init(&dev, bus, 0), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lis2dw12_post_init_calls_reject_uninitialised)
{
    lis2dw12_t dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    alp_status_t s = lis2dw12_init(&dev, bus, LIS2DW12_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        lis2dw12_axes_t axes;
        zassert_equal(lis2dw12_read_accel(&dev, &axes), ALP_ERR_NOT_READY);
        zassert_equal(lis2dw12_set_accel(&dev, LIS2DW12_ODR_50_HZ, LIS2DW12_FS_2G,
                                         LIS2DW12_MODE_HIGH_PERF_14BIT),
                      ALP_ERR_NOT_READY);
    }

    lis2dw12_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* ssd1331 (v0.2 chip — SPI)                                           */
/*                                                                     */
/* The SSD1331 framebuffer is 12 KiB; we keep it in BSS to avoid       */
/* putting it on the ztest thread stack.                                */
/* ------------------------------------------------------------------ */

static uint8_t ssd1331_test_fb[SSD1331_FB_BYTES];

ZTEST(alp_chips, test_ssd1331_init_null_args)
{
    ssd1331_t dev;
    zassert_equal(ssd1331_init(NULL, NULL, NULL, ssd1331_test_fb, sizeof ssd1331_test_fb),
                  ALP_ERR_INVAL);
    /* Even non-NULL ctx + NULL spi/dc/fb → INVAL. */
    zassert_equal(ssd1331_init(&dev, NULL, NULL, ssd1331_test_fb, sizeof ssd1331_test_fb),
                  ALP_ERR_INVAL);
    /* NULL framebuffer → INVAL. */
    zassert_equal(ssd1331_init(&dev, NULL, NULL, NULL, sizeof ssd1331_test_fb), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ssd1331_pixel_safe_without_init)
{
    ssd1331_t dev = {0};
    dev.fb        = ssd1331_test_fb;
    dev.fb_len    = sizeof ssd1331_test_fb;

    /* clear/draw_pixel touch only the in-memory framebuffer — safe
     * pre-init.  Clear first so any stale bits from earlier cases
     * are wiped. */
    ssd1331_clear(&dev);
    ssd1331_draw_pixel(&dev, 1000, 1000, 0xFFFFu); /* OOB silently ignored. */
    ssd1331_draw_pixel(NULL, 0, 0, 0u);            /* NULL ctx is a no-op. */
    ssd1331_clear(NULL);

    /* In-bounds pixel writes the right RGB565 bytes (MSB-first). */
    ssd1331_draw_pixel(&dev, 0, 0, 0xF800u); /* red */
    zassert_equal(ssd1331_test_fb[0], 0xF8u);
    zassert_equal(ssd1331_test_fb[1], 0x00u);
}

ZTEST(alp_chips, test_ssd1331_display_rejects_uninitialised)
{
    ssd1331_t dev = {0};
    zassert_equal(ssd1331_display(&dev), ALP_ERR_NOT_READY);
    zassert_equal(ssd1331_set_display_on(&dev, true), ALP_ERR_NOT_READY);
    zassert_equal(ssd1331_set_master_current(&dev, 0x06), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_ssd1331_rgb565_helper)
{
    /* (0xFF, 0x00, 0x00) → 0xF800 (max red).
     * (0x00, 0xFF, 0x00) → 0x07E0 (max green).
     * (0x00, 0x00, 0xFF) → 0x001F (max blue). */
    zassert_equal(ssd1331_rgb565(0xFF, 0x00, 0x00), 0xF800u);
    zassert_equal(ssd1331_rgb565(0x00, 0xFF, 0x00), 0x07E0u);
    zassert_equal(ssd1331_rgb565(0x00, 0x00, 0xFF), 0x001Fu);
}

/* ------------------------------------------------------------------ */
/* ov5640 (v0.2 chip — SCCB config side)                               */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ov5640_init_null_args)
{
    ov5640_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(ov5640_init(NULL, bus, OV5640_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov5640_init(&dev, NULL, OV5640_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov5640_init(&dev, bus, 0), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov5640_post_init_calls_reject_uninitialised)
{
    ov5640_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    alp_status_t s = ov5640_init(&dev, bus, OV5640_I2C_ADDR);
    if (s != ALP_OK) {
        zassert_equal(ov5640_set_resolution(&dev, OV5640_RES_VGA), ALP_ERR_NOT_READY);
        zassert_equal(ov5640_set_format(&dev, OV5640_FMT_RGB565), ALP_ERR_NOT_READY);
        zassert_equal(ov5640_set_test_pattern(&dev, true), ALP_ERR_NOT_READY);
    }

    ov5640_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* pdm_mic (v0.2 helper — surface only; impl returns NOSUPPORT in v0.1) */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_pdm_mic_open_returns_null_in_v01)
{
    alp_pdm_mic_t *mic = alp_pdm_mic_open(&(alp_pdm_mic_config_t){
        .peripheral_id  = 0,
        .sample_rate_hz = 16000,
        .channels       = ALP_PDM_MIC_MONO,
        .sample_bits    = 16,
    });
    zassert_is_null(mic, "v0.1 stub must return NULL until v0.2 audio lands");
}

ZTEST(alp_chips, test_pdm_mic_calls_return_nosupport)
{
    /* Even with a NULL handle (v0.1 contract), the read/set_gain
     * surface must reply ALP_ERR_NOSUPPORT — the stub asserts the
     * shape, not real arithmetic. */
    int16_t buf[16] = {0};
    size_t  n       = 999;
    zassert_equal(alp_pdm_mic_read(NULL, buf, sizeof buf / sizeof buf[0], &n, 0),
                  ALP_ERR_NOSUPPORT);
    zassert_equal(n, 0u, "out_frames must be zeroed by the stub");
    zassert_equal(alp_pdm_mic_set_gain(NULL, 0, 0), ALP_ERR_NOSUPPORT);
    alp_pdm_mic_close(NULL); /* must not crash. */
}

/* ------------------------------------------------------------------ */
/* icm42670 (TDK 6-axis IMU, on-board EVK chip)                        */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_icm42670_init_null_args)
{
    icm42670_t dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(icm42670_init(NULL, bus, ICM42670_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(icm42670_init(&dev, NULL, ICM42670_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(icm42670_init(&dev, bus, 0), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_icm42670_post_init_calls_reject_uninitialised)
{
    /* Without a real ICM-42670 on the emul controller, init's
     * WHO_AM_I check returns wrong/no value -- we expect failure
     * and that subsequent reads report NOT_READY. */
    icm42670_t dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    alp_status_t s = icm42670_init(&dev, bus, ICM42670_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        icm42670_axes_t a;
        zassert_equal(icm42670_read_accel(&dev, &a), ALP_ERR_NOT_READY);
        zassert_equal(icm42670_set_accel(&dev, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_4G),
                      ALP_ERR_NOT_READY);
    }

    icm42670_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bmi323 (Bosch 6-axis IMU, second on-board EVK IMU)                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bmi323_init_null_args)
{
    bmi323_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(bmi323_init(NULL, bus, BMI323_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(bmi323_init(&dev, NULL, BMI323_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(bmi323_init(&dev, bus, 0), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_bmi323_post_init_calls_reject_uninitialised)
{
    bmi323_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    alp_status_t s = bmi323_init(&dev, bus, BMI323_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        bmi323_axes_t a;
        zassert_equal(bmi323_read_accel(&dev, &a), ALP_ERR_NOT_READY);
        zassert_equal(bmi323_set_gyro(&dev, BMI323_ODR_100_HZ, BMI323_GYRO_FS_2000_DPS),
                      ALP_ERR_NOT_READY);
    }

    bmi323_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bmp581 (Bosch barometer, on-board EVK pressure sensor)              */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bmp581_init_null_args)
{
    bmp581_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(bmp581_init(NULL, bus, BMP581_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(bmp581_init(&dev, NULL, BMP581_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(bmp581_init(&dev, bus, 0), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_bmp581_compensate_pure_arithmetic)
{
    /* bmp581_compensate is a pure scale conversion -- it doesn't
     * touch the bus and doesn't need an initialised device.  Test
     * the conversion math directly. */

    /* 1/64 Pa LSB.  raw = 64 * 101325 + 32 (rounding) = 6484832 */
    bmp581_raw_t raw = {
        .pressure_raw    = 6484800,    /* exactly 101325 Pa */
        .temperature_raw = 25 * 65536, /* exactly 25 °C */
    };
    bmp581_compensated_t comp;
    zassert_equal(bmp581_compensate(&raw, &comp), ALP_OK);
    zassert_equal(comp.pressure_pa, 101325);
    zassert_equal(comp.temperature_c1000, 25000);

    /* NULL-arg pair must reject. */
    zassert_equal(bmp581_compensate(NULL, &comp), ALP_ERR_INVAL);
    zassert_equal(bmp581_compensate(&raw, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_bmp581_post_init_calls_reject_uninitialised)
{
    bmp581_t   dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    alp_status_t s = bmp581_init(&dev, bus, BMP581_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        bmp581_raw_t raw;
        zassert_equal(bmp581_read_raw(&dev, &raw), ALP_ERR_NOT_READY);
        zassert_equal(bmp581_set_sampling(&dev, BMP581_OSR_X4, BMP581_OSR_X1, BMP581_ODR_25_HZ,
                                          BMP581_MODE_NORMAL),
                      ALP_ERR_NOT_READY);
    }

    bmp581_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* All public headers compile cleanly when included together           */
/* ------------------------------------------------------------------ */

#include "alp/peripheral.h"
#include "alp/display.h"
#include "alp/camera.h"
#include "alp/gui.h"
#include "alp/iot.h"
#include "alp/audio.h"
#include "alp/ble.h"
#include "alp/security.h"
#include "alp/mproc.h"
#include "alp/e1m_pinout.h"
#include "alp/boards/alp_e1m_evk.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/ssd1306.h"
#include "alp/chips/button_led.h"
#include "alp/chips/bme280.h"
#include "alp/chips/lis2dw12.h"
#include "alp/chips/ssd1331.h"
#include "alp/chips/ov5640.h"
#include "alp/chips/pdm_mic.h"

ZTEST(alp_chips, test_public_headers_co_compile)
{
    /* If any of the headers above introduce a typedef/macro
     * collision the translation unit fails to build — getting
     * here at runtime is the success signal. */
    zassert_equal((int)ALP_OK, 0, "ALP_OK must remain 0 across header-set evolution");
    zassert_equal((unsigned)ALP_E1M_GPIO_IO0, 0u);
    zassert_equal((unsigned)EVK_PWM_LED_RED, ALP_E1M_PWM3,
                  "EVK feature names must layer atop the global e1m_pinout map");
}

/* ------------------------------------------------------------------ */
/* v0.2 / v0.3 stubbed surfaces — link-cleanliness + NOSUPPORT contract */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_audio_surface_v01_nosupport)
{
    /* audio.h went stub -> real on AEN-Zephyr in v0.2.  The contract
     * shifted: open(NULL cfg) still returns NULL (with last_error =
     * INVAL); start/stop/etc. on a NULL handle now report NOT_READY
     * (the standard wrapper convention) rather than NOSUPPORT.
     * The "v0.1 stubbed" naming is kept for the suite history; the
     * assertions match the v0.2 reality.  */
    zassert_is_null(alp_audio_in_open(NULL), "open(NULL cfg) -> NULL");
    zassert_is_null(alp_audio_out_open(NULL), "open(NULL cfg) -> NULL");
    zassert_equal(alp_audio_in_start(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_audio_out_start(NULL), ALP_ERR_NOT_READY);
    alp_audio_in_close(NULL);
    alp_audio_out_close(NULL);
}

ZTEST(alp_chips, test_ble_surface_v01_nosupport)
{
    /* ble.h went stub -> real on AEN-Zephyr in v0.3.  Same contract
     * shift as audio: open() with no controller still returns NULL
     * (NOSUPPORT), but operations on NULL handles now report
     * NOT_READY (the standard wrapper convention). */
    zassert_is_null(alp_ble_open(), "no BT controller -> NULL");
    zassert_equal(alp_ble_advertise_start(NULL, NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_ble_scan_stop(NULL), ALP_ERR_NOT_READY);
    alp_ble_close(NULL);
}

ZTEST(alp_chips, test_security_surface_v01_nosupport)
{
    zassert_is_null(alp_hash_open(ALP_HASH_SHA256));
    zassert_is_null(alp_aead_open(ALP_AEAD_AES_128_GCM, NULL, 0));
    uint8_t buf[16];
    zassert_equal(alp_random_bytes(buf, sizeof buf), ALP_ERR_NOSUPPORT);
    alp_hash_close(NULL);
    alp_aead_close(NULL);
}

ZTEST(alp_chips, test_mproc_surface_v01_nosupport)
{
    /* mproc.h went stub -> real on AEN-Zephyr in v0.3.  NULL-handle
     * operations move from NOSUPPORT to NOT_READY (the standard
     * wrapper convention); open() still falls through to NULL when
     * the underlying mbox/hwsem device isn't present. */
    zassert_is_null(alp_shmem_open(NULL));
    zassert_is_null(alp_mbox_open(NULL));
    zassert_is_null(alp_hwsem_open(0));
    zassert_equal(alp_hwsem_try_lock(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_mbox_send(NULL, NULL, 0, 0), ALP_ERR_NOT_READY);
    alp_shmem_close(NULL);
    alp_mbox_close(NULL);
    alp_hwsem_close(NULL);
}

/* ==================================================================== */
/* Register-protocol tests (fake i2c-emul targets)                       */
/* ==================================================================== */
/* These tests exercise actual register paths through the chip drivers, */
/* not just lifecycle / NULL handling.  Each fake target lives in       */
/* src/fake_<chip>.c, attaches to i2c0_emul at the chip's default I2C   */
/* address, and exposes inspection helpers via fakes.h.                  */
/*                                                                      */
/* TEMPORARILY GATED — the fake EMUL_DT_INST_DEFINE pattern needs a     */
/* paired DEVICE_DT_INST_DEFINE for Zephyr 3.7's native_sim x86 link    */
/* to resolve `__device_dts_ord_<N>` references the parent i2c-emul     */
/* controller's emuls_<N> array generates.  Tracked as a v0.2 follow-   */
/* up; gated on DT_NODE_EXISTS so the tests are no-ops while the        */
/* overlay's fake_* nodes are commented out.                             */
/* ==================================================================== */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso))

/* ------------------------------------------------------------------ */
/* lsm6dso (fake-backed)                                               */
/* ------------------------------------------------------------------ */

#define REG_LSM6DSO_CTRL1_XL 0x10
#define REG_LSM6DSO_CTRL2_G 0x11
#define REG_LSM6DSO_OUTX_L_A 0x28
#define REG_LSM6DSO_OUTX_L_G 0x22

ZTEST(alp_chips, test_fake_lsm6dso_init_succeeds_against_fake)
{
    fake_lsm6dso_reset();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    lsm6dso_t dev;
    zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW), ALP_OK,
                  "fake responds to WHO_AM_I with 0x6C — init must succeed");

    uint8_t id = 0;
    zassert_equal(lsm6dso_read_id(&dev, &id), ALP_OK);
    zassert_equal(id, LSM6DSO_WHO_AM_I_VAL);

    lsm6dso_deinit(&dev);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_lsm6dso_set_accel_writes_ctrl1_xl)
{
    fake_lsm6dso_reset();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    lsm6dso_t dev;
    zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW), ALP_OK);

    /* CTRL1_XL: ODR_XL[7:4] | FS_XL[3:2] | LPF2_XL_EN[1] | reserved[0].
     * ODR=104Hz (0x4), FS=4G (0x2) → byte = (0x4<<4) | (0x2<<2) = 0x48. */
    zassert_equal(lsm6dso_set_accel(&dev, LSM6DSO_ODR_104_HZ, LSM6DSO_ACCEL_FS_4G), ALP_OK);
    zassert_equal(fake_lsm6dso_get_reg(REG_LSM6DSO_CTRL1_XL), 0x48u,
                  "CTRL1_XL should encode ODR=104Hz | FS=4G");

    /* Switch to ODR=833Hz (0x7), FS=16G (0x1) → (0x7<<4) | (0x1<<2) = 0x74. */
    zassert_equal(lsm6dso_set_accel(&dev, LSM6DSO_ODR_833_HZ, LSM6DSO_ACCEL_FS_16G), ALP_OK);
    zassert_equal(fake_lsm6dso_get_reg(REG_LSM6DSO_CTRL1_XL), 0x74u);

    lsm6dso_deinit(&dev);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_lsm6dso_set_gyro_writes_ctrl2_g)
{
    fake_lsm6dso_reset();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    lsm6dso_t dev;
    zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW), ALP_OK);

    /* ODR=208Hz (0x5), FS=1000dps (0x2) → (0x5<<4) | (0x2<<2) = 0x58. */
    zassert_equal(lsm6dso_set_gyro(&dev, LSM6DSO_ODR_208_HZ, LSM6DSO_GYRO_FS_1000_DPS), ALP_OK);
    zassert_equal(fake_lsm6dso_get_reg(REG_LSM6DSO_CTRL2_G), 0x58u);

    lsm6dso_deinit(&dev);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_lsm6dso_read_accel_decodes_le16)
{
    fake_lsm6dso_reset();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    lsm6dso_t dev;
    zassert_equal(lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW), ALP_OK);

    /* Seed the accel output registers: X = 0x1234, Y = -0x100 = 0xFF00,
     * Z = 0x7FFF (positive max).  LSM6DSO returns little-endian. */
    fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 0, 0x34); /* X low */
    fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 1, 0x12); /* X high */
    fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 2, 0x00); /* Y low */
    fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 3, 0xFF); /* Y high */
    fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 4, 0xFF); /* Z low */
    fake_lsm6dso_set_reg(REG_LSM6DSO_OUTX_L_A + 5, 0x7F); /* Z high */

    lsm6dso_axes_t a;
    zassert_equal(lsm6dso_read_accel(&dev, &a), ALP_OK);
    zassert_equal(a.x, (int16_t)0x1234);
    zassert_equal(a.y, (int16_t)0xFF00); /* -256 */
    zassert_equal(a.z, (int16_t)0x7FFF);

    lsm6dso_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* ssd1306 (fake-backed)                                               */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_fake_ssd1306_init_streams_documented_opcodes)
{
    fake_ssd1306_reset_logs();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    ssd1306_t dev;
    zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 128, 64), ALP_OK,
                  "fake records command bytes — init must succeed");

    /* The init sequence MUST contain DISPLAY_OFF (0xAE), CHARGE_PUMP
     * enable (0x8D, 0x14), MEMORY_MODE horizontal (0x20, 0x00), and
     * DISPLAY_ON (0xAF) in order.  The fake's command log captures
     * every byte after the 0x00 control byte, so we scan it for the
     * documented anchors. */
    const uint8_t *log = fake_ssd1306_cmd_log();
    const size_t   len = fake_ssd1306_cmd_log_len();
    zassert_true(len > 0, "init must stream at least one command");

    /* DISPLAY_OFF must come first per the datasheet's recommended
     * power-up sequence. */
    zassert_equal(log[0], 0xAEu, "first opcode must be DISPLAY_OFF");

    /* DISPLAY_ON must come last. */
    zassert_equal(log[len - 1], 0xAFu, "last opcode must be DISPLAY_ON");

    /* Charge pump enable must appear: 0x8D followed by 0x14. */
    bool found_charge_pump = false;
    for (size_t i = 0; i + 1 < len; i++) {
        if (log[i] == 0x8Du && log[i + 1] == 0x14u) {
            found_charge_pump = true;
            break;
        }
    }
    zassert_true(found_charge_pump, "init must enable the charge pump (0x8D 0x14)");

    /* No data bytes streamed during init (no fb push). */
    zassert_equal(fake_ssd1306_data_log_len(), 0u, "init must not push any framebuffer bytes");

    ssd1306_deinit(&dev);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_ssd1306_display_pushes_full_framebuffer)
{
    fake_ssd1306_reset_logs();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    ssd1306_t dev;
    zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 128, 64), ALP_OK);

    fake_ssd1306_reset_logs(); /* drop the init bytes from the logs. */

    /* Write a single test pixel and flush. */
    ssd1306_clear(&dev);
    ssd1306_draw_pixel(&dev, 0, 0, true);
    zassert_equal(ssd1306_display(&dev), ALP_OK);

    /* display() pushes width*height/8 = 1024 framebuffer bytes. */
    zassert_equal(fake_ssd1306_data_log_len(), 1024u, "expected 1024 fb bytes, got %zu",
                  fake_ssd1306_data_log_len());

    /* The first byte (page=0, col=0) should hold our pixel. */
    zassert_equal(fake_ssd1306_data_log()[0], 0x01u, "page=0, col=0, bit 0 should be set");

    /* The address-window command must precede the data — confirm
     * the column/page-range opcodes appear in the cmd_log. */
    const uint8_t *cmd             = fake_ssd1306_cmd_log();
    const size_t   clen            = fake_ssd1306_cmd_log_len();
    bool           found_col_addr  = false;
    bool           found_page_addr = false;
    for (size_t i = 0; i < clen; i++) {
        if (cmd[i] == 0x21u) found_col_addr = true;
        if (cmd[i] == 0x22u) found_page_addr = true;
    }
    zassert_true(found_col_addr, "display must set column address (0x21)");
    zassert_true(found_page_addr, "display must set page address (0x22)");

    ssd1306_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* bme280 (fake-backed)                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_fake_bme280_init_loads_calibration)
{
    fake_bme280_reset();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    bme280_t dev;
    zassert_equal(bme280_init(&dev, bus, BME280_I2C_ADDR_LOW), ALP_OK,
                  "fake responds to CHIP_ID with 0x60 — init must succeed");

    /* Canonical Bosch datasheet example values seeded by the fake. */
    zassert_equal(dev.calib.T1, 27504u);
    zassert_equal(dev.calib.T2, (int16_t)26435);
    zassert_equal(dev.calib.T3, (int16_t)-1000);
    zassert_equal(dev.calib.P1, 36477u);
    zassert_equal(dev.calib.P2, (int16_t)-10685);
    zassert_equal(dev.calib.P9, (int16_t)6000);
    zassert_equal(dev.calib.H1, 75);
    zassert_equal(dev.calib.H6, (int8_t)30);

    bme280_deinit(&dev);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bme280_read_raw_decodes_msb_first)
{
    fake_bme280_reset();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    bme280_t dev;
    zassert_equal(bme280_init(&dev, bus, BME280_I2C_ADDR_LOW), ALP_OK);

    bme280_raw_t raw;
    zassert_equal(bme280_read_raw(&dev, &raw), ALP_OK);

    /* Pressure raw block 0x65 0x5A 0xC0 → (0x65<<12)|(0x5A<<4)|(0xC0>>4)
     * = 0x655AC = 415148 (canonical Bosch example). */
    zassert_equal(raw.pressure_raw, 415148);
    /* Temperature raw block 0x7E 0xF5 0x00 → 0x7EF50 = 519888. */
    zassert_equal(raw.temperature_raw, 519888);
    /* Humidity 0x6F 0xF0 → 0x6FF0 (synthetic). */
    zassert_equal(raw.humidity_raw, 0x6FF0u);

    bme280_deinit(&dev);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_fake_bme280_compensate_matches_datasheet_example)
{
    fake_bme280_reset();
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    bme280_t dev;
    zassert_equal(bme280_init(&dev, bus, BME280_I2C_ADDR_LOW), ALP_OK);

    bme280_raw_t raw;
    zassert_equal(bme280_read_raw(&dev, &raw), ALP_OK);

    bme280_compensated_t comp;
    zassert_equal(bme280_compensate(&dev, &raw, &comp), ALP_OK);

    /* BST-BME280-DS002 §4.2.3 Table 2 documents the worked example:
     * T = 25.08 °C → c100 = 2508,  P ≈ 100653 Pa.  Allow ±2 c100 to
     * absorb the documented ±2-LSB rounding noise across compilers. */
    zassert_within(comp.temperature_c100, 2508, 2, "compensated T x100 = %d, expected 2508 ± 2",
                   (int)comp.temperature_c100);
    zassert_within(comp.pressure_pa, 100653u, 50u, "compensated P (Pa) = %u, expected 100653 ± 50",
                   comp.pressure_pa);

    bme280_deinit(&dev);
    alp_i2c_close(bus);
}

#endif /* DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso)) — fake-emul block */

/* ------------------------------------------------------------------ */
/* gd32g553 -- V2N supervisor MCU host driver, NULL-arg validation     */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_gd32g553_init_null_ctx)
{
    zassert_equal(gd32g553_init(NULL, NULL, NULL, 0u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32g553_init_no_bus_handles)
{
    gd32g553_t ctx;
    zassert_equal(gd32g553_init(&ctx, NULL, NULL, 0u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32g553_init_invalid_i2c_addr)
{
    gd32g553_t ctx;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000,
    });
    zassert_not_null(bus);
    zassert_equal(gd32g553_init(&ctx, NULL, bus, 0x80u), ALP_ERR_INVAL,
                  "8-bit address through the 7-bit API must be rejected");
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_gd32g553_post_init_calls_reject_uninitialised)
{
    gd32g553_t ctx = {0};
    zassert_equal(gd32g553_set_default_transport(&ctx, GD32G553_TRANSPORT_SPI),
                  ALP_ERR_NOT_READY);

    uint32_t levels;
    zassert_equal(gd32g553_gpio_read(&ctx, 0u, &levels), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_gpio_write(&ctx, 0u, 0u),     ALP_ERR_NOT_READY);

    uint8_t pmic = 0u;
    zassert_equal(gd32g553_da9292_status_forward(&ctx, &pmic), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_gd32g553_pwm_set_invalid_duty)
{
    gd32g553_t ctx = {.initialised = true};
    zassert_equal(gd32g553_pwm_set(&ctx, 0u, 100000u, 200000u),
                  ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32g553_adc_read_invalid_samples)
{
    gd32g553_t ctx = {.initialised = true};
    uint16_t   mv[16];

    zassert_equal(gd32g553_adc_read(&ctx, 0u, 0u, mv), ALP_ERR_INVAL);
    zassert_equal(gd32g553_adc_read(&ctx, 0u, GD32G553_BRIDGE_ADC_MAX_SAMPLES + 1u, mv),
                  ALP_ERR_INVAL);
    zassert_equal(gd32g553_adc_read(&ctx, 0u, 1u, NULL), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* rtl8211fdi -- Realtek PHY driver, NULL-arg validation              */
/* ------------------------------------------------------------------ */

static int test_dummy_mdio_read(uint8_t phy, uint8_t reg, uint16_t *val, void *user)
{
    (void)phy; (void)reg; (void)user;
    *val = 0u;
    return 0;
}
static int test_dummy_mdio_write(uint8_t phy, uint8_t reg, uint16_t val, void *user)
{
    (void)phy; (void)reg; (void)val; (void)user;
    return 0;
}

ZTEST(alp_chips, test_rtl8211fdi_init_null_args)
{
    rtl8211fdi_t ctx;
    zassert_equal(rtl8211fdi_init(NULL, 0u, test_dummy_mdio_read,
                                  test_dummy_mdio_write, NULL),
                  ALP_ERR_INVAL);
    zassert_equal(rtl8211fdi_init(&ctx, 0u, NULL,
                                  test_dummy_mdio_write, NULL),
                  ALP_ERR_INVAL);
    zassert_equal(rtl8211fdi_init(&ctx, 0u, test_dummy_mdio_read,
                                  NULL, NULL),
                  ALP_ERR_INVAL);
    /* PHY address > 31 (5-bit address space) must be rejected. */
    zassert_equal(rtl8211fdi_init(&ctx, 32u, test_dummy_mdio_read,
                                  test_dummy_mdio_write, NULL),
                  ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_rtl8211fdi_init_oui_check_rejects_zero)
{
    /* Dummy callbacks read 0x0000 for every register -- PHYID1
     * OUI check should reject (Realtek OUI is 0x001C). */
    rtl8211fdi_t ctx;
    zassert_equal(rtl8211fdi_init(&ctx, 0u, test_dummy_mdio_read,
                                  test_dummy_mdio_write, NULL),
                  ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_rtl8211fdi_post_init_rejects_uninitialised)
{
    rtl8211fdi_t ctx = {0};

    bool up; rtl8211fdi_speed_t speed; bool fd;
    zassert_equal(rtl8211fdi_get_link(&ctx, &up, &speed, &fd),
                  ALP_ERR_NOT_READY);
    zassert_equal(rtl8211fdi_soft_reset(&ctx, 1000u),
                  ALP_ERR_NOT_READY);
    zassert_equal(rtl8211fdi_restart_autoneg(&ctx),
                  ALP_ERR_NOT_READY);
}
