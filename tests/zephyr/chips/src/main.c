/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the v0.1 chip drivers (lsm6dso, ssd1306) and
 * block helpers (button_led — `<alp/blocks/>` from v0.6+).
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
#include "alp/blocks/button_led.h"
#include "alp/chips/bme280.h"
#include "alp/chips/lis2dw12.h"
#include "alp/chips/ssd1331.h"
#include "alp/chips/ov5640.h"
#include "alp/blocks/pdm_mic.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/bmi323.h"
#include "alp/chips/bmp581.h"
#include "alp/chips/gd32g553.h"
#include "alp/chips/rtl8211fdi.h"
#include "alp/chips/clk_5l35023b.h"
#include "alp/chips/murata_lbee5hy2fy.h"
#include "alp/chips/deepx_dxm1.h"
#include "alp/chips/pi3dbs12212.h"
#include "alp/chips/gd32_swd.h"
#include "alp/chips/act8760.h"
#include "alp/chips/da9292.h"
#include "alp/chips/tps628640.h"
#include "alp/chips/rv3028c7.h"
#include "alp/chips/tmp112.h"
#include "alp/chips/ina236.h"
#include "alp/chips/eeprom_24c128.h"
#include "alp/chips/tcal9538.h"
#include "alp/chips/optiga_trust_m.h"
#include "alp/chips/cam_mux_pi3wvr626.h"
#include "alp/chips/cc3501e.h"
#include "alp/chips/tas2563.h"

/* v0.5 §D.AI batch -- 18 vision / display / accelerator chips. */
#include "alp/chips/ov2640.h"
#include "alp/chips/ov5645.h"
#include "alp/chips/ov7670.h"
#include "alp/chips/ov9281.h"
#include "alp/chips/ar0234.h"
#include "alp/chips/imx219.h"
#include "alp/chips/imx477.h"
#include "alp/chips/gc2145.h"
#include "alp/chips/ti_ds90ub953_954.h"
#include "alp/chips/maxim_max9295_9296.h"
#include "alp/chips/st7789.h"
#include "alp/chips/ili9341.h"
#include "alp/chips/ili9488.h"
#include "alp/chips/ra8875.h"
#include "alp/chips/sh1106.h"
#include "alp/chips/il3820.h"
#include "alp/chips/gdew0154t8.h"
#include "alp/chips/hailo_8l.h"

/* v0.5 §D.industrial batch -- 18 industrial sensing / control chips. */
#include "alp/chips/bmp390.h"
#include "alp/chips/ms5611.h"
#include "alp/chips/lps22hb.h"
#include "alp/chips/vl53l1x.h"
#include "alp/chips/vl53l5cx.h"
#include "alp/chips/a02yyuw.h"
#include "alp/chips/drv8833.h"
#include "alp/chips/drv8825.h"
#include "alp/chips/tmc2209.h"
#include "alp/chips/a4988.h"
#include "alp/chips/as5048a_b.h"
#include "alp/chips/mt6701.h"
#include "alp/chips/hx711.h"
#include "alp/chips/max31855.h"
#include "alp/chips/max31865.h"
#include "alp/chips/tsl2591.h"
#include "alp/chips/qmc5883l.h"
#include "alp/chips/veml7700.h"

/* v0.5 §D.iot batch -- 9 IoT / connectivity chips. */
#include "alp/chips/quectel_bg95.h"
#include "alp/chips/quectel_bg77.h"
#include "alp/chips/ublox_sara_r5.h"
#include "alp/chips/semtech_sx1262.h"
#include "alp/chips/semtech_sx1276.h"
#include "alp/chips/ublox_neo_m9n.h"
#include "alp/chips/ublox_max_m10s.h"
#include "alp/chips/atgm336h.h"
#include "alp/chips/atecc608b.h"

/* v0.5 §D.audio batch -- 6 audio chips. */
#include "alp/chips/ics_43434.h"
#include "alp/chips/inmp441.h"
#include "alp/chips/wm8960.h"
#include "alp/chips/tlv320aic3204.h"
#include "alp/chips/max98357a.h"
#include "alp/chips/es8388.h"

#include "fakes.h"

ZTEST_SUITE(alp_chips, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* lsm6dso                                                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_lsm6dso_init_null_args)
{
    lsm6dso_t  dev;
    alp_i2c_t *bus =
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 100000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 100000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 100000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 100000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
#include "alp/blocks/button_led.h"
#include "alp/chips/bme280.h"
#include "alp/chips/lis2dw12.h"
#include "alp/chips/ssd1331.h"
#include "alp/chips/ov5640.h"
#include "alp/blocks/pdm_mic.h"

ZTEST(alp_chips, test_public_headers_co_compile)
{
    /* If any of the headers above introduce a typedef/macro
     * collision the translation unit fails to build — getting
     * here at runtime is the success signal. */
    zassert_equal((int)ALP_OK, 0, "ALP_OK must remain 0 across header-set evolution");
    zassert_equal((unsigned)E1M_GPIO_IO0, 0u);
    zassert_equal((unsigned)EVK_PWM_LED_RED, E1M_PWM3,
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
/* ==================================================================== */

/* Shared bus handle for the fake-backed register tests -- bus_id 0
 * resolves to the i2c0_emul controller via the alp-i2c0 DT alias. */
static alp_i2c_t *chips_test_bus(void)
{
    static alp_i2c_t *bus;
    if (bus == NULL) {
        bus = alp_i2c_open(&(alp_i2c_config_t){
            .bus_id     = 0u,
            .bitrate_hz = 400000u,
        });
    }
    return bus;
}

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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    bme280_t dev;
    zassert_equal(bme280_init(&dev, bus, BME280_I2C_ADDR_LOW), ALP_OK);

    bme280_raw_t raw;
    zassert_equal(bme280_read_raw(&dev, &raw), ALP_OK);

    /* Pressure raw block 0x65 0x5A 0xC0 → (0x65<<12)|(0x5A<<4)|(0xC0>>4)
     * = 0x655AC = 415148 (canonical Bosch example). */
    zassert_equal(raw.pressure_raw, 415148);
    /* Temperature raw block 0x7E 0xED 0x00 → 0x7EED0 = 519888. */
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
        alp_i2c_open(&(alp_i2c_config_t){.bus_id = E1M_I2C0, .bitrate_hz = 400000});
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
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 100000,
    });
    zassert_not_null(bus);
    zassert_equal(gd32g553_init(&ctx, NULL, bus, 0x80u), ALP_ERR_INVAL,
                  "8-bit address through the 7-bit API must be rejected");
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_gd32g553_post_init_calls_reject_uninitialised)
{
    gd32g553_t ctx = {0};
    zassert_equal(gd32g553_set_default_transport(&ctx, GD32G553_TRANSPORT_SPI), ALP_ERR_NOT_READY);

    uint32_t levels;
    zassert_equal(gd32g553_gpio_read(&ctx, 0u, &levels), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_gpio_write(&ctx, 0u, 0u), ALP_ERR_NOT_READY);

    uint8_t pmic = 0u;
    zassert_equal(gd32g553_da9292_status_forward(&ctx, &pmic), ALP_ERR_NOT_READY);

    /* v0.2 wrappers must obey the same NOT_READY contract. */
    uint16_t mv = 0u;
    int32_t  pos = 0;
    uint32_t ticks = 0u;
    zassert_equal(gd32g553_dac_set(&ctx, 0u, 0u), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_dac_get(&ctx, 0u, &mv), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_qenc_read(&ctx, 0u, &pos), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_qenc_reset(&ctx, 0u), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_counter_read(&ctx, 0u, &ticks), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_gd32g553_pwm_set_invalid_duty)
{
    gd32g553_t ctx = {.initialised = true};
    zassert_equal(gd32g553_pwm_set(&ctx, 0u, 100000u, 200000u), ALP_ERR_INVAL);
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

ZTEST(alp_chips, test_gd32g553_v02_invalid_args)
{
    /* Range + NULL-arg validation on the v0.2 wrappers happens before
     * any transport call so it's safe to assert against an
     * `initialised=true` stub context with no real bus handles. */
    gd32g553_t ctx = {.initialised = true};
    uint16_t   mv  = 0u;
    int32_t    pos = 0;
    uint32_t   ticks = 0u;

    /* DAC: channel out of range rejects, NULL out-pointer on DAC_GET rejects. */
    zassert_equal(gd32g553_dac_set(&ctx, GD32G553_BRIDGE_DAC_CHANNELS, 1650u),
                  ALP_ERR_INVAL);
    zassert_equal(gd32g553_dac_get(&ctx, 0u, NULL), ALP_ERR_INVAL);
    zassert_equal(gd32g553_dac_get(&ctx, GD32G553_BRIDGE_DAC_CHANNELS, &mv),
                  ALP_ERR_INVAL);

    /* QENC: encoder out of range rejects, NULL out-pointer on QENC_READ rejects. */
    zassert_equal(gd32g553_qenc_read(&ctx, 0u, NULL), ALP_ERR_INVAL);
    zassert_equal(gd32g553_qenc_read(&ctx, GD32G553_BRIDGE_QENC_CHANNELS, &pos),
                  ALP_ERR_INVAL);
    zassert_equal(gd32g553_qenc_reset(&ctx, GD32G553_BRIDGE_QENC_CHANNELS),
                  ALP_ERR_INVAL);

    /* COUNTER: counter out of range rejects, NULL out-pointer rejects. */
    zassert_equal(gd32g553_counter_read(&ctx, 0u, NULL), ALP_ERR_INVAL);
    zassert_equal(gd32g553_counter_read(&ctx, GD32G553_BRIDGE_COUNTER_CHANNELS,
                                        &ticks),
                  ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* rtl8211fdi -- Realtek PHY driver, NULL-arg validation              */
/* ------------------------------------------------------------------ */

static int test_dummy_mdio_read(uint8_t phy, uint8_t reg, uint16_t *val, void *user)
{
    (void)phy;
    (void)reg;
    (void)user;
    *val = 0u;
    return 0;
}
static int test_dummy_mdio_write(uint8_t phy, uint8_t reg, uint16_t val, void *user)
{
    (void)phy;
    (void)reg;
    (void)val;
    (void)user;
    return 0;
}

ZTEST(alp_chips, test_rtl8211fdi_init_null_args)
{
    rtl8211fdi_t ctx;
    zassert_equal(rtl8211fdi_init(NULL, 0u, test_dummy_mdio_read, test_dummy_mdio_write, NULL),
                  ALP_ERR_INVAL);
    zassert_equal(rtl8211fdi_init(&ctx, 0u, NULL, test_dummy_mdio_write, NULL), ALP_ERR_INVAL);
    zassert_equal(rtl8211fdi_init(&ctx, 0u, test_dummy_mdio_read, NULL, NULL), ALP_ERR_INVAL);
    /* PHY address > 31 (5-bit address space) must be rejected. */
    zassert_equal(rtl8211fdi_init(&ctx, 32u, test_dummy_mdio_read, test_dummy_mdio_write, NULL),
                  ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_rtl8211fdi_init_oui_check_rejects_zero)
{
    /* Dummy callbacks read 0x0000 for every register -- PHYID1
     * OUI check should reject (Realtek OUI is 0x001C). */
    rtl8211fdi_t ctx;
    zassert_equal(rtl8211fdi_init(&ctx, 0u, test_dummy_mdio_read, test_dummy_mdio_write, NULL),
                  ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_rtl8211fdi_post_init_rejects_uninitialised)
{
    rtl8211fdi_t       ctx = {0};

    bool               up;
    rtl8211fdi_speed_t speed;
    bool               fd;
    zassert_equal(rtl8211fdi_get_link(&ctx, &up, &speed, &fd), ALP_ERR_NOT_READY);
    zassert_equal(rtl8211fdi_soft_reset(&ctx, 1000u), ALP_ERR_NOT_READY);
    zassert_equal(rtl8211fdi_restart_autoneg(&ctx), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* clk_5l35023b -- Renesas/IDT audio-rate clock generator stub        */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_clk_5l35023b_init_null_args)
{
    clk_5l35023b_t ctx;
    alp_i2c_t     *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 100000,
    });
    zassert_not_null(bus);

    zassert_equal(clk_5l35023b_init(NULL, bus, CLK_5L35023B_I2C_ADDR_DEFAULT), ALP_ERR_INVAL,
                  "NULL ctx must be rejected");
    zassert_equal(clk_5l35023b_init(&ctx, NULL, CLK_5L35023B_I2C_ADDR_DEFAULT), ALP_ERR_INVAL,
                  "NULL bus must be rejected");
    /* 0x80 is out of 7-bit range. */
    zassert_equal(clk_5l35023b_init(&ctx, bus, 0x80u), ALP_ERR_INVAL,
                  "addr > 0x7F must be rejected");

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_clk_5l35023b_raw_rw_rejects_uninitialised)
{
    /* Without a real chip behind the emul controller, the I2C ACK
     * probe in clk_5l35023b_init will fail and the driver stays in
     * its zero state.  All subsequent register accesses must report
     * NOT_READY rather than IO. */
    clk_5l35023b_t ctx = {0};

    uint8_t        v;
    zassert_equal(clk_5l35023b_read_reg(&ctx, 0u, &v), ALP_ERR_NOT_READY);
    zassert_equal(clk_5l35023b_write_reg(&ctx, 0u, 0xFFu), ALP_ERR_NOT_READY);

    uint8_t dump[8];
    zassert_equal(clk_5l35023b_register_dump(&ctx, 0u, dump, sizeof dump), ALP_ERR_NOT_READY);

    /* deinit on a zero context is a no-op (idempotent). */
    clk_5l35023b_deinit(&ctx);
    clk_5l35023b_deinit(NULL);
}

ZTEST(alp_chips, test_clk_5l35023b_register_dump_rejects_invalid)
{
    /* Force the .initialised flag so the function passes the
     * NOT_READY gate and reaches its INVAL argument-validation
     * branch.  This is the same trick used by the gd32g553 tests. */
    clk_5l35023b_t ctx = {.initialised = true};

    /* count == 0 -> INVAL even with a non-NULL out. */
    uint8_t out[4];
    zassert_equal(clk_5l35023b_register_dump(&ctx, 0u, out, 0u), ALP_ERR_INVAL);

    /* NULL out -> INVAL even with a positive count. */
    zassert_equal(clk_5l35023b_register_dump(&ctx, 0u, NULL, 1u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_clk_5l35023b_typed_helpers_reject_uninitialised)
{
    /* New typed surface added with the datasheet integration --
     * Dash-Code-ID read, strap-address decode, soft power-down.
     * Each must report NOT_READY on a zeroed context. */
    clk_5l35023b_t            ctx = {0};
    uint8_t                   dashcode;
    clk_5l35023b_strap_addr_t strap;

    zassert_equal(clk_5l35023b_read_dashcode_id(&ctx, &dashcode), ALP_ERR_NOT_READY);
    zassert_equal(clk_5l35023b_get_strap_addr(&ctx, &strap), ALP_ERR_NOT_READY);
    zassert_equal(clk_5l35023b_set_power_down(&ctx, true), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_clk_5l35023b_typed_helpers_validate_args)
{
    /* .initialised forced so the function reaches the NULL-out
     * check before any bus access. */
    clk_5l35023b_t ctx = {.initialised = true};

    zassert_equal(clk_5l35023b_read_dashcode_id(&ctx, NULL), ALP_ERR_INVAL);
    zassert_equal(clk_5l35023b_get_strap_addr(&ctx, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_clk_5l35023b_get_strap_addr_decodes_general_ctrl)
{
    /* The strap address lives in Byte 0x00 bits[6:5].  Drive the
     * cached general_ctrl byte through each of the four strap
     * encodings and confirm the decoded enum value matches. */
    clk_5l35023b_t ctx = {.initialised = true};

    static const struct {
        uint8_t                   gc_byte;
        clk_5l35023b_strap_addr_t expected;
    } cases[] = {
        {.gc_byte = 0x00u, .expected = CLK_5L35023B_STRAP_ADDR_0X68},
        {.gc_byte = 0x20u, .expected = CLK_5L35023B_STRAP_ADDR_0X69},
        {.gc_byte = 0x40u, .expected = CLK_5L35023B_STRAP_ADDR_0X6A},
        {.gc_byte = 0x60u, .expected = CLK_5L35023B_STRAP_ADDR_0X6B},
    };

    for (size_t i = 0u; i < ARRAY_SIZE(cases); ++i) {
        ctx.general_ctrl              = cases[i].gc_byte;
        clk_5l35023b_strap_addr_t got = (clk_5l35023b_strap_addr_t)0xFFu;
        zassert_equal(clk_5l35023b_get_strap_addr(&ctx, &got), ALP_OK);
        zassert_equal((unsigned)got, (unsigned)cases[i].expected,
                      "gc_byte=0x%02X: expected strap %u, got %u", cases[i].gc_byte,
                      (unsigned)cases[i].expected, (unsigned)got);
    }
}

/* ------------------------------------------------------------------ */
/* murata_lbee5hy2fy -- Wi-Fi 6 + BLE 5.4 module GPIO surface         */
/*                                                                    */
/* The driver delegates the REG_ON outputs to caller-supplied         */
/* callbacks (because on V2N those lines live on the GD32 supervisor  */
/* MCU and aren't reachable through Zephyr's GPIO API).  The fake     */
/* callbacks below capture every set / get into module-local arrays   */
/* so the tests can observe what the driver wrote.                    */
/* ------------------------------------------------------------------ */

static bool fake_murata_reg_state[2];
static int  fake_murata_set_calls;

static int  fake_murata_reg_set(murata_reg_t which, bool enable, void *user)
{
    (void)user;
    fake_murata_reg_state[(int)which] = enable;
    ++fake_murata_set_calls;
    return 0;
}

ZTEST(alp_chips, test_murata_lbee5hy2fy_init_null_args)
{
    murata_lbee5hy2fy_t ctx;
    /* NULL ctx -> INVAL. */
    zassert_equal(murata_lbee5hy2fy_init(NULL, fake_murata_reg_set, NULL, NULL, NULL, NULL, NULL),
                  ALP_ERR_INVAL);
    /* NULL reg_set callback -> INVAL.  reg_get is optional so it stays
     * NULL here. */
    zassert_equal(murata_lbee5hy2fy_init(&ctx, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_murata_lbee5hy2fy_power_calls_reject_uninitialised)
{
    /* Zero-init the ctx; driver sees .initialised == false and must
     * report NOT_READY rather than IO from the power helpers. */
    murata_lbee5hy2fy_t ctx = {0};

    zassert_equal(murata_lbee5hy2fy_bt_power(&ctx, true), ALP_ERR_NOT_READY);
    zassert_equal(murata_lbee5hy2fy_wl_power(&ctx, true), ALP_ERR_NOT_READY);

    bool level;
    zassert_equal(murata_lbee5hy2fy_bt_host_wake_level(&ctx, &level), ALP_ERR_NOT_READY);
    zassert_equal(murata_lbee5hy2fy_wl_host_wake_level(&ctx, &level), ALP_ERR_NOT_READY);

    /* deinit on uninitialised must be a safe no-op. */
    murata_lbee5hy2fy_deinit(&ctx);
    murata_lbee5hy2fy_deinit(NULL);
}

ZTEST(alp_chips, test_murata_lbee5hy2fy_bt_wake_returns_nosupport_when_pin_null)
{
    /* Init the ctx with a working reg_set callback and NULL
     * bt_dev_wake (the V2N convention -- the line is not routed).
     * bt_wake_device() must report NOSUPPORT (not NOT_READY). */
    fake_murata_set_calls    = 0;
    fake_murata_reg_state[0] = true; /* seed non-zero to verify init drives low. */
    fake_murata_reg_state[1] = true;

    murata_lbee5hy2fy_t ctx;
    alp_status_t        s =
        murata_lbee5hy2fy_init(&ctx, fake_murata_reg_set, NULL, NULL, NULL, NULL, NULL);
    zassert_equal(s, ALP_OK, "init with NULL bt_dev_wake must succeed (V2N path)");
    zassert_equal(fake_murata_set_calls, 2, "init must drive BOTH regulators low");
    zassert_false(fake_murata_reg_state[(int)MURATA_REG_BT], "init must drive BT_REG_ON low");
    zassert_false(fake_murata_reg_state[(int)MURATA_REG_WL], "init must drive WL_REG_ON low");

    /* bt_dev_wake handle is NULL by construction. */
    zassert_equal(murata_lbee5hy2fy_bt_wake_device(&ctx), ALP_ERR_NOSUPPORT);

    murata_lbee5hy2fy_deinit(&ctx);
}

/* ------------------------------------------------------------------ */
/* deepx_dxm1 -- DEEPX DX-M1 NPU host-side bring-up sequencer         */
/*                                                                    */
/* The driver consumes two opened handles:                            */
/*   - alp_gpio_t for M1_RESET (Renesas PA6 on V2N-M1)                */
/*   - pi3dbs12212_t mux context                                       */
/* The mux context itself takes two alp_gpio_t pinned to PD + SEL.     */
/* For the NULL-arg coverage we only need the validation rejections   */
/* to fire; for the post-init-rejection tests we use a zeroed ctx.    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_deepx_dxm1_init_null_args)
{
    deepx_dxm1_t ctx;
    /* All three pointer args must be non-NULL.  The validation order is
     * (ctx, m1_reset, pcie_mux) — any single NULL is rejected.  We don't
     * need to construct a real mux handle since the function returns
     * INVAL before it dereferences any of them. */
    pi3dbs12212_t bogus_mux = {0};
    alp_gpio_t   *bogus_pin = (alp_gpio_t *)0xDEADBEEFu;

    zassert_equal(deepx_dxm1_init(NULL, bogus_pin, &bogus_mux, PI3DBS_STATE_PATH_0), ALP_ERR_INVAL,
                  "NULL ctx must be rejected");
    zassert_equal(deepx_dxm1_init(&ctx, NULL, &bogus_mux, PI3DBS_STATE_PATH_0), ALP_ERR_INVAL,
                  "NULL m1_reset must be rejected");
    zassert_equal(deepx_dxm1_init(&ctx, bogus_pin, NULL, PI3DBS_STATE_PATH_0), ALP_ERR_INVAL,
                  "NULL mux ctx must be rejected");

    /* Out-of-range deepx_path enum value -- caller must hit one of
     * PI3DBS_STATE_PATH_0 or PI3DBS_STATE_PATH_1.  PI3DBS_STATE_OFF
     * isn't a valid "to DEEPX" destination. */
    zassert_equal(deepx_dxm1_init(&ctx, bogus_pin, &bogus_mux, PI3DBS_STATE_OFF), ALP_ERR_INVAL,
                  "deepx_path = OFF is not a valid bring-up destination");
}

ZTEST(alp_chips, test_deepx_dxm1_bring_up_rejects_uninitialised)
{
    deepx_dxm1_t ctx = {0};
    /* The sequencer must report NOT_READY rather than dereferencing
     * NULL m1_reset_pin / pcie_mux when called on a zeroed context. */
    zassert_equal(deepx_dxm1_bring_up(&ctx, 0u), ALP_ERR_NOT_READY);
    zassert_equal(deepx_dxm1_shut_down(&ctx), ALP_ERR_NOT_READY);
    zassert_equal(deepx_dxm1_set_reset_polarity(&ctx, DEEPX_DXM1_RESET_ACTIVE_HIGH),
                  ALP_ERR_NOT_READY);

    /* deinit on a zero context must be safe. */
    deepx_dxm1_deinit(&ctx);
    deepx_dxm1_deinit(NULL);
}

ZTEST(alp_chips, test_deepx_dxm1_set_reset_polarity_invalid_value)
{
    /* Force initialised so the function reaches the polarity-range
     * check before the m1_reset_pin write -- m1_reset_pin is NULL
     * here but the check rejects on the value first. */
    deepx_dxm1_t ctx = {.initialised = true};

    /* Pass a value outside the documented enum range -- both LOW (0)
     * and HIGH (1) are valid; 2 is not. */
    zassert_equal(deepx_dxm1_set_reset_polarity(&ctx, (deepx_dxm1_reset_polarity_t)2),
                  ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* gd32_swd -- bit-bang SWD controller (host-side)                    */
/*                                                                    */
/* No real GPIO emul can drive the SWD protocol -- the bits hit the   */
/* wire faster than gpio_emul can latch state.  These tests cover the */
/* argument-validation surface + the uninitialised post-init paths.   */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_gd32_swd_init_null_args)
{
    gd32_swd_t  ctx;
    alp_gpio_t *bogus = (alp_gpio_t *)0xDEADBEEFu;

    /* NULL ctx -> INVAL.  Even with non-NULL pin handles. */
    zassert_equal(gd32_swd_init(NULL, bogus, bogus, NULL), ALP_ERR_INVAL);
    /* NULL swdio -> INVAL. */
    zassert_equal(gd32_swd_init(&ctx, NULL, bogus, NULL), ALP_ERR_INVAL);
    /* NULL swclk -> INVAL. */
    zassert_equal(gd32_swd_init(&ctx, bogus, NULL, NULL), ALP_ERR_INVAL);
    /* NULL nrst is allowed (boards that don't route it work via
     * AIRCR.SYSRESETREQ).  Not asserted here -- the gpio_emul-backed
     * init would still try alp_gpio_configure on the two bogus
     * pointers, which is not a contract this layer tests. */
}

ZTEST(alp_chips, test_gd32_swd_calls_reject_uninitialised)
{
    gd32_swd_t ctx = {0};

    /* Every post-init helper must report NOT_READY rather than
     * dereferencing NULL swdio / swclk on a zeroed context. */
    zassert_equal(gd32_swd_set_clock_delay(&ctx, 4u), ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_connect(&ctx), ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_halt(&ctx), ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_flash_erase(&ctx, GD32_SWD_FMC_FLASH_BASE, 4096u), ALP_ERR_NOT_READY);
    uint8_t buf[8] = {0};
    zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
                  ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
                  ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_reset_and_run(&ctx), ALP_ERR_NOT_READY);

    /* deinit must be safe on a zero context + on NULL. */
    gd32_swd_deinit(&ctx);
    gd32_swd_deinit(NULL);
}

ZTEST(alp_chips, test_gd32_swd_flash_helpers_reject_unconnected)
{
    /* .initialised but not .connected -- erase / write / verify
     * call gd32_swd_connect()'s outcome.  These should still report
     * NOT_READY because the SW-DP hasn't been brought up. */
    gd32_swd_t ctx    = {.initialised = true};
    uint8_t    buf[8] = {0};

    zassert_equal(gd32_swd_halt(&ctx), ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_flash_erase(&ctx, GD32_SWD_FMC_FLASH_BASE, 4096u), ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
                  ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
                  ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_gd32_swd_flash_arg_validation)
{
    /* .connected = true so the function reaches the argument-check
     * branch before bus access. */
    gd32_swd_t ctx    = {.initialised = true, .connected = true};
    uint8_t    buf[8] = {0};

    /* size == 0 -> INVAL. */
    zassert_equal(gd32_swd_flash_erase(&ctx, GD32_SWD_FMC_FLASH_BASE, 0u), ALP_ERR_INVAL);
    /* addr below flash base -> INVAL. */
    zassert_equal(gd32_swd_flash_erase(&ctx, 0x00000000u, 4096u), ALP_ERR_INVAL);

    /* NULL data / zero len -> INVAL on write + verify. */
    zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, NULL, 8u), ALP_ERR_INVAL);
    zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, 0u), ALP_ERR_INVAL);
    zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, NULL, 8u), ALP_ERR_INVAL);

    /* Misaligned addr -> INVAL.  Write requires doubleword (8-byte)
     * alignment; verify requires word (4-byte) alignment. */
    zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE + 1u, buf, sizeof buf),
                  ALP_ERR_INVAL);
    zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE + 1u, buf, sizeof buf),
                  ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32_swd_clock_delay_clamp)
{
    /* set_clock_delay clamps to [0, 2048].  We can't read the
     * private field, but the contract is that the call accepts any
     * value (no INVAL) and only NOT_READY when uninit -- exercise
     * the latter only here. */
    gd32_swd_t ctx = {0};
    zassert_equal(gd32_swd_set_clock_delay(&ctx, 0u), ALP_ERR_NOT_READY);
    zassert_equal(gd32_swd_set_clock_delay(&ctx, 100000u), ALP_ERR_NOT_READY);

    /* Once initialised the call returns OK even for an out-of-range
     * value (internally clamped). */
    ctx.initialised = true;
    zassert_equal(gd32_swd_set_clock_delay(&ctx, 100000u), ALP_OK,
                  "delay 100000 must clamp + return OK, not INVAL");
}

/* ------------------------------------------------------------------ */
/* act8760 -- ACT88760 primary PMIC on V2N BRD_I2C                    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_act8760_init_null_args)
{
    act8760_t  ctx;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(act8760_init(NULL, bus), ALP_ERR_INVAL);
    zassert_equal(act8760_init(&ctx, NULL), ALP_ERR_INVAL);

    /* _init_at takes the same constraints plus an explicit address. */
    zassert_equal(act8760_init_at(NULL, bus, ACT8760_I2C_ADDR_PAGE0), ALP_ERR_INVAL);
    zassert_equal(act8760_init_at(&ctx, NULL, ACT8760_I2C_ADDR_PAGE0), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_act8760_calls_reject_uninitialised)
{
    act8760_t        ctx = {0};
    act8760_status_t status;
    uint8_t          v;

    zassert_equal(act8760_get_status(&ctx, &status), ALP_ERR_NOT_READY);
    zassert_equal(act8760_read_reg(&ctx, ACT8760_PAGE_SYSTEM, 0u, &v), ALP_ERR_NOT_READY);
    zassert_equal(act8760_write_reg(&ctx, ACT8760_PAGE_SYSTEM, 0u, 0u), ALP_ERR_NOT_READY);
    zassert_equal(act8760_rail_get_vset(&ctx, ACT8760_RAIL_BUCK1, &v), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* da9292 -- DA9292 secondary PMIC on V2N BRD_I2C                     */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_da9292_init_null_args)
{
    da9292_t   ctx;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(da9292_init(NULL, bus, DA9292_I2C_ADDR_V2N), ALP_ERR_INVAL);
    zassert_equal(da9292_init(&ctx, NULL, DA9292_I2C_ADDR_V2N), ALP_ERR_INVAL);
    /* 8-bit-encoded address through the 7-bit API must be rejected. */
    zassert_equal(da9292_init(&ctx, bus, 0x80u), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_da9292_calls_reject_uninitialised)
{
    da9292_t        ctx = {0};
    da9292_status_t status;
    da9292_events_t events;
    uint8_t         v;
    uint16_t        mv;

    zassert_equal(da9292_get_status(&ctx, &status), ALP_ERR_NOT_READY);
    zassert_equal(da9292_read_and_clear_events(&ctx, &events), ALP_ERR_NOT_READY);
    zassert_equal(da9292_set_enable(&ctx, DA9292_CH1, false), ALP_ERR_NOT_READY);
    zassert_equal(da9292_set_voltage_mv(&ctx, DA9292_CH1, 800u), ALP_ERR_NOT_READY);
    zassert_equal(da9292_get_voltage_mv(&ctx, DA9292_CH1, &mv), ALP_ERR_NOT_READY);
    zassert_equal(da9292_read_reg(&ctx, 0u, &v), ALP_ERR_NOT_READY);
    zassert_equal(da9292_v2n_base_init(&ctx), ALP_ERR_NOT_READY);
    zassert_equal(da9292_v2n_m1_enable_deepx_rail(&ctx, 5000u), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_da9292_set_voltage_range_validation)
{
    /* Force .initialised so the function reaches the range-check
     * before any bus access. */
    da9292_t ctx = {.initialised = true};

    /* Below the documented range (300..1275 mV). */
    zassert_equal(da9292_set_voltage_mv(&ctx, DA9292_CH1, 100u), ALP_ERR_INVAL);
    /* Above the documented range. */
    zassert_equal(da9292_set_voltage_mv(&ctx, DA9292_CH1, 2000u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_da9292_get_fault_pins)
{
    uint8_t flags = 0xAAu;

    /* NULL out-pointer is the only hard-invalid argument. */
    zassert_equal(da9292_get_fault_pins(NULL, NULL, NULL), ALP_ERR_INVAL);

    /* Both pins NULL is legal (a board may wire neither): each bit
     * reports deasserted, so the packed byte must be 0x00 -- never
     * the bridge's 0xFF "no sample" sentinel. */
    zassert_equal(da9292_get_fault_pins(NULL, NULL, &flags), ALP_OK);
    zassert_equal(flags, 0x00u);
}

/* ------------------------------------------------------------------ */
/* da9292 (fake-backed) -- register-level TDD locks                   */
/* ------------------------------------------------------------------ */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_da9292))

ZTEST(alp_chips, test_da9292_fake_probe_reads_ids)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);
    zassert_equal(pmic.dev_id, 0xEA, "PMC_DEV_ID reset value (Table 12 p.35)");
    zassert_equal(pmic.rev_id, 0x10, "PMC_REV_ID reset value");
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_status_decode_matches_table14)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);

    /* Asymmetric pattern across the CH2-upper/CH1-lower pairs:
     * S_CH2_OC (bit7) | S_CH1_UV (bit2) | S_CH1_PG (bit0) = 0x85. */
    fake_da9292_set_reg(0x00, 0x85);
    fake_da9292_set_reg(0x01, 0x05); /* S_TEMP_WARN | S_VIN_UVLO */

    da9292_status_t st;
    zassert_equal(da9292_get_status(&pmic, &st), ALP_OK);
    zassert_true(st.ch2_oc);
    zassert_false(st.ch1_oc);
    zassert_true(st.ch1_uv);
    zassert_false(st.ch2_uv);
    zassert_true(st.ch1_pg);
    zassert_false(st.ch2_pg);
    zassert_false(st.ch1_ov);
    zassert_false(st.ch2_ov);
    zassert_true(st.temp_warn);
    zassert_true(st.vin_uvlo);
    zassert_false(st.temp_crit);
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_events_write1_to_clear)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);

    fake_da9292_set_reg(0x02, 0x18); /* E_CH1_OV | E_CH2_UV */
    da9292_events_t ev;
    zassert_equal(da9292_read_and_clear_events(&pmic, &ev), ALP_OK);
    zassert_true(ev.e_ch1_ov);
    zassert_true(ev.e_ch2_uv);
    zassert_false(ev.e_ch2_ov);
    /* RWC1: the driver echoed 0x18 back; the hook cleared the bits. */
    zassert_equal(fake_da9292_get_reg(0x02), 0x00);
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_voltage_encoding_roundtrip)
{
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);

    /* 750 mV at VSTEP=0: 0x3C + (750-300)/5 = 0x3C + 90 = 0x96. */
    zassert_equal(da9292_set_voltage_mv(&pmic, DA9292_CH2, 750), ALP_OK);
    zassert_equal(fake_da9292_get_reg(0x0C), 0x96, "Table 24 encoding");
    uint16_t mv = 0;
    zassert_equal(da9292_get_voltage_mv(&pmic, DA9292_CH2, &mv), ALP_OK);
    zassert_equal(mv, 750);

    /* Range guards: 0x00..0x3B are reserved bytes; <300 / >1275 mV invalid. */
    zassert_equal(da9292_set_voltage_mv(&pmic, DA9292_CH1, 299), ALP_ERR_INVAL);
    zassert_equal(da9292_set_voltage_mv(&pmic, DA9292_CH1, 1280), ALP_ERR_INVAL);
    fake_da9292_set_reg(0x0A, 0x3B); /* reserved byte */
    zassert_equal(da9292_get_voltage_mv(&pmic, DA9292_CH1, &mv), ALP_ERR_IO);
    da9292_deinit(&pmic);
}

ZTEST(alp_chips, test_da9292_deepx_rail_clears_vstep_before_vout)
{
    /* THE trap: AROVx OTP boots CH2_VSTEP=1; writing the 0.75 V byte
     * (0x96) at VSTEP=1 would put 1.5 V on DEEPX.  Assert the driver
     * clears VSTEP (a CTRL_01 write) BEFORE any VOUT_CH2 write, via
     * the fake's ordered write log. */
    fake_da9292_reset();
    alp_i2c_t *bus = chips_test_bus();
    da9292_t   pmic;
    zassert_equal(da9292_init(&pmic, bus, 0x1E), ALP_OK);
    fake_da9292_set_reg(0x00, 0x02); /* pre-seed S_CH2_PG so the poll exits */

    zassert_equal(da9292_v2n_m1_enable_deepx_rail(&pmic, 1000), ALP_OK);

    /* Walk the log: the first CTRL_01 (0x07) write must precede the
     * first VOUT_CH2_00 (0x0C) write, and must have VSTEP (bit7) low. */
    int idx_ctrl = -1, idx_vout = -1;
    for (uint8_t i = 0; i < fake_da9292_log_count(); i++) {
        uint8_t r, v;
        fake_da9292_log_at(i, &r, &v);
        if (r == 0x07 && idx_ctrl < 0) {
            idx_ctrl = i;
            zassert_equal(v & 0x80, 0, "VSTEP must be cleared by the first CTRL_01 write");
        }
        if (r == 0x0C && idx_vout < 0) idx_vout = i;
    }
    zassert_true(idx_ctrl >= 0, "driver never wrote PMC_CTRL_01");
    zassert_true(idx_vout >= 0, "driver never wrote PMC_VOUT_CH2_00");
    zassert_true(idx_ctrl < idx_vout, "VSTEP clear must precede the VOUT write");
    zassert_equal(fake_da9292_get_reg(0x0C), 0x96);
    /* Final CTRL_01: CH2_EN set, VSTEP still clear. */
    zassert_equal(fake_da9292_get_reg(0x07) & 0x82, 0x02);
    da9292_deinit(&pmic);
}

#endif /* DT_NODE_EXISTS(DT_NODELABEL(fake_da9292)) */

/* ------------------------------------------------------------------ */
/* tps628640 -- single-channel buck (multi-instance)                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tps628640_init_null_args)
{
    tps628640_t ctx;
    alp_i2c_t  *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(tps628640_init(NULL, bus, 0x44u, 1050u), ALP_ERR_INVAL);
    zassert_equal(tps628640_init(&ctx, NULL, 0x44u, 1050u), ALP_ERR_INVAL);
    /* 8-bit-encoded address through the 7-bit API. */
    zassert_equal(tps628640_init(&ctx, bus, 0x80u, 1050u), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tps628640_calls_reject_uninitialised)
{
    tps628640_t ctx = {0};
    uint16_t    mv;
    uint8_t     v;

    zassert_equal(tps628640_set_voltage_mv(&ctx, 1050u), ALP_ERR_NOT_READY);
    zassert_equal(tps628640_get_voltage_mv(&ctx, &mv), ALP_ERR_NOT_READY);
    zassert_equal(tps628640_get_status(&ctx, &v), ALP_ERR_NOT_READY);
    zassert_equal(tps628640_read_reg(&ctx, 0x01u, &v), ALP_ERR_NOT_READY);
    zassert_equal(tps628640_write_reg(&ctx, 0x01u, 0xA0u), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_tps628640_set_voltage_out_of_range)
{
    /* Force .initialised so the function reaches the range-check
     * before bus access.  Documented range 400..1675 mV. */
    tps628640_t ctx = {.initialised = true};
    zassert_equal(tps628640_set_voltage_mv(&ctx, 200u), ALP_ERR_OUT_OF_RANGE);
    zassert_equal(tps628640_set_voltage_mv(&ctx, 2000u), ALP_ERR_OUT_OF_RANGE);
    /* Same range check applies to VOUT2 (the VID-pin-high register). */
    zassert_equal(tps628640_set_voltage2_mv(&ctx, 200u), ALP_ERR_OUT_OF_RANGE);
    zassert_equal(tps628640_set_voltage2_mv(&ctx, 2000u), ALP_ERR_OUT_OF_RANGE);
}

ZTEST(alp_chips, test_tps628640_control_helpers_reject_uninitialised)
{
    /* Datasheet-integration follow-up: typed CONTROL helpers must
     * report NOT_READY on a zeroed context like every other call. */
    tps628640_t ctx = {0};
    zassert_equal(tps628640_software_enable(&ctx, true), ALP_ERR_NOT_READY);
    zassert_equal(tps628640_set_fpwm_mode(&ctx, true), ALP_ERR_NOT_READY);
    zassert_equal(tps628640_set_ramp_speed(&ctx, TPS628640_RAMP_1_MV_PER_US), ALP_ERR_NOT_READY);
    zassert_equal(tps628640_reset_to_defaults(&ctx), ALP_ERR_NOT_READY);
    uint16_t mv;
    zassert_equal(tps628640_get_voltage2_mv(&ctx, &mv), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_tps628640_set_ramp_speed_invalid)
{
    /* Force .initialised so the function reaches the enum check.
     * Documented range 0..3; 4 is invalid. */
    tps628640_t ctx = {.initialised = true};
    zassert_equal(tps628640_set_ramp_speed(&ctx, (tps628640_ramp_speed_t)4u), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* pi3dbs12212 -- passive 2:1 PCIe mux                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_pi3dbs12212_init_null_args)
{
    pi3dbs12212_t ctx;
    alp_gpio_t   *bogus = (alp_gpio_t *)0xDEADBEEFu;

    zassert_equal(pi3dbs12212_init(NULL, bogus, bogus), ALP_ERR_INVAL);
    zassert_equal(pi3dbs12212_init(&ctx, NULL, bogus), ALP_ERR_INVAL);
    zassert_equal(pi3dbs12212_init(&ctx, bogus, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_pi3dbs12212_calls_reject_uninitialised)
{
    pi3dbs12212_t       ctx = {0};
    pi3dbs12212_state_t state;

    zassert_equal(pi3dbs12212_set_state(&ctx, PI3DBS_STATE_PATH_0), ALP_ERR_NOT_READY);
    zassert_equal(pi3dbs12212_get_state(&ctx, &state), ALP_ERR_NOT_READY);

    /* deinit on a zero ctx is a no-op. */
    pi3dbs12212_deinit(&ctx);
    pi3dbs12212_deinit(NULL);
}

ZTEST(alp_chips, test_pi3dbs12212_set_state_invalid_value)
{
    /* Force .initialised so the function reaches the enum check.
     * pd / sel are NULL here but the check rejects on the value
     * first. */
    pi3dbs12212_t ctx = {.initialised = true};

    /* PI3DBS_STATE_OFF / PATH_0 / PATH_1 are 0 / 1 / 2; 3 isn't
     * valid. */
    zassert_equal(pi3dbs12212_set_state(&ctx, (pi3dbs12212_state_t)3), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* rv3028c7 -- Micro Crystal RV-3028-C7 RTC                           */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_rv3028c7_init_null_args)
{
    rv3028c7_t ctx;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(rv3028c7_init(NULL, bus), ALP_ERR_INVAL);
    zassert_equal(rv3028c7_init(&ctx, NULL), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_rv3028c7_calls_reject_uninitialised)
{
    rv3028c7_t             ctx   = {0};
    rv3028c7_time_t        when  = {.year = 2026, .month = 5, .day = 13};
    rv3028c7_alarm_match_t match = {.match_minute = true};
    bool                   fired;
    uint8_t                status_seen;

    zassert_equal(rv3028c7_get_time(&ctx, &when), ALP_ERR_NOT_READY);
    zassert_equal(rv3028c7_set_time(&ctx, &when), ALP_ERR_NOT_READY);
    zassert_equal(rv3028c7_set_alarm(&ctx, &when, &match), ALP_ERR_NOT_READY);
    zassert_equal(rv3028c7_alarm_int_enable(&ctx, true), ALP_ERR_NOT_READY);
    zassert_equal(rv3028c7_alarm_check_and_clear(&ctx, &fired), ALP_ERR_NOT_READY);
    zassert_equal(rv3028c7_dispatch_irq(&ctx, &status_seen), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_rv3028c7_register_handler_validates_src)
{
    /* Force .initialised so the function reaches the src-range
     * check before any I2C work. */
    rv3028c7_t ctx = {.initialised = true};

    /* Source value beyond the documented enum (RV3028C7_SRC_COUNT = 7)
     * must be rejected. */
    zassert_equal(rv3028c7_register_handler(&ctx, (rv3028c7_src_t)RV3028C7_SRC_COUNT, NULL, NULL),
                  ALP_ERR_INVAL);

    /* NULL handler is documented as "unregister" -- must NOT be an
     * INVAL.  A valid source + NULL handler should succeed. */
    zassert_equal(rv3028c7_register_handler(&ctx, RV3028C7_SRC_ALARM, NULL, NULL), ALP_OK);
}

/* ------------------------------------------------------------------ */
/* tmp112 -- TI temperature sensor                                    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tmp112_init_null_args)
{
    tmp112_t   ctx;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(tmp112_init(NULL, bus, TMP112_I2C_ADDR_GND), ALP_ERR_INVAL);
    zassert_equal(tmp112_init(&ctx, NULL, TMP112_I2C_ADDR_GND), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tmp112_calls_reject_uninitialised)
{
    tmp112_t ctx = {0};
    int32_t  temp_mc;

    zassert_equal(tmp112_set_rate(&ctx, TMP112_RATE_4_HZ), ALP_ERR_NOT_READY);
    zassert_equal(tmp112_set_extended_mode(&ctx, false), ALP_ERR_NOT_READY);
    zassert_equal(tmp112_read_temp_milli_c(&ctx, &temp_mc), ALP_ERR_NOT_READY);

    tmp112_deinit(&ctx);
    tmp112_deinit(NULL);
}

/* ------------------------------------------------------------------ */
/* ina236 -- TI current / voltage / power monitor                     */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ina236_init_null_args)
{
    /* The init signature requires several extra calibration
     * parameters; check NULL ctx / NULL bus + invalid shunt
     * resistance are all rejected.  The driver accepts addr_7bit == 0
     * as "fall back to default" so don't test that as INVAL. */
    /* Forward-declare the init signature locally so the test
     * compiles even if the header's extra args shift around;
     * this matches the real signature documented in ina236.h. */
    extern alp_status_t ina236_init(ina236_t * ctx, alp_i2c_t * bus, uint8_t addr_7bit,
                                    float shunt_ohms, float max_current_a,
                                    ina236_adcrange_t adcrange);
    ina236_t            ctx;
    alp_i2c_t          *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(ina236_init(NULL, bus, 0x40u, 0.010f, 1.0f, INA236_ADCRANGE_81MV), ALP_ERR_INVAL);
    zassert_equal(ina236_init(&ctx, NULL, 0x40u, 0.010f, 1.0f, INA236_ADCRANGE_81MV),
                  ALP_ERR_INVAL);
    /* shunt_ohms <= 0 must be rejected (datasheet's CURRENT_LSB
     * formula divides by it). */
    zassert_equal(ina236_init(&ctx, bus, 0x40u, 0.0f, 1.0f, INA236_ADCRANGE_81MV), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* eeprom_24c128 -- generic 24Cxx I2C EEPROM                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_eeprom_24c128_init_null_args)
{
    eeprom_24c128_t ctx;
    alp_i2c_t      *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(eeprom_24c128_init(NULL, bus, EEPROM_24C128_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(eeprom_24c128_init(&ctx, NULL, EEPROM_24C128_I2C_ADDR_LOW), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_eeprom_24c128_io_rejects_uninitialised)
{
    eeprom_24c128_t ctx         = {0};
    uint8_t         scratch[16] = {0};

    zassert_equal(eeprom_24c128_read(&ctx, 0u, scratch, sizeof scratch), ALP_ERR_NOT_READY);
    zassert_equal(eeprom_24c128_write(&ctx, 0u, scratch, sizeof scratch), ALP_ERR_NOT_READY);

    eeprom_24c128_deinit(&ctx);
    eeprom_24c128_deinit(NULL);
}

ZTEST(alp_chips, test_eeprom_24c128_io_validates_range)
{
    /* Force .initialised so the function reaches the bounds check. */
    eeprom_24c128_t ctx         = {.initialised = true};
    uint8_t         scratch[16] = {0};

    /* Read past the end of the device (16 KB).  Driver reports
     * OUT_OF_RANGE because the offset+len addresses a region the
     * chip doesn't have. */
    zassert_equal(eeprom_24c128_read(&ctx, EEPROM_24C128_BYTES - 8u, scratch, 16u),
                  ALP_ERR_OUT_OF_RANGE);
    /* Write past the end. */
    zassert_equal(eeprom_24c128_write(&ctx, EEPROM_24C128_BYTES - 8u, scratch, 16u),
                  ALP_ERR_OUT_OF_RANGE);
    /* NULL data buffer with non-zero length -> INVAL.  (NULL +
     * zero-length is a documented no-op short-circuit). */
    zassert_equal(eeprom_24c128_read(&ctx, 0u, NULL, 8u), ALP_ERR_INVAL);
    zassert_equal(eeprom_24c128_write(&ctx, 0u, NULL, 8u), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* tcal9538 -- TI 8-channel I2C I/O expander                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tcal9538_init_null_args)
{
    tcal9538_t ctx;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(tcal9538_init(NULL, bus, TCAL9538_I2C_ADDR_BASE), ALP_ERR_INVAL);
    zassert_equal(tcal9538_init(&ctx, NULL, TCAL9538_I2C_ADDR_BASE), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tcal9538_calls_reject_uninitialised)
{
    tcal9538_t ctx = {0};
    bool       level;
    uint8_t    bits;

    zassert_equal(tcal9538_set_direction(&ctx, 0u, TCAL9538_DIR_OUTPUT), ALP_ERR_NOT_READY);
    zassert_equal(tcal9538_set_directions(&ctx, 0xFFu, 0x00u), ALP_ERR_NOT_READY);
    zassert_equal(tcal9538_set(&ctx, 0u, true), ALP_ERR_NOT_READY);
    zassert_equal(tcal9538_get(&ctx, 0u, &level), ALP_ERR_NOT_READY);
    zassert_equal(tcal9538_read_all(&ctx, &bits), ALP_ERR_NOT_READY);
    zassert_equal(tcal9538_write_all(&ctx, 0u), ALP_ERR_NOT_READY);

    tcal9538_deinit(&ctx);
    tcal9538_deinit(NULL);
}

ZTEST(alp_chips, test_tcal9538_pin_index_validation)
{
    /* Force .initialised so the function reaches the pin-index
     * check.  The chip has 8 pins (0..7); 8+ is invalid. */
    tcal9538_t ctx = {.initialised = true};

    zassert_equal(tcal9538_set_direction(&ctx, 8u, TCAL9538_DIR_OUTPUT), ALP_ERR_INVAL);
    zassert_equal(tcal9538_set(&ctx, 99u, true), ALP_ERR_INVAL);
    bool level;
    zassert_equal(tcal9538_get(&ctx, 99u, &level), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* optiga_trust_m -- Infineon secure element                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_optiga_trust_m_init_null_args)
{
    optiga_trust_m_t ctx;
    alp_i2c_t       *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    zassert_equal(optiga_trust_m_init(NULL, bus, OPTIGA_TRUST_M_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(optiga_trust_m_init(&ctx, NULL, OPTIGA_TRUST_M_I2C_ADDR), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_optiga_trust_m_calls_reject_uninitialised)
{
    optiga_trust_m_t              ctx = {0};
    optiga_trust_m_product_info_t info;
    uint8_t                       apdu[8] = {0};
    uint8_t                       resp[16];
    size_t                        resp_len;

    zassert_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_ERR_NOT_READY);
    zassert_equal(
        optiga_trust_m_send_apdu(&ctx, apdu, sizeof apdu, resp, sizeof resp, &resp_len, 100u),
        ALP_ERR_NOT_READY);

    optiga_trust_m_deinit(&ctx);
    optiga_trust_m_deinit(NULL);
}

/* ------------------------------------------------------------------ */
/* cam_mux_pi3wvr626 -- MIPI CSI 2:1 mux (GPIO-only)                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_cam_mux_pi3wvr626_init_null_args)
{
    cam_mux_pi3wvr626_t ctx;
    alp_gpio_t         *bogus = (alp_gpio_t *)0xDEADBEEFu;

    zassert_equal(cam_mux_pi3wvr626_init(NULL, bogus), ALP_ERR_INVAL);
    zassert_equal(cam_mux_pi3wvr626_init(&ctx, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_cam_mux_pi3wvr626_calls_reject_uninitialised)
{
    cam_mux_pi3wvr626_t       ctx = {0};
    cam_mux_pi3wvr626_input_t got;

    zassert_equal(cam_mux_pi3wvr626_select(&ctx, (cam_mux_pi3wvr626_input_t)0), ALP_ERR_NOT_READY);
    zassert_equal(cam_mux_pi3wvr626_get(&ctx, &got), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* cc3501e -- TI Wi-Fi 6 + BLE 5.4 coprocessor (E1M-AEN)              */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_cc3501e_init_null_args)
{
    cc3501e_t  ctx;
    alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = 0u,
        .freq_hz       = 10000000u,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8u,
        .cs_pin_id     = 0u,
    });
    /* The test rig's SPI emul may or may not be available; either
     * way the NULL-arg paths are testable. */
    zassert_equal(cc3501e_init(NULL, bus), ALP_ERR_INVAL);
    zassert_equal(cc3501e_init(&ctx, NULL), ALP_ERR_INVAL);
    if (bus != NULL) alp_spi_close(bus);
}

ZTEST(alp_chips, test_cc3501e_calls_reject_uninitialised)
{
    cc3501e_t ctx = {0};
    uint16_t  version;
    uint8_t   tx[4]  = {0}, rx[4];
    size_t    rx_len = sizeof rx;

    zassert_equal(cc3501e_reset(&ctx), ALP_ERR_NOT_READY);
    zassert_equal(cc3501e_get_version(&ctx, &version), ALP_ERR_NOT_READY);
    zassert_equal(
        cc3501e_request(&ctx, (alp_cc3501e_cmd_t)0, tx, sizeof tx, rx, sizeof rx, &rx_len, 100u),
        ALP_ERR_NOT_READY);
    zassert_equal(cc3501e_set_event_callback(&ctx, NULL, NULL), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* tas2563 -- TI smart Class-D speaker amplifier                      */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tas2563_init_null_args)
{
    tas2563_t  ctx;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    zassert_not_null(bus);

    /* sd_n is optional in the driver -- not required to be non-NULL. */
    zassert_equal(tas2563_init(NULL, bus, 0x4Du, NULL), ALP_ERR_INVAL);
    zassert_equal(tas2563_init(&ctx, NULL, 0x4Du, NULL), ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tas2563_calls_reject_uninitialised)
{
    tas2563_t ctx = {0};
    uint8_t   rev;

    zassert_equal(tas2563_read_revision(&ctx, &rev), ALP_ERR_NOT_READY);
    zassert_equal(tas2563_set_mode(&ctx, (tas2563_mode_t)0), ALP_ERR_NOT_READY);
    zassert_equal(tas2563_set_hw_enable(&ctx, true), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* GD32G553 v0.5 host helpers (§2B.2 + §2B.3 + Task #10) -- NOT_READY  */
/* + INVAL contracts against an uninitialised ctx.  Wire-side          */
/* behaviour validates against the firmware HAL bodies in HW-in-loop. */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_gd32g553_v05_calls_reject_uninitialised)
{
    gd32g553_t ctx       = { 0 };
    uint32_t   period_ns = 0u;
    uint32_t   pulse_ns  = 0u;
    uint8_t    chain_id  = 0u;

    /* Every v0.5 host helper must reject an uninitialised ctx with
     * NOT_READY -- the standard chip-driver lifecycle contract.
     * Real wire dispatch only happens after gd32g553_init() flips
     * ctx.initialised; until then the helpers short-circuit. */
    zassert_equal(gd32g553_pwm_capture_begin(&ctx, 0u, 0u), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_pwm_capture_read(&ctx, 0u, &period_ns, &pulse_ns), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_pwm_capture_end(&ctx, 0u), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_pwm_single_pulse(&ctx, 0u, 1000u), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_timer_sync(&ctx, 0u, 1u, 0u), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_power_mode_set(&ctx, 1u, 0u, 0u), ALP_ERR_NOT_READY);
    /* §2B wave-2 chunked DSP-chain upload helpers honour the same
     * NOT_READY contract -- chain_open, stage_push, chain_bind all
     * short-circuit before serialising the wire envelope. */
    zassert_equal(gd32g553_adc_dsp_chain_open(&ctx, &chain_id), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 0u, NULL, 0u), ALP_ERR_NOT_READY);
    zassert_equal(gd32g553_adc_dsp_chain_bind(&ctx, 0u, 0u), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_gd32g553_v05_invalid_args)
{
    gd32g553_t ctx = { .initialised = true };

    /* pwm_capture_begin rejects edge > 2 (i.e. outside RISING /
     * FALLING / BOTH). */
    zassert_equal(gd32g553_pwm_capture_begin(&ctx, 0u, 3u), ALP_ERR_INVAL);
    zassert_equal(gd32g553_pwm_capture_begin(&ctx, 0u, 99u), ALP_ERR_INVAL);

    /* pwm_capture_read rejects both NULL out-params (caller wants
     * something out of the call). */
    zassert_equal(gd32g553_pwm_capture_read(&ctx, 0u, NULL, NULL), ALP_ERR_INVAL);

    /* pwm_single_pulse rejects pulse_ns == 0 (zero-width pulse
     * is meaningless and likely caller error). */
    zassert_equal(gd32g553_pwm_single_pulse(&ctx, 0u, 0u), ALP_ERR_INVAL);

    /* power_mode_set rejects mode > 3 (outside RUN / SLEEP /
     * DEEP_SLEEP / STANDBY). */
    zassert_equal(gd32g553_power_mode_set(&ctx, 4u, 0u, 0u), ALP_ERR_INVAL);
    zassert_equal(gd32g553_power_mode_set(&ctx, 99u, 0u, 0u), ALP_ERR_INVAL);

    /* §2B wave-2 DSP-chain helpers reject malformed args before they
     * hit cmd_send.  Each constraint mirrors the firmware-side
     * decoder's expectations so callers don't waste a wire trip to
     * surface obvious typos. */
    /* chain_open rejects NULL out-param (caller can't observe the
     * assigned chain_id without it). */
    zassert_equal(gd32g553_adc_dsp_chain_open(&ctx, NULL), ALP_ERR_INVAL);

    /* stage_push rejects stage_index outside [0, MAX_STAGES). */
    zassert_equal(
        gd32g553_adc_dsp_stage_push(&ctx, 0u, GD32G553_BRIDGE_ADC_DSP_MAX_STAGES, 0u, NULL, 0u),
        ALP_ERR_INVAL);
    zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 99u, 0u, NULL, 0u), ALP_ERR_INVAL);
    /* stage_push rejects kind > 3 (outside FIR/IIR/WINDOW/FFT). */
    zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 4u, NULL, 0u), ALP_ERR_INVAL);
    zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 99u, NULL, 0u), ALP_ERR_INVAL);
    /* stage_push rejects NULL params when len > 0 (caller asked to
     * upload bytes from a NULL pointer). */
    zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 0u, NULL, 4u), ALP_ERR_INVAL);
    /* stage_push rejects oversized payload (would overrun the
     * firmware's per-stage buffer). */
    {
        static const uint8_t oversized[GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES + 4u] = { 0 };
        zassert_equal(
            gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 0u, oversized, (uint16_t)sizeof(oversized)),
            ALP_ERR_OUT_OF_RANGE);
    }

    /* chain_bind rejects stream_id outside [0, ADC_STREAM_COUNT). */
    zassert_equal(gd32g553_adc_dsp_chain_bind(&ctx, 0u, GD32G553_BRIDGE_ADC_STREAM_COUNT),
                  ALP_ERR_INVAL);
    zassert_equal(gd32g553_adc_dsp_chain_bind(&ctx, 0u, 99u), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.AI batch -- NULL-arg guard smokes                          */
/*                                                                    */
/* Pattern: each new chip's init/post-init API rejects NULL ctx /     */
/* NULL bus / zero-address.  The functional verification of these     */
/* drivers happens against real silicon under the HIL suite once the  */
/* matching board boards land -- these smokes only confirm the      */
/* defensive argument-checking layer compiles + behaves uniformly.    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ov2640_init_null_args)
{
    ov2640_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(ov2640_init(NULL, bus, OV2640_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov2640_init(&dev, NULL, OV2640_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov2640_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov5645_init_null_args)
{
    ov5645_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(ov5645_init(NULL, bus, OV5645_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov5645_init(&dev, NULL, OV5645_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov5645_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov7670_init_null_args)
{
    ov7670_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(ov7670_init(NULL, bus, OV7670_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov7670_init(&dev, NULL, OV7670_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov7670_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov9281_init_null_args)
{
    ov9281_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(ov9281_init(NULL, bus, OV9281_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(ov9281_init(&dev, NULL, OV9281_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(ov9281_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ar0234_init_null_args)
{
    ar0234_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(ar0234_init(NULL, bus, AR0234_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(ar0234_init(&dev, NULL, AR0234_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(ar0234_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_imx219_init_null_args)
{
    imx219_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(imx219_init(NULL, bus, IMX219_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(imx219_init(&dev, NULL, IMX219_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(imx219_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_imx477_init_null_args)
{
    imx477_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(imx477_init(NULL, bus, IMX477_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(imx477_init(&dev, NULL, IMX477_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(imx477_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_gc2145_init_null_args)
{
    gc2145_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(gc2145_init(NULL, bus, GC2145_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(gc2145_init(&dev, NULL, GC2145_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(gc2145_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ti_ds90ub_init_null_args)
{
    ti_ds90ub_t dev;
    alp_i2c_t  *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(ti_ds90ub_init(NULL, bus, DS90UB953_I2C_ADDR_DEFAULT, DS90UB954_I2C_ADDR_DEFAULT),
                  ALP_ERR_INVAL);
    zassert_equal(ti_ds90ub_init(&dev, NULL, DS90UB953_I2C_ADDR_DEFAULT, DS90UB954_I2C_ADDR_DEFAULT),
                  ALP_ERR_INVAL);
    zassert_equal(ti_ds90ub_init(&dev, bus, 0, DS90UB954_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(ti_ds90ub_init(&dev, bus, DS90UB953_I2C_ADDR_DEFAULT, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_maxim_gmsl2_init_null_args)
{
    maxim_gmsl2_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(maxim_gmsl2_init(NULL, bus, MAX9295_I2C_ADDR_DEFAULT, MAX9296_I2C_ADDR_DEFAULT),
                  ALP_ERR_INVAL);
    zassert_equal(maxim_gmsl2_init(&dev, NULL, MAX9295_I2C_ADDR_DEFAULT, MAX9296_I2C_ADDR_DEFAULT),
                  ALP_ERR_INVAL);
    zassert_equal(maxim_gmsl2_init(&dev, bus, 0, MAX9296_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(maxim_gmsl2_init(&dev, bus, MAX9295_I2C_ADDR_DEFAULT, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_st7789_init_null_args)
{
    st7789_t dev;
    /* NULL ctx / NULL spi / NULL dc -- INVAL.  Real bus opens are
     * not required at this layer; the driver rejects NULL handles
     * before touching them. */
    zassert_equal(st7789_init(NULL, NULL, NULL, NULL, 240, 320), ALP_ERR_INVAL);
    zassert_equal(st7789_init(&dev, NULL, NULL, NULL, 240, 320), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ili9341_init_null_args)
{
    ili9341_t dev;
    zassert_equal(ili9341_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(ili9341_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ili9488_init_null_args)
{
    ili9488_t dev;
    zassert_equal(ili9488_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(ili9488_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ra8875_init_null_args)
{
    ra8875_t dev;
    zassert_equal(ra8875_init(NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(ra8875_init(&dev, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_sh1106_init_null_args)
{
    sh1106_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(sh1106_init(NULL, bus, SH1106_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(sh1106_init(&dev, NULL, SH1106_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(sh1106_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_sh1106_draw_pixel_clips_oob)
{
    sh1106_t dev = { 0 };
    /* Pre-init draw_pixel is silently no-op (no crash, no fb write). */
    sh1106_draw_pixel(&dev, SH1106_WIDTH + 5, 0, true);
    sh1106_draw_pixel(&dev, 0, SH1106_HEIGHT + 5, true);
    sh1106_draw_pixel(NULL, 10, 10, true);
    for (size_t i = 0; i < sizeof(dev.fb); i++) {
        zassert_equal(dev.fb[i], 0u, "fb[%zu] = 0x%02x; OOB write must not corrupt fb", i,
                      dev.fb[i]);
    }
}

ZTEST(alp_chips, test_il3820_init_null_args)
{
    il3820_t dev;
    zassert_equal(il3820_init(NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(il3820_init(&dev, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gdew0154t8_init_null_args)
{
    gdew0154t8_t dev;
    zassert_equal(gdew0154t8_init(NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(gdew0154t8_init(&dev, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_hailo_8l_init_null_args)
{
    hailo_8l_t dev;
    zassert_equal(hailo_8l_init(NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(hailo_8l_init(&dev, NULL, NULL), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.industrial batch -- NULL-arg guard smokes                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bmp390_init_null_args)
{
    bmp390_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(bmp390_init(NULL, bus, BMP390_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
    zassert_equal(bmp390_init(&dev, NULL, BMP390_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
    zassert_equal(bmp390_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ms5611_init_null_args)
{
    ms5611_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(ms5611_init(NULL, bus, MS5611_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
    zassert_equal(ms5611_init(&dev, NULL, MS5611_I2C_ADDR_PRIMARY), ALP_ERR_INVAL);
    zassert_equal(ms5611_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lps22hb_init_null_args)
{
    lps22hb_t  dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(lps22hb_init(NULL, bus, LPS22HB_I2C_ADDR_HIGH), ALP_ERR_INVAL);
    zassert_equal(lps22hb_init(&dev, NULL, LPS22HB_I2C_ADDR_HIGH), ALP_ERR_INVAL);
    zassert_equal(lps22hb_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_vl53l1x_init_null_args)
{
    vl53l1x_t  dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(vl53l1x_init(NULL, bus, VL53L1X_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(vl53l1x_init(&dev, NULL, VL53L1X_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(vl53l1x_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_vl53l5cx_init_null_args)
{
    vl53l5cx_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(vl53l5cx_init(NULL, bus, VL53L5CX_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(vl53l5cx_init(&dev, NULL, VL53L5CX_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(vl53l5cx_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_a02yyuw_init_null_args)
{
    a02yyuw_t dev;
    zassert_equal(a02yyuw_init(NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(a02yyuw_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_drv8833_init_null_args)
{
    drv8833_t dev;
    zassert_equal(drv8833_init(NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(drv8833_init(&dev, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_drv8825_init_null_args)
{
    drv8825_t dev;
    zassert_equal(drv8825_init(NULL, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(drv8825_init(&dev, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_tmc2209_init_null_args)
{
    tmc2209_t dev;
    zassert_equal(tmc2209_init(NULL, NULL, 0), ALP_ERR_INVAL);
    zassert_equal(tmc2209_init(&dev, NULL, 0), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_a4988_init_null_args)
{
    a4988_t dev;
    zassert_equal(a4988_init(NULL, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(a4988_init(&dev, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_as5048b_init_null_args)
{
    as5048b_t  dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(as5048b_init(NULL, bus, AS5048B_I2C_ADDR_BASE), ALP_ERR_INVAL);
    zassert_equal(as5048b_init(&dev, NULL, AS5048B_I2C_ADDR_BASE), ALP_ERR_INVAL);
    zassert_equal(as5048b_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_mt6701_init_null_args)
{
    mt6701_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(mt6701_init(NULL, bus, MT6701_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(mt6701_init(&dev, NULL, MT6701_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(mt6701_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_hx711_init_null_args)
{
    hx711_t dev;
    zassert_equal(hx711_init(NULL, NULL, NULL, HX711_GAIN_128), ALP_ERR_INVAL);
    zassert_equal(hx711_init(&dev, NULL, NULL, HX711_GAIN_128), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_max31855_init_null_args)
{
    max31855_t dev;
    zassert_equal(max31855_init(NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(max31855_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_max31865_init_null_args)
{
    max31865_t dev;
    zassert_equal(max31865_init(NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(max31865_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_tsl2591_init_null_args)
{
    tsl2591_t  dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(tsl2591_init(NULL, bus, TSL2591_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(tsl2591_init(&dev, NULL, TSL2591_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(tsl2591_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_qmc5883l_init_null_args)
{
    qmc5883l_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(qmc5883l_init(NULL, bus, QMC5883L_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(qmc5883l_init(&dev, NULL, QMC5883L_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(qmc5883l_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_veml7700_init_null_args)
{
    veml7700_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(veml7700_init(NULL, bus, VEML7700_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(veml7700_init(&dev, NULL, VEML7700_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(veml7700_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.iot batch -- NULL-arg guard smokes                         */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_quectel_bg95_init_null_args)
{
    quectel_bg95_t dev;
    zassert_equal(quectel_bg95_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(quectel_bg95_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_quectel_bg77_init_null_args)
{
    quectel_bg77_t dev;
    zassert_equal(quectel_bg77_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(quectel_bg77_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ublox_sara_r5_init_null_args)
{
    ublox_sara_r5_t dev;
    zassert_equal(ublox_sara_r5_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(ublox_sara_r5_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_semtech_sx1262_init_null_args)
{
    semtech_sx1262_t dev;
    zassert_equal(semtech_sx1262_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(semtech_sx1262_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_semtech_sx1276_init_null_args)
{
    semtech_sx1276_t dev;
    zassert_equal(semtech_sx1276_init(NULL, NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(semtech_sx1276_init(&dev, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ublox_neo_m9n_init_null_args)
{
    ublox_neo_m9n_t dev;
    zassert_equal(ublox_neo_m9n_init(NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(ublox_neo_m9n_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ublox_max_m10s_init_null_args)
{
    ublox_max_m10s_t dev;
    zassert_equal(ublox_max_m10s_init(NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(ublox_max_m10s_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_atgm336h_init_null_args)
{
    atgm336h_t dev;
    zassert_equal(atgm336h_init(NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(atgm336h_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_atecc608b_init_null_args)
{
    atecc608b_t dev;
    alp_i2c_t  *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(atecc608b_init(NULL, bus, ATECC608B_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(atecc608b_init(&dev, NULL, ATECC608B_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
    zassert_equal(atecc608b_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.audio batch -- NULL-arg guard smokes                       */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ics_43434_init_null_args)
{
    ics_43434_t dev;
    zassert_equal(ics_43434_init(NULL, ICS_43434_CH_LEFT), ALP_ERR_INVAL);
    zassert_equal(ics_43434_init(&dev, (ics_43434_channel_t)99), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_inmp441_init_null_args)
{
    inmp441_t dev;
    zassert_equal(inmp441_init(NULL, INMP441_CH_LEFT), ALP_ERR_INVAL);
    zassert_equal(inmp441_init(&dev, (inmp441_channel_t)99), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_wm8960_init_null_args)
{
    wm8960_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(wm8960_init(NULL, bus, WM8960_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(wm8960_init(&dev, NULL, WM8960_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(wm8960_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tlv320aic3204_init_null_args)
{
    tlv320aic3204_t dev;
    alp_i2c_t      *bus =
        alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(tlv320aic3204_init(NULL, bus, TLV320AIC3204_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(tlv320aic3204_init(&dev, NULL, TLV320AIC3204_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(tlv320aic3204_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_max98357a_init_null_args)
{
    max98357a_t dev;
    zassert_equal(max98357a_init(NULL, NULL), ALP_ERR_INVAL);
    zassert_equal(max98357a_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_es8388_init_null_args)
{
    es8388_t   dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = E1M_I2C0, .bitrate_hz = 400000 });
    zassert_not_null(bus);
    zassert_equal(es8388_init(NULL, bus, ES8388_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(es8388_init(&dev, NULL, ES8388_I2C_ADDR_LOW), ALP_ERR_INVAL);
    zassert_equal(es8388_init(&dev, bus, 0), ALP_ERR_INVAL);
    alp_i2c_close(bus);
}
