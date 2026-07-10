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
