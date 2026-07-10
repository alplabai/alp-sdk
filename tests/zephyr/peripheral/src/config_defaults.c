/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ALP_*_CONFIG_DEFAULT(id) contract (#610 WS1): each macro fills the
 * identity field from its argument and every other field with the one
 * canonical default documented in the public header. These tests pin
 * those defaults so a header edit that silently changes a default is
 * caught. They assert VALUES, not hardware behavior -- the macros are
 * header-only, so this runs on native_sim with no backend.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/dac.h"
#include "alp/wdt.h"
#include "alp/counter.h"
#include "alp/can.h"
#include "alp/adc.h"
#include "alp/audio.h"
#include "alp/ble.h"
#include "alp/camera.h"
#include "alp/display.h"
#include "alp/i2s.h"
#include "alp/inference.h"
#include "alp/mproc.h"
#include "alp/iot.h"
#include "alp/rpc.h"
#include "alp/storage.h"
#include "alp/usb.h"

ZTEST(alp_peripheral, test_i2c_config_default)
{
	alp_i2c_config_t cfg = ALP_I2C_CONFIG_DEFAULT(5u);
	zassert_equal(cfg.bus_id, 5u, "identity from the macro arg");
	zassert_equal(cfg.bitrate_hz, 100000u, "I2C standard-mode default");
}

ZTEST(alp_peripheral, test_spi_config_default)
{
	alp_spi_config_t cfg = ALP_SPI_CONFIG_DEFAULT(2u);
	zassert_equal(cfg.bus_id, 2u, NULL);
	zassert_equal(cfg.freq_hz, 1000000u, "1 MHz default");
	zassert_equal(cfg.mode, ALP_SPI_MODE_0, "mode 0 default");
	zassert_equal(cfg.bits_per_word, 8u, NULL);
	zassert_equal(cfg.cs_pin_id, ALP_SPI_NO_CS, "no controller-driven CS by default");
}

ZTEST(alp_peripheral, test_uart_config_default)
{
	alp_uart_config_t cfg = ALP_UART_CONFIG_DEFAULT(3u);
	zassert_equal(cfg.port_id, 3u, NULL);
	zassert_equal(cfg.baudrate, 115200u, "115200 8N1");
	zassert_equal(cfg.data_bits, 8u, NULL);
	zassert_equal(cfg.stop_bits, 1u, NULL);
	zassert_equal(cfg.parity, ALP_UART_PARITY_NONE, NULL);
}

ZTEST(alp_peripheral, test_pwm_config_default)
{
	alp_pwm_config_t cfg = ALP_PWM_CONFIG_DEFAULT(7u);
	zassert_equal(cfg.channel_id, 7u, NULL);
	zassert_equal(cfg.period_ns, 0u, "0 = devicetree default period");
	zassert_equal(cfg.polarity, ALP_PWM_POLARITY_NORMAL, NULL);
}

ZTEST(alp_peripheral, test_dac_config_default)
{
	alp_dac_config_t cfg = ALP_DAC_CONFIG_DEFAULT(1u);
	zassert_equal(cfg.channel_id, 1u, NULL);
	zassert_equal(cfg.initial_mv, 0u, "starts at ground");
}

ZTEST(alp_peripheral, test_wdt_config_default)
{
	alp_wdt_config_t cfg = ALP_WDT_CONFIG_DEFAULT(0u);
	zassert_equal(cfg.wdt_id, 0u, NULL);
	zassert_equal(cfg.timeout_ms, 1000u, "non-zero deadline (zero-init is invalid for wdt)");
	zassert_equal(cfg.on_timeout, ALP_WDT_RESET_SOC, "safest action default");
}

ZTEST(alp_peripheral, test_counter_config_default)
{
	alp_counter_config_t cfg = ALP_COUNTER_CONFIG_DEFAULT(2u);
	zassert_equal(cfg.counter_id, 2u, "identity-only config");
}

ZTEST(alp_peripheral, test_can_config_default)
{
	alp_can_config_t cfg = ALP_CAN_CONFIG_DEFAULT(4u);
	zassert_equal(cfg.bus_id, 4u, NULL);
	zassert_equal(cfg.bitrate_nominal_hz, 500000u, "500k classic default");
	zassert_equal(cfg.bitrate_data_hz, 0u, "classic = no data-phase rate");
	zassert_equal(cfg.mode, ALP_CAN_MODE_CLASSIC, NULL);
	zassert_false(cfg.loopback, "on the wire, not local self-test");
}

/* -------------------------------------------------------------------- */
/* #610 WS2: remaining peripheral/subsystem configs                     */
/* -------------------------------------------------------------------- */

ZTEST(alp_peripheral, test_adc_config_default)
{
	alp_adc_config_t cfg = ALP_ADC_CONFIG_DEFAULT(6u);
	zassert_equal(cfg.channel_id, 6u, NULL);
	zassert_equal(cfg.resolution_bits, 0u, "0 = use DT default");
	zassert_equal(cfg.acquisition_us, 0u, "0 = use backend default");
	zassert_equal(cfg.reference, ALP_ADC_REF_INTERNAL, "on-die bandgap default");
	zassert_equal(cfg.gain_num, 1u, "unity gain 1/1");
	zassert_equal(cfg.gain_den, 1u, "unity gain 1/1");
	zassert_equal(cfg.oversampling_ratio, 0u, "0 = backend default");
	zassert_equal(cfg.sample_cycles, 0u, "0 = backend default");
}

ZTEST(alp_peripheral, test_adc_stream_config_default)
{
	alp_adc_stream_config_t cfg = ALP_ADC_STREAM_CONFIG_DEFAULT(1u);
	zassert_equal(cfg.channel_id, 1u, NULL);
	zassert_equal(cfg.sample_rate_hz, 0u, "0 sentinel -- caller MUST set before open()");
}

ZTEST(alp_peripheral, test_adc_filter_config_default)
{
	alp_adc_filter_config_t cfg = ALP_ADC_FILTER_CONFIG_DEFAULT(2u);
	zassert_equal(cfg.channel_id, 2u, NULL);
	zassert_equal(cfg.sample_rate_hz, 0u, "sentinel -- caller MUST set");
	zassert_is_null(cfg.stages, "sentinel -- caller MUST set the DSP chain");
	zassert_equal(cfg.n_stages, 0u, "sentinel -- caller MUST set");
}

ZTEST(alp_peripheral, test_adc_spectrum_config_default)
{
	alp_adc_spectrum_config_t cfg = ALP_ADC_SPECTRUM_CONFIG_DEFAULT(3u);
	zassert_equal(cfg.channel_id, 3u, NULL);
	zassert_equal(cfg.sample_rate_hz, 0u, "sentinel -- caller MUST set");
	zassert_is_null(cfg.stages, "sentinel -- caller MUST set the DSP chain");
	zassert_equal(cfg.n_stages, 0u, "sentinel -- caller MUST set");
}

ZTEST(alp_peripheral, test_audio_config_default)
{
	alp_audio_config_t cfg = ALP_AUDIO_CONFIG_DEFAULT(0u);
	zassert_equal(cfg.peripheral_id, 0u, NULL);
	zassert_equal(cfg.sample_rate_hz, 16000u, "16 kHz voice-band default");
	zassert_equal(cfg.channels, 1u, "mono default");
	zassert_equal(cfg.format, ALP_AUDIO_FMT_S16_LE, NULL);
	zassert_equal(cfg.frames_per_block, 256u, NULL);
}

ZTEST(alp_peripheral, test_ble_adv_config_default)
{
	alp_ble_adv_config_t cfg = ALP_BLE_ADV_CONFIG_DEFAULT("MyDevice");
	zassert_str_equal(cfg.name, "MyDevice", NULL);
	zassert_is_null(cfg.services, "no service UUIDs by default");
	zassert_equal(cfg.num_services, 0u, NULL);
	zassert_equal(cfg.interval_min_ms, 100u, NULL);
	zassert_equal(cfg.interval_max_ms, 200u, NULL);
	zassert_true(cfg.connectable, NULL);
}

ZTEST(alp_peripheral, test_camera_config_default)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0u);
	zassert_equal(cfg.camera_id, 0u, NULL);
	zassert_equal(cfg.width, 0u, "sentinel -- caller MUST set a resolution");
	zassert_equal(cfg.height, 0u, "sentinel -- caller MUST set a resolution");
	zassert_equal(cfg.fps, 30u, "common video frame rate");
	zassert_equal(cfg.format, ALP_PIXFMT_RGB565, "widely-supported embedded default");
}

ZTEST(alp_peripheral, test_display_config_default)
{
	alp_display_config_t cfg = ALP_DISPLAY_CONFIG_DEFAULT(2u);
	zassert_equal(cfg.display_id, 2u, "identity-only config");
}

ZTEST(alp_peripheral, test_i2c_target_config_default)
{
	alp_i2c_target_config_t cfg = ALP_I2C_TARGET_CONFIG_DEFAULT(5u);
	zassert_equal(cfg.bus_id, 5u, NULL);
	zassert_equal(cfg.own_addr_7bit, 0u, "sentinel in the reserved range -- caller MUST set");
	zassert_is_null(cfg.on_write, "sentinel -- caller MUST set");
	zassert_is_null(cfg.on_read, "sentinel -- caller MUST set");
	zassert_is_null(cfg.on_stop, "optional");
	zassert_is_null(cfg.user, "optional");
}

ZTEST(alp_peripheral, test_spi_target_config_default)
{
	alp_spi_target_config_t cfg = ALP_SPI_TARGET_CONFIG_DEFAULT(2u);
	zassert_equal(cfg.bus_id, 2u, NULL);
	zassert_equal(cfg.mode, ALP_SPI_MODE_0, NULL);
	zassert_equal(cfg.bits_per_word, 0u, "0 = defaults to 8 per the backend");
}

ZTEST(alp_peripheral, test_i2s_config_default)
{
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(1u);
	zassert_equal(cfg.bus_id, 1u, NULL);
	zassert_equal(cfg.sample_rate_hz, 48000u, NULL);
	zassert_equal(cfg.word_bits, 16u, NULL);
	zassert_equal(cfg.channels, 2u, "stereo default");
	zassert_equal(cfg.format, ALP_I2S_FMT_I2S, "most modern codecs accept this");
	zassert_equal(cfg.direction, ALP_I2S_DIR_RX, "passive, listen-only default");
	zassert_equal(cfg.block_frames, 256u, NULL);
}

ZTEST(alp_peripheral, test_inference_config_default)
{
	static const uint8_t   dummy_model[4];
	alp_inference_config_t cfg = ALP_INFERENCE_CONFIG_DEFAULT(dummy_model);
	zassert_equal_ptr(cfg.model_data, dummy_model, NULL);
	zassert_equal(cfg.model_size, 0u, "sentinel -- caller MUST set");
	zassert_equal(cfg.format, ALP_INFERENCE_MODEL_TFLITE, NULL);
	zassert_equal(cfg.backend, ALP_INFERENCE_BACKEND_AUTO, NULL);
	zassert_equal(cfg.arena_bytes, 0u, "documented backend default");
	zassert_is_null(cfg.arena, "documented backend default (use heap)");
}

ZTEST(alp_peripheral, test_mbox_config_default)
{
	alp_mbox_config_t cfg = ALP_MBOX_CONFIG_DEFAULT(3u);
	zassert_equal(cfg.channel, 3u, NULL);
	zassert_equal(cfg.peer, ALP_CORE_SELF, "sentinel -- caller MUST set the real peer");
}

ZTEST(alp_peripheral, test_shmem_config_default)
{
	alp_shmem_config_t cfg = ALP_SHMEM_CONFIG_DEFAULT("alp_shmem0");
	zassert_str_equal(cfg.name, "alp_shmem0", NULL);
	zassert_equal(cfg.size, 0u, "sentinel -- caller MUST set");
	zassert_false(cfg.cacheable, "required for the simple core A/B pattern");
}

ZTEST(alp_peripheral, test_mqtt_config_default)
{
	alp_mqtt_config_t cfg = ALP_MQTT_CONFIG_DEFAULT("mqtt://broker.local:1883");
	zassert_str_equal(cfg.broker_uri, "mqtt://broker.local:1883", NULL);
	zassert_is_null(cfg.client_id, NULL);
	zassert_is_null(cfg.username, NULL);
	zassert_is_null(cfg.password, NULL);
	zassert_equal(cfg.keepalive_s, 60u, "conventional MQTT keepalive");
	zassert_true(cfg.clean_session, NULL);
	zassert_is_null(cfg.tls, "NULL = OS default CA path, no client cert, verify peer");
}

ZTEST(alp_peripheral, test_pwm_capture_config_default)
{
	alp_pwm_capture_config_t cfg = ALP_PWM_CAPTURE_CONFIG_DEFAULT(4u);
	zassert_equal(cfg.channel_id, 4u, NULL);
	zassert_equal(cfg.edge, ALP_PWM_CAPTURE_EDGE_RISING, NULL);
}

ZTEST(alp_peripheral, test_qenc_config_default)
{
	alp_qenc_config_t cfg = ALP_QENC_CONFIG_DEFAULT(1u);
	zassert_equal(cfg.encoder_id, 1u, NULL);
	zassert_equal(cfg.pulses_per_rev, 0u, "informational-only; 0 = unspecified");
}

ZTEST(alp_peripheral, test_rpc_config_default)
{
	alp_rpc_config_t cfg = ALP_RPC_CONFIG_DEFAULT("alp_default_rpmsg");
	zassert_str_equal(cfg.name, "alp_default_rpmsg", NULL);
	zassert_equal(cfg.src_ept, 0u, "0 = FNV-1a hash of name");
	zassert_equal(cfg.dst_ept, 0u, "0 = src_ept + 1");
	zassert_equal(cfg.mbox_ch, ALP_RPC_DEFAULT_MBOX_CH, NULL);
	zassert_false(cfg.cacheable, "v0.6 default");
}

ZTEST(alp_peripheral, test_storage_config_default)
{
	alp_storage_config_t cfg = ALP_STORAGE_CONFIG_DEFAULT(ALP_STORAGE_KIND_QSPI_FLASH);
	zassert_equal(cfg.kind, ALP_STORAGE_KIND_QSPI_FLASH, NULL);
	zassert_equal(cfg.instance_id, 0u, "primary device");
	zassert_equal(cfg.freq_hz, 0u, "0 = backend default");
	zassert_false(cfg.read_only, "common writable case");
}

ZTEST(alp_peripheral, test_usb_device_config_default)
{
	alp_usb_device_config_t cfg = ALP_USB_DEVICE_CONFIG_DEFAULT(ALP_USB_DEVICE_CDC_ACM);
	zassert_equal(cfg.device_class, ALP_USB_DEVICE_CDC_ACM, NULL);
	zassert_equal(cfg.vendor_id, 0x0000u, "placeholder -- caller MUST set before shipping");
	zassert_equal(cfg.product_id, 0x0000u, "placeholder -- caller MUST set before shipping");
	zassert_equal(cfg.bcd_device, 0x0100u, "conventional v1.00 encoding");
	zassert_is_null(cfg.manufacturer, NULL);
	zassert_is_null(cfg.product, NULL);
	zassert_is_null(cfg.serial, NULL);
}
