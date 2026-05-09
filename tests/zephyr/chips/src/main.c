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

ZTEST_SUITE(alp_chips, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* lsm6dso                                                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_lsm6dso_init_null_args) {
    lsm6dso_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    zassert_equal(lsm6dso_init(NULL, bus, LSM6DSO_I2C_ADDR_LOW),
                  ALP_ERR_INVAL, "NULL ctx must be invalid");
    zassert_equal(lsm6dso_init(&dev, NULL, LSM6DSO_I2C_ADDR_LOW),
                  ALP_ERR_INVAL, "NULL bus must be invalid");
    zassert_equal(lsm6dso_init(&dev, bus, 0),
                  ALP_ERR_INVAL, "addr=0 must be invalid");

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lsm6dso_post_init_calls_reject_uninitialised) {
    /* Without a real chip behind the emul controller, init's
     * WHO_AM_I check will not return 0x6C.  We expect lsm6dso_init
     * to fail; subsequent reads must report NOT_READY.
     *
     * If the WHO_AM_I check returns ALP_OK against the emul (e.g.
     * if a future revision fakes it), the test still passes
     * because reads on a successfully-initialised driver return
     * ALP_OK and the assertion below is conditional. */
    lsm6dso_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    alp_status_t s = lsm6dso_init(&dev, bus, LSM6DSO_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        /* Expected path on a bare emul controller. */
        lsm6dso_axes_t axes;
        zassert_equal(lsm6dso_read_accel(&dev, &axes),
                      ALP_ERR_NOT_READY,
                      "reads on a failed-init driver must be NOT_READY");
    }

    lsm6dso_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* ssd1306                                                             */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ssd1306_init_invalid_geometry) {
    ssd1306_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 96, 48),
                  ALP_ERR_NOSUPPORT, "v0.1 supports only 128x64 or 128x32");
    zassert_equal(ssd1306_init(&dev, bus, SSD1306_I2C_ADDR_LOW, 0, 0),
                  ALP_ERR_NOSUPPORT, "zero-sized geometry must be invalid");

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ssd1306_clear_and_pixel_safe_without_init) {
    ssd1306_t dev = {0};
    /* These two functions must be NULL-safe and tolerate an
     * uninitialised context — they only touch the in-memory
     * framebuffer, not the panel. */
    ssd1306_clear(&dev);
    ssd1306_draw_pixel(&dev, 1000, 1000, true);  /* OOB silently ignored */
    ssd1306_clear(NULL);                          /* NULL is a no-op */
    ssd1306_draw_pixel(NULL, 0, 0, true);
}

ZTEST(alp_chips, test_ssd1306_display_rejects_uninitialised) {
    ssd1306_t dev = {0};
    zassert_equal(ssd1306_display(&dev), ALP_ERR_NOT_READY,
                  "display() on uninitialised driver must be NOT_READY");
}

/* ------------------------------------------------------------------ */
/* button_led                                                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_button_led_init_null_args) {
    alp_button_led_t bl;
    zassert_equal(alp_button_led_init(NULL, NULL),
                  ALP_ERR_INVAL, "all-NULL must be invalid");
    zassert_equal(alp_button_led_init(&bl, NULL),
                  ALP_ERR_INVAL, "NULL cfg must be invalid");
}

ZTEST(alp_chips, test_button_led_init_valid_pair) {
    /* Overlay wires alp,pin-array index 0 → button (pull-up) and
     * index 1 → LED on the chips test's gpio_emul. */
    alp_button_led_t bl;
    alp_status_t s = alp_button_led_init(&bl, &(alp_button_led_config_t){
        .button_pin_id = 0,
        .led_pin_id    = 1,
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

    zassert_equal(alp_button_led_set(&bl, true),  ALP_OK);
    zassert_equal(alp_button_led_set(&bl, false), ALP_OK);
    zassert_equal(alp_button_led_toggle(&bl),     ALP_OK);

    alp_button_led_deinit(&bl);
}

ZTEST(alp_chips, test_button_led_calls_reject_uninitialised) {
    alp_button_led_t bl = {0};
    bool pressed;
    zassert_equal(alp_button_led_is_pressed(&bl, &pressed),
                  ALP_ERR_NOT_READY);
    zassert_equal(alp_button_led_set(&bl, true), ALP_ERR_NOT_READY);
    zassert_equal(alp_button_led_toggle(&bl),    ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* ssd1306 framebuffer logic                                           */
/*                                                                     */
/* These tests exercise the pure pixel-buffer manipulation path —      */
/* no I2C transfers, no emulator fixture.  The framebuffer is part     */
/* of the public struct, so we can drive draw_pixel / clear directly   */
/* and inspect the bytes the panel would receive via display().        */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ssd1306_draw_pixel_sets_correct_bit) {
    /* Geometry filled in manually so init's I2C side-effects don't
     * matter — we're testing framebuffer math. */
    ssd1306_t dev = { .width = 128, .height = 64 };

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

ZTEST(alp_chips, test_ssd1306_draw_pixel_clears_bit) {
    ssd1306_t dev = { .width = 128, .height = 64 };
    ssd1306_draw_pixel(&dev, 5, 3, true);
    zassert_equal(dev.fb[5], 0x08u, "set first");

    ssd1306_draw_pixel(&dev, 5, 3, false);
    zassert_equal(dev.fb[5], 0x00u, "clear should mask the bit");
}

ZTEST(alp_chips, test_ssd1306_draw_pixel_oob_silently_ignored) {
    ssd1306_t dev = { .width = 128, .height = 64 };
    /* These must not write outside the framebuffer. */
    ssd1306_draw_pixel(&dev, 128, 0, true);     /* x at width */
    ssd1306_draw_pixel(&dev, 0, 64, true);      /* y at height */
    ssd1306_draw_pixel(&dev, 9999, 9999, true); /* far OOB */

    for (size_t i = 0; i < sizeof dev.fb; i++) {
        zassert_equal(dev.fb[i], 0u,
                      "fb[%zu] = 0x%02x; OOB writes should be ignored",
                      i, dev.fb[i]);
    }
}

ZTEST(alp_chips, test_ssd1306_clear_wipes_only_fb) {
    ssd1306_t dev = { .width = 128, .height = 64, .addr = 0x3C };
    for (size_t i = 0; i < sizeof dev.fb; i++) dev.fb[i] = 0xAA;

    ssd1306_clear(&dev);

    for (size_t i = 0; i < sizeof dev.fb; i++) {
        zassert_equal(dev.fb[i], 0u, "fb[%zu] not cleared", i);
    }
    /* Other fields preserved. */
    zassert_equal(dev.width,  128u);
    zassert_equal(dev.height, 64u);
    zassert_equal(dev.addr,   0x3Cu);
}

/* ------------------------------------------------------------------ */
/* bme280 (v0.2 chip)                                                  */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_bme280_init_null_args) {
    bme280_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(bme280_init(NULL, bus, BME280_I2C_ADDR_LOW),
                  ALP_ERR_INVAL, "NULL ctx must be invalid");
    zassert_equal(bme280_init(&dev, NULL, BME280_I2C_ADDR_LOW),
                  ALP_ERR_INVAL, "NULL bus must be invalid");
    zassert_equal(bme280_init(&dev, bus, 0),
                  ALP_ERR_INVAL, "addr=0 must be invalid");

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_bme280_post_init_calls_reject_uninitialised) {
    /* No real chip behind the emul controller — CHIP_ID won't be 0x60,
     * so init fails and downstream reads must report NOT_READY. */
    bme280_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    alp_status_t s = bme280_init(&dev, bus, BME280_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        bme280_raw_t raw;
        zassert_equal(bme280_read_raw(&dev, &raw),
                      ALP_ERR_NOT_READY,
                      "read_raw on a failed-init driver must be NOT_READY");
        zassert_equal(bme280_set_sampling(&dev,
                          BME280_OVERSAMPLING_X1, BME280_OVERSAMPLING_X1,
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

ZTEST(alp_chips, test_lis2dw12_init_null_args) {
    lis2dw12_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    zassert_equal(lis2dw12_init(NULL, bus, LIS2DW12_I2C_ADDR_LOW),
                  ALP_ERR_INVAL);
    zassert_equal(lis2dw12_init(&dev, NULL, LIS2DW12_I2C_ADDR_LOW),
                  ALP_ERR_INVAL);
    zassert_equal(lis2dw12_init(&dev, bus, 0),
                  ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_lis2dw12_post_init_calls_reject_uninitialised) {
    lis2dw12_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 100000});
    zassert_not_null(bus);

    alp_status_t s = lis2dw12_init(&dev, bus, LIS2DW12_I2C_ADDR_LOW);
    if (s != ALP_OK) {
        lis2dw12_axes_t axes;
        zassert_equal(lis2dw12_read_accel(&dev, &axes), ALP_ERR_NOT_READY);
        zassert_equal(lis2dw12_set_accel(&dev, LIS2DW12_ODR_50_HZ,
                                         LIS2DW12_FS_2G,
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

ZTEST(alp_chips, test_ssd1331_init_null_args) {
    ssd1331_t dev;
    zassert_equal(ssd1331_init(NULL, NULL, NULL,
                               ssd1331_test_fb, sizeof ssd1331_test_fb),
                  ALP_ERR_INVAL);
    /* Even non-NULL ctx + NULL spi/dc/fb → INVAL. */
    zassert_equal(ssd1331_init(&dev, NULL, NULL,
                               ssd1331_test_fb, sizeof ssd1331_test_fb),
                  ALP_ERR_INVAL);
    /* NULL framebuffer → INVAL. */
    zassert_equal(ssd1331_init(&dev, NULL, NULL, NULL,
                               sizeof ssd1331_test_fb),
                  ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ssd1331_pixel_safe_without_init) {
    ssd1331_t dev = {0};
    dev.fb = ssd1331_test_fb;
    dev.fb_len = sizeof ssd1331_test_fb;

    /* clear/draw_pixel touch only the in-memory framebuffer — safe
     * pre-init.  Clear first so any stale bits from earlier cases
     * are wiped. */
    ssd1331_clear(&dev);
    ssd1331_draw_pixel(&dev, 1000, 1000, 0xFFFFu);  /* OOB silently ignored. */
    ssd1331_draw_pixel(NULL, 0, 0, 0u);             /* NULL ctx is a no-op. */
    ssd1331_clear(NULL);

    /* In-bounds pixel writes the right RGB565 bytes (MSB-first). */
    ssd1331_draw_pixel(&dev, 0, 0, 0xF800u);   /* red */
    zassert_equal(ssd1331_test_fb[0], 0xF8u);
    zassert_equal(ssd1331_test_fb[1], 0x00u);
}

ZTEST(alp_chips, test_ssd1331_display_rejects_uninitialised) {
    ssd1331_t dev = {0};
    zassert_equal(ssd1331_display(&dev), ALP_ERR_NOT_READY);
    zassert_equal(ssd1331_set_display_on(&dev, true), ALP_ERR_NOT_READY);
    zassert_equal(ssd1331_set_master_current(&dev, 0x06), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_ssd1331_rgb565_helper) {
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

ZTEST(alp_chips, test_ov5640_init_null_args) {
    ov5640_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    zassert_equal(ov5640_init(NULL, bus, OV5640_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov5640_init(&dev, NULL, OV5640_I2C_ADDR), ALP_ERR_INVAL);
    zassert_equal(ov5640_init(&dev, bus, 0),                ALP_ERR_INVAL);

    alp_i2c_close(bus);
}

ZTEST(alp_chips, test_ov5640_post_init_calls_reject_uninitialised) {
    ov5640_t dev;
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000});
    zassert_not_null(bus);

    alp_status_t s = ov5640_init(&dev, bus, OV5640_I2C_ADDR);
    if (s != ALP_OK) {
        zassert_equal(ov5640_set_resolution(&dev, OV5640_RES_VGA),
                      ALP_ERR_NOT_READY);
        zassert_equal(ov5640_set_format(&dev, OV5640_FMT_RGB565),
                      ALP_ERR_NOT_READY);
        zassert_equal(ov5640_set_test_pattern(&dev, true),
                      ALP_ERR_NOT_READY);
    }

    ov5640_deinit(&dev);
    alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* pdm_mic (v0.2 helper — surface only; impl returns NOSUPPORT in v0.1) */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_pdm_mic_open_returns_null_in_v01) {
    alp_pdm_mic_t *mic = alp_pdm_mic_open(&(alp_pdm_mic_config_t){
        .peripheral_id  = 0,
        .sample_rate_hz = 16000,
        .channels       = ALP_PDM_MIC_MONO,
        .sample_bits    = 16,
    });
    zassert_is_null(mic, "v0.1 stub must return NULL until v0.2 audio lands");
}

ZTEST(alp_chips, test_pdm_mic_calls_return_nosupport) {
    /* Even with a NULL handle (v0.1 contract), the read/set_gain
     * surface must reply ALP_ERR_NOSUPPORT — the stub asserts the
     * shape, not real arithmetic. */
    int16_t buf[16] = {0};
    size_t n = 999;
    zassert_equal(alp_pdm_mic_read(NULL, buf, sizeof buf / sizeof buf[0],
                                   &n, 0),
                  ALP_ERR_NOSUPPORT);
    zassert_equal(n, 0u, "out_frames must be zeroed by the stub");
    zassert_equal(alp_pdm_mic_set_gain(NULL, 0, 0), ALP_ERR_NOSUPPORT);
    alp_pdm_mic_close(NULL);    /* must not crash. */
}

/* ------------------------------------------------------------------ */
/* All public headers compile cleanly when included together           */
/* ------------------------------------------------------------------ */

#include "alp/peripheral.h"
#include "alp/display.h"
#include "alp/camera.h"
#include "alp/gui.h"
#include "alp/math.h"
#include "alp/signal.h"
#include "alp/iot.h"
#include "alp/audio.h"
#include "alp/ble.h"
#include "alp/security.h"
#include "alp/mproc.h"
#include "alp/e1m_pinout.h"
#include "alp/boards/alp_e1m_evk_aen.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/ssd1306.h"
#include "alp/chips/button_led.h"
#include "alp/chips/bme280.h"
#include "alp/chips/lis2dw12.h"
#include "alp/chips/ssd1331.h"
#include "alp/chips/ov5640.h"
#include "alp/chips/pdm_mic.h"

ZTEST(alp_chips, test_public_headers_co_compile) {
    /* If any of the headers above introduce a typedef/macro
     * collision the translation unit fails to build — getting
     * here at runtime is the success signal. */
    zassert_equal((int)ALP_OK, 0,
                  "ALP_OK must remain 0 across header-set evolution");
    zassert_equal((unsigned)ALP_E1M_GPIO_IO0, 0u);
    zassert_equal((unsigned)EVK_AEN_PIN_LED_RED, ALP_E1M_GPIO_IO0,
                  "EVK feature names must layer atop the global e1m_pinout map");
}

/* ------------------------------------------------------------------ */
/* v0.2 / v0.3 stubbed surfaces — link-cleanliness + NOSUPPORT contract */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_audio_surface_v01_nosupport) {
    zassert_is_null(alp_audio_in_open(NULL),  "v0.1 audio_in stub returns NULL");
    zassert_is_null(alp_audio_out_open(NULL), "v0.1 audio_out stub returns NULL");
    zassert_equal(alp_audio_in_start(NULL),  ALP_ERR_NOSUPPORT);
    zassert_equal(alp_audio_out_start(NULL), ALP_ERR_NOSUPPORT);
    alp_audio_in_close(NULL);
    alp_audio_out_close(NULL);
}

ZTEST(alp_chips, test_ble_surface_v01_nosupport) {
    zassert_is_null(alp_ble_open(), "v0.1 BLE stub returns NULL");
    zassert_equal(alp_ble_advertise_start(NULL, NULL), ALP_ERR_NOSUPPORT);
    zassert_equal(alp_ble_scan_stop(NULL),             ALP_ERR_NOSUPPORT);
    alp_ble_close(NULL);
}

ZTEST(alp_chips, test_security_surface_v01_nosupport) {
    zassert_is_null(alp_hash_open(ALP_HASH_SHA256));
    zassert_is_null(alp_aead_open(ALP_AEAD_AES_128_GCM, NULL, 0));
    uint8_t buf[16];
    zassert_equal(alp_random_bytes(buf, sizeof buf), ALP_ERR_NOSUPPORT);
    alp_hash_close(NULL);
    alp_aead_close(NULL);
}

ZTEST(alp_chips, test_mproc_surface_v01_nosupport) {
    zassert_is_null(alp_shmem_open(NULL));
    zassert_is_null(alp_mbox_open(NULL));
    zassert_is_null(alp_hwsem_open(0));
    zassert_equal(alp_hwsem_try_lock(NULL), ALP_ERR_NOSUPPORT);
    zassert_equal(alp_mbox_send(NULL, NULL, 0, 0), ALP_ERR_NOSUPPORT);
    alp_shmem_close(NULL);
    alp_mbox_close(NULL);
    alp_hwsem_close(NULL);
}
