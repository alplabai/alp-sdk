/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable-API conformance suite -- the "vendor N+1" gate.
 *
 * One data-driven ztest suite that every backend (Zephyr, Yocto,
 * baremetal, a new vendor port) must pass.  It exercises the uniform
 * lifecycle contract that <alp/peripheral.h> and its sibling class
 * headers document for EVERY portable peripheral class:
 *
 *   - alp_<class>_open(NULL cfg)   -> NULL + alp_last_error() == ALP_ERR_INVAL
 *   - alp_<class>_open(bad id)     -> NULL + a documented alp_status_t error
 *   - alp_<class>_close(NULL)      -> no crash (idempotent)
 *   - alp_<class>_capabilities(NULL) -> NULL (documented graceful return)
 *   - alp_<class>_capabilities(h)  -> non-NULL for an open handle
 *   - every surfaced error code is a member of alp_status_t
 *     (no raw negative errno leaking through the portable surface)
 *   - double close is safe (dispatchers gate on the pool's in_use flag)
 *   - a representative call on a NULL handle refuses with an in-enum
 *     status (ALP_ERR_NOT_READY across today's dispatchers)
 *
 * TEST MATRIX (classes x cases)
 * =============================
 *
 *   Case key:
 *     A  open(NULL cfg) -> NULL + INVAL       E  open(valid) contract (see below)
 *     B  open(bad instance) -> NULL + error   F  capabilities(open handle) non-NULL
 *     C  close(NULL) idempotent               G  double-close safe
 *     D  capabilities(NULL) -> NULL           H  NULL-handle call -> in-enum refusal
 *
 *   class    | A  | B | C | D  | E        | F  | G | H
 *   ---------+----+---+---+----+----------+----+---+---
 *   gpio     | -- | x | x | x  | must-open| x  | x | x    (open takes a pin id, not a cfg pointer)
 *   i2c      | x  | x | x | x  | must-open| x  | x | x
 *   spi      | x  | x | x | x  | must-open| x  | x | x
 *   uart     | x  | x | x | x  | must-open| x  | x | x
 *   adc      | x  | x | x | x  | degrade  | x  | x | x
 *   dac      | x  | x | x | x  | degrade  | x  | x | x
 *   pwm      | x  | x | x | x  | degrade  | x  | x | x
 *   can      | x  | x | x | x  | degrade  | x  | x | x
 *   rtc      | -- | x | x | x  | degrade  | x  | x | x    (open takes an rtc id, not a cfg pointer)
 *   wdt      | x  | x | x | x  | degrade  | x  | x | x
 *   counter  | x  | x | x | x  | degrade  | x  | x | x
 *   qenc     | x  | x | x | x  | degrade  | x  | x | x
 *   i2s      | x  | x | x | x  | degrade  | x  | x | x
 *
 *   "must-open": native_sim's emulated controllers (gpio_emul /
 *   i2c_emul / spi_emul / uart0) back instance 0, so open MUST
 *   succeed and the positive-path cases (F, G) run for real.
 *
 *   "degrade": native_sim has no controller for the class, so
 *   open(valid cfg) is REQUIRED to fail the documented way -- NULL
 *   plus an alp_status_t member in alp_last_error().  The case never
 *   silently skips: the refusal itself is the asserted contract, and
 *   the degrade path is logged via TC_PRINT so a twister log shows
 *   exactly which classes ran positive vs. failure-contract paths.
 *   On a real SoM build where the controller exists, the same table
 *   row automatically upgrades to the positive path (open succeeds
 *   -> F and G run against real hardware).
 *
 * ADDING VENDOR N+1 (or a new class): add ONE conf_class_t row to
 * conf_classes[] below -- three tiny open adapters, the class's
 * close / capabilities functions, and a representative NULL-handle
 * call.  The generic runners pick the row up automatically; no new
 * ZTEST bodies are needed.
 *
 */

#include <zephyr/ztest.h>

#include "alp/adc.h"
#include "alp/can.h"
#include "alp/counter.h"
#include "alp/dac.h"
#include "alp/i2s.h"
#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/rtc.h"
#include "alp/wdt.h"

ZTEST_SUITE(alp_conformance, NULL, NULL, NULL, NULL, NULL);

/* Out-of-range instance id used by case B.  Every portable class
 * documents an instance ceiling far below this. */
#define CONF_BAD_INSTANCE 99u

/* ------------------------------------------------------------------ */
/* Class descriptor + generic expectations                             */
/* ------------------------------------------------------------------ */

/** Uniform-shape adapter: run one class's open() variant. */
typedef void *(*conf_open_fn_t)(void);
/** Uniform-shape adapter: release a handle (must accept NULL). */
typedef void (*conf_close_fn_t)(void *h);
/** Uniform-shape adapter: query instance capabilities. */
typedef const alp_capabilities_t *(*conf_caps_fn_t)(const void *h);
/** Uniform-shape adapter: one representative API call on a NULL handle. */
typedef alp_status_t (*conf_null_call_fn_t)(void);

/**
 * One row per portable peripheral class.  THE deliverable of this
 * suite: porting a new backend means making every row pass, and
 * covering a new class means adding one row.
 */
typedef struct {
	/** Class name for failure messages / degrade logs. */
	const char *name;
	/** Calls alp_<class>_open(NULL).  NULL for classes whose open()
	 *  takes a plain instance id instead of a config pointer
	 *  (gpio, rtc) -- case A does not apply to them. */
	conf_open_fn_t open_null_cfg;
	/** Calls open() with instance CONF_BAD_INSTANCE and otherwise
	 *  sane parameters. */
	conf_open_fn_t open_invalid;
	/** Calls open() with instance 0 and a config every backend
	 *  documents as valid. */
	conf_open_fn_t open_valid;
	/** alp_<class>_close wrapper. */
	conf_close_fn_t close;
	/** alp_<class>_capabilities wrapper; NULL ("skip-marked") when
	 *  the class has no capabilities query yet. */
	conf_caps_fn_t capabilities;
	/** Representative data-path call on a NULL handle; must refuse
	 *  with an in-enum, non-OK status. */
	conf_null_call_fn_t null_handle_call;
	/** true when native_sim's overlay backs instance 0, making the
	 *  positive open path mandatory on this platform. */
	bool must_open;
} conf_class_t;

/** @return true when @p s is a member of the alp_status_t enum. */
static bool conf_status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_ERR_NOT_PROVISIONED;
}

/* ------------------------------------------------------------------ */
/* Per-class adapters (three lines of glue per class)                  */
/* ------------------------------------------------------------------ */

/* GPIO -- id-based open, no config struct (case A n/a). */
static void *gpio_open_invalid_(void)
{
	return alp_gpio_open(CONF_BAD_INSTANCE);
}
static void *gpio_open_valid_(void)
{
	return alp_gpio_open(0);
}
static void gpio_close_(void *h)
{
	alp_gpio_close(h);
}
static const alp_capabilities_t *gpio_caps_(const void *h)
{
	return alp_gpio_capabilities(h);
}
static alp_status_t gpio_null_call_(void)
{
	return alp_gpio_write(NULL, true);
}

/* I2C */
static void *i2c_open_null_(void)
{
	return alp_i2c_open(NULL);
}
static void *i2c_open_invalid_(void)
{
	return alp_i2c_open(&(alp_i2c_config_t){ .bus_id = CONF_BAD_INSTANCE, .bitrate_hz = 100000 });
}
static void *i2c_open_valid_(void)
{
	return alp_i2c_open(&(alp_i2c_config_t){ .bus_id = 0, .bitrate_hz = 100000 });
}
static void i2c_close_(void *h)
{
	alp_i2c_close(h);
}
static const alp_capabilities_t *i2c_caps_(const void *h)
{
	return alp_i2c_capabilities(h);
}
static alp_status_t i2c_null_call_(void)
{
	return alp_i2c_write(NULL, 0x42, (uint8_t[]){ 0xAA }, 1);
}

/* SPI */
static void *spi_open_null_(void)
{
	return alp_spi_open(NULL);
}
static void *spi_open_invalid_(void)
{
	return alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = CONF_BAD_INSTANCE,
	    .freq_hz       = 1000000,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	    .cs_pin_id     = ALP_SPI_NO_CS,
	});
}
static void *spi_open_valid_(void)
{
	return alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 0,
	    .freq_hz       = 1000000,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	    .cs_pin_id     = ALP_SPI_NO_CS,
	});
}
static void spi_close_(void *h)
{
	alp_spi_close(h);
}
static const alp_capabilities_t *spi_caps_(const void *h)
{
	return alp_spi_capabilities(h);
}
static alp_status_t spi_null_call_(void)
{
	return alp_spi_write(NULL, (uint8_t[]){ 0xAA }, 1);
}

/* UART */
static void *uart_open_null_(void)
{
	return alp_uart_open(NULL);
}
static void *uart_open_invalid_(void)
{
	return alp_uart_open(&(alp_uart_config_t){
	    .port_id   = CONF_BAD_INSTANCE,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
}
static void *uart_open_valid_(void)
{
	return alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
}
static void uart_close_(void *h)
{
	alp_uart_close(h);
}
static const alp_capabilities_t *uart_caps_(const void *h)
{
	return alp_uart_capabilities(h);
}
static alp_status_t uart_null_call_(void)
{
	return alp_uart_write(NULL, (uint8_t[]){ 0xAA }, 1);
}

/* ADC */
static void *adc_open_null_(void)
{
	return alp_adc_open(NULL);
}
static void *adc_open_invalid_(void)
{
	return alp_adc_open(&(alp_adc_config_t){ .channel_id = CONF_BAD_INSTANCE });
}
static void *adc_open_valid_(void)
{
	return alp_adc_open(&(alp_adc_config_t){ .channel_id = 0 });
}
static void adc_close_(void *h)
{
	alp_adc_close(h);
}
static const alp_capabilities_t *adc_caps_(const void *h)
{
	return alp_adc_capabilities(h);
}
static alp_status_t adc_null_call_(void)
{
	int32_t raw = 0;
	return alp_adc_read_raw(NULL, &raw);
}

/* DAC */
static void *dac_open_null_(void)
{
	return alp_dac_open(NULL);
}
static void *dac_open_invalid_(void)
{
	return alp_dac_open(&(alp_dac_config_t){ .channel_id = CONF_BAD_INSTANCE });
}
static void *dac_open_valid_(void)
{
	return alp_dac_open(&(alp_dac_config_t){ .channel_id = 0, .initial_mv = 0 });
}
static void dac_close_(void *h)
{
	alp_dac_close(h);
}
static alp_status_t dac_null_call_(void)
{
	return alp_dac_write_mv(NULL, 0);
}
static const alp_capabilities_t *dac_caps_(const void *h)
{
	return alp_dac_capabilities(h);
}

/* PWM */
static void *pwm_open_null_(void)
{
	return alp_pwm_open(NULL);
}
static void *pwm_open_invalid_(void)
{
	return alp_pwm_open(&(alp_pwm_config_t){
	    .channel_id = CONF_BAD_INSTANCE,
	    .period_ns  = 1000000,
	});
}
static void *pwm_open_valid_(void)
{
	return alp_pwm_open(&(alp_pwm_config_t){
	    .channel_id = 0,
	    .period_ns  = 1000000,
	});
}
static void pwm_close_(void *h)
{
	alp_pwm_close(h);
}
static const alp_capabilities_t *pwm_caps_(const void *h)
{
	return alp_pwm_capabilities(h);
}
static alp_status_t pwm_null_call_(void)
{
	return alp_pwm_set_duty(NULL, 0);
}

/* CAN */
static void *can_open_null_(void)
{
	return alp_can_open(NULL);
}
static void *can_open_invalid_(void)
{
	return alp_can_open(&(alp_can_config_t){
	    .bus_id             = CONF_BAD_INSTANCE,
	    .bitrate_nominal_hz = 500000,
	    .mode               = ALP_CAN_MODE_CLASSIC,
	});
}
static void *can_open_valid_(void)
{
	return alp_can_open(&(alp_can_config_t){
	    .bus_id             = 0,
	    .bitrate_nominal_hz = 500000,
	    .mode               = ALP_CAN_MODE_CLASSIC,
	});
}
static void can_close_(void *h)
{
	alp_can_close(h);
}
static const alp_capabilities_t *can_caps_(const void *h)
{
	return alp_can_capabilities(h);
}
static alp_status_t can_null_call_(void)
{
	return alp_can_start(NULL);
}

/* RTC -- id-based open, no config struct (case A n/a). */
static void *rtc_open_invalid_(void)
{
	return alp_rtc_open(CONF_BAD_INSTANCE);
}
static void *rtc_open_valid_(void)
{
	return alp_rtc_open(0);
}
static void rtc_close_(void *h)
{
	alp_rtc_close(h);
}
static const alp_capabilities_t *rtc_caps_(const void *h)
{
	return alp_rtc_capabilities(h);
}
static alp_status_t rtc_null_call_(void)
{
	alp_rtc_time_t t;
	return alp_rtc_get_time(NULL, &t);
}

/* WDT */
static void *wdt_open_null_(void)
{
	return alp_wdt_open(NULL);
}
static void *wdt_open_invalid_(void)
{
	return alp_wdt_open(&(alp_wdt_config_t){
	    .wdt_id     = CONF_BAD_INSTANCE,
	    .timeout_ms = 1000,
	    .on_timeout = ALP_WDT_RESET_SOC,
	});
}
static void *wdt_open_valid_(void)
{
	return alp_wdt_open(&(alp_wdt_config_t){
	    .wdt_id     = 0,
	    .timeout_ms = 1000,
	    .on_timeout = ALP_WDT_RESET_SOC,
	});
}
static void wdt_close_(void *h)
{
	alp_wdt_close(h);
}
static const alp_capabilities_t *wdt_caps_(const void *h)
{
	return alp_wdt_capabilities(h);
}
static alp_status_t wdt_null_call_(void)
{
	return alp_wdt_feed(NULL);
}

/* Counter */
static void *counter_open_null_(void)
{
	return alp_counter_open(NULL);
}
static void *counter_open_invalid_(void)
{
	return alp_counter_open(&(alp_counter_config_t){ .counter_id = CONF_BAD_INSTANCE });
}
static void *counter_open_valid_(void)
{
	return alp_counter_open(&(alp_counter_config_t){ .counter_id = 0 });
}
static void counter_close_(void *h)
{
	alp_counter_close(h);
}
static const alp_capabilities_t *counter_caps_(const void *h)
{
	return alp_counter_capabilities(h);
}
static alp_status_t counter_null_call_(void)
{
	return alp_counter_start(NULL);
}

/* Quadrature encoder */
static void *qenc_open_null_(void)
{
	return alp_qenc_open(NULL);
}
static void *qenc_open_invalid_(void)
{
	return alp_qenc_open(&(alp_qenc_config_t){ .encoder_id = CONF_BAD_INSTANCE });
}
static void *qenc_open_valid_(void)
{
	return alp_qenc_open(&(alp_qenc_config_t){ .encoder_id = 0, .pulses_per_rev = 1024 });
}
static void qenc_close_(void *h)
{
	alp_qenc_close(h);
}
static const alp_capabilities_t *qenc_caps_(const void *h)
{
	return alp_qenc_capabilities(h);
}
static alp_status_t qenc_null_call_(void)
{
	int32_t pos = 0;
	return alp_qenc_get_position(NULL, &pos);
}

/* I2S */
static void *i2s_open_null_(void)
{
	return alp_i2s_open(NULL);
}
static void *i2s_open_invalid_(void)
{
	return alp_i2s_open(&(alp_i2s_config_t){
	    .bus_id         = CONF_BAD_INSTANCE,
	    .sample_rate_hz = 16000,
	    .word_bits      = 16,
	    .channels       = 1,
	    .format         = ALP_I2S_FMT_I2S,
	    .direction      = ALP_I2S_DIR_RX,
	    .block_frames   = 64,
	});
}
static void *i2s_open_valid_(void)
{
	return alp_i2s_open(&(alp_i2s_config_t){
	    .bus_id         = 0,
	    .sample_rate_hz = 16000,
	    .word_bits      = 16,
	    .channels       = 1,
	    .format         = ALP_I2S_FMT_I2S,
	    .direction      = ALP_I2S_DIR_RX,
	    .block_frames   = 64,
	});
}
static void i2s_close_(void *h)
{
	alp_i2s_close(h);
}
static const alp_capabilities_t *i2s_caps_(const void *h)
{
	return alp_i2s_capabilities(h);
}
static alp_status_t i2s_null_call_(void)
{
	return alp_i2s_start(NULL);
}

/* ------------------------------------------------------------------ */
/* THE TABLE -- one row per portable class                             */
/* ------------------------------------------------------------------ */

static const conf_class_t conf_classes[] = {
	{
	    .name             = "gpio",
	    .open_null_cfg    = NULL, /* id-based open: case A n/a */
	    .open_invalid     = gpio_open_invalid_,
	    .open_valid       = gpio_open_valid_,
	    .close            = gpio_close_,
	    .capabilities     = gpio_caps_,
	    .null_handle_call = gpio_null_call_,
	    .must_open        = true, /* gpio_emul pin 0 */
	},
	{
	    .name             = "i2c",
	    .open_null_cfg    = i2c_open_null_,
	    .open_invalid     = i2c_open_invalid_,
	    .open_valid       = i2c_open_valid_,
	    .close            = i2c_close_,
	    .capabilities     = i2c_caps_,
	    .null_handle_call = i2c_null_call_,
	    .must_open        = true, /* i2c_emul bus 0 */
	},
	{
	    .name             = "spi",
	    .open_null_cfg    = spi_open_null_,
	    .open_invalid     = spi_open_invalid_,
	    .open_valid       = spi_open_valid_,
	    .close            = spi_close_,
	    .capabilities     = spi_caps_,
	    .null_handle_call = spi_null_call_,
	    .must_open        = true, /* spi_emul bus 0 */
	},
	{
	    .name             = "uart",
	    .open_null_cfg    = uart_open_null_,
	    .open_invalid     = uart_open_invalid_,
	    .open_valid       = uart_open_valid_,
	    .close            = uart_close_,
	    .capabilities     = uart_caps_,
	    .null_handle_call = uart_null_call_,
	    .must_open        = true, /* native_sim uart0 */
	},
	{
	    .name             = "adc",
	    .open_null_cfg    = adc_open_null_,
	    .open_invalid     = adc_open_invalid_,
	    .open_valid       = adc_open_valid_,
	    .close            = adc_close_,
	    .capabilities     = adc_caps_,
	    .null_handle_call = adc_null_call_,
	    .must_open        = false, /* no ADC controller on native_sim */
	},
	{
	    .name             = "dac",
	    .open_null_cfg    = dac_open_null_,
	    .open_invalid     = dac_open_invalid_,
	    .open_valid       = dac_open_valid_,
	    .close            = dac_close_,
	    .capabilities     = dac_caps_,
	    .null_handle_call = dac_null_call_,
	    .must_open        = false, /* no DAC controller on native_sim */
	},
	{
	    .name             = "pwm",
	    .open_null_cfg    = pwm_open_null_,
	    .open_invalid     = pwm_open_invalid_,
	    .open_valid       = pwm_open_valid_,
	    .close            = pwm_close_,
	    .capabilities     = pwm_caps_,
	    .null_handle_call = pwm_null_call_,
	    .must_open        = false, /* no PWM controller on native_sim */
	},
	{
	    .name             = "can",
	    .open_null_cfg    = can_open_null_,
	    .open_invalid     = can_open_invalid_,
	    .open_valid       = can_open_valid_,
	    .close            = can_close_,
	    .capabilities     = can_caps_,
	    .null_handle_call = can_null_call_,
	    .must_open        = false, /* no CAN controller on native_sim */
	},
	{
	    .name             = "rtc",
	    .open_null_cfg    = NULL, /* id-based open: case A n/a */
	    .open_invalid     = rtc_open_invalid_,
	    .open_valid       = rtc_open_valid_,
	    .close            = rtc_close_,
	    .capabilities     = rtc_caps_,
	    .null_handle_call = rtc_null_call_,
	    .must_open        = false, /* no RTC device on native_sim */
	},
	{
	    .name             = "wdt",
	    .open_null_cfg    = wdt_open_null_,
	    .open_invalid     = wdt_open_invalid_,
	    .open_valid       = wdt_open_valid_,
	    .close            = wdt_close_,
	    .capabilities     = wdt_caps_,
	    .null_handle_call = wdt_null_call_,
	    .must_open        = false, /* no WDT device wired in the overlay */
	},
	{
	    .name             = "counter",
	    .open_null_cfg    = counter_open_null_,
	    .open_invalid     = counter_open_invalid_,
	    .open_valid       = counter_open_valid_,
	    .close            = counter_close_,
	    .capabilities     = counter_caps_,
	    .null_handle_call = counter_null_call_,
	    .must_open        = false, /* no counter device on native_sim */
	},
	{
	    .name             = "qenc",
	    .open_null_cfg    = qenc_open_null_,
	    .open_invalid     = qenc_open_invalid_,
	    .open_valid       = qenc_open_valid_,
	    .close            = qenc_close_,
	    .capabilities     = qenc_caps_,
	    .null_handle_call = qenc_null_call_,
	    .must_open        = false, /* no quadrature decoder on native_sim */
	},
	{
	    .name             = "i2s",
	    .open_null_cfg    = i2s_open_null_,
	    .open_invalid     = i2s_open_invalid_,
	    .open_valid       = i2s_open_valid_,
	    .close            = i2s_close_,
	    .capabilities     = i2s_caps_,
	    .null_handle_call = i2s_null_call_,
	    .must_open        = false, /* no I2S controller on native_sim */
	},
};

/* ------------------------------------------------------------------ */
/* Generic runners -- one ZTEST per contract case, iterating the table */
/* ------------------------------------------------------------------ */

/** Case A: open(NULL config) refuses with NULL + ALP_ERR_INVAL. */
ZTEST(alp_conformance, test_open_null_config_yields_inval)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		if (c->open_null_cfg == NULL) {
			TC_PRINT("conformance[%s]: open() is id-based, NULL-cfg case n/a\n", c->name);
			continue;
		}
		void *h = c->open_null_cfg();
		zassert_is_null(h, "conformance[%s]: open(NULL cfg) must return NULL", c->name);
		zassert_equal(alp_last_error(),
		              ALP_ERR_INVAL,
		              "conformance[%s]: open(NULL cfg) must set ALP_ERR_INVAL, got %d",
		              c->name,
		              (int)alp_last_error());
	}
}

/** Case B: open(out-of-range instance) refuses with NULL + a
 *  documented alp_status_t error -- never OK, never a raw errno. */
ZTEST(alp_conformance, test_open_invalid_instance_fails_cleanly)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		void *h = c->open_invalid();
		zassert_is_null(h, "conformance[%s]: open(bad instance) must return NULL", c->name);

		alp_status_t e = alp_last_error();
		zassert_not_equal(
		    e, ALP_OK, "conformance[%s]: open(bad instance) must record an error", c->name);
		zassert_true(conf_status_in_enum(e),
		             "conformance[%s]: last_error %d is outside alp_status_t "
		             "(raw errno leak?)",
		             c->name,
		             (int)e);
	}
}

/** Case C: close(NULL) is documented as a no-op for every class. */
ZTEST(alp_conformance, test_close_null_is_idempotent)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		/* Twice: a survived double NULL-close is the proof. */
		c->close(NULL);
		c->close(NULL);
	}
}

/** Case D: capabilities(NULL) returns NULL per the documented
 *  contract ("NULL if the handle is NULL"). */
ZTEST(alp_conformance, test_capabilities_null_handle_yields_null)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		if (c->capabilities == NULL) {
			TC_PRINT("conformance[%s]: no capabilities query yet (skip-marked)\n", c->name);
			continue;
		}
		zassert_is_null(
		    c->capabilities(NULL), "conformance[%s]: capabilities(NULL) must return NULL", c->name);
	}
}

/** Cases E + F: open(valid instance 0).  Where native_sim backs the
 *  class the open MUST succeed and capabilities(handle) MUST be
 *  non-NULL.  Where it doesn't, the open MUST fail the documented
 *  way (NULL + in-enum error) -- the degrade path is asserted and
 *  logged, never skipped silently. */
ZTEST(alp_conformance, test_open_valid_instance_contract)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		void *h = c->open_valid();
		if (c->must_open) {
			zassert_not_null(h,
			                 "conformance[%s]: instance 0 is backed on native_sim, open must "
			                 "succeed (last_error %d)",
			                 c->name,
			                 (int)alp_last_error());
		}

		if (h != NULL) {
			if (c->capabilities != NULL) {
				zassert_not_null(c->capabilities(h),
				                 "conformance[%s]: capabilities(open handle) must be "
				                 "non-NULL",
				                 c->name);
			} else {
				TC_PRINT("conformance[%s]: opened OK; capabilities query "
				         "skip-marked\n",
				         c->name);
			}
			c->close(h);
			TC_PRINT("conformance[%s]: positive path (open + caps + close)\n", c->name);
		} else {
			/* Degrade path: no instance on this platform.  The failure
			 * contract IS the test -- NULL + documented error code. */
			alp_status_t e = alp_last_error();
			zassert_not_equal(
			    e, ALP_OK, "conformance[%s]: failed open must record an error", c->name);
			zassert_true(conf_status_in_enum(e),
			             "conformance[%s]: last_error %d is outside alp_status_t "
			             "(raw errno leak?)",
			             c->name,
			             (int)e);
			TC_PRINT("conformance[%s]: degrade path -- no native_sim instance, "
			         "open refused with %d (failure contract validated)\n",
			         c->name,
			         (int)e);
		}
	}
}

/** Case G: double close is safe -- the second close on an
 *  already-released handle is a no-op (dispatchers gate on the
 *  pool's in_use flag). */
ZTEST(alp_conformance, test_double_close_is_safe)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		void *h = c->open_valid();
		if (h == NULL) {
			/* close(NULL) idempotency for this class is already covered
			 * by case C; nothing more to prove without an instance. */
			TC_PRINT("conformance[%s]: degrade path -- no instance to "
			         "double-close (NULL-close covered by case C)\n",
			         c->name);
			continue;
		}
		c->close(h);
		c->close(h); /* must be a silent no-op, not a crash / double-free */
		TC_PRINT("conformance[%s]: double-close survived\n", c->name);
	}
}

/** Case H: a representative data-path call on a NULL handle refuses
 *  with an in-enum, non-OK status (uniformly ALP_ERR_NOT_READY in
 *  today's dispatchers; the portable contract only requires "a
 *  documented refusal, no raw errno"). */
ZTEST(alp_conformance, test_null_handle_call_refuses_in_enum)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		alp_status_t s = c->null_handle_call();
		zassert_not_equal(
		    s, ALP_OK, "conformance[%s]: NULL-handle call must not return ALP_OK", c->name);
		zassert_true(conf_status_in_enum(s),
		             "conformance[%s]: NULL-handle call returned %d, outside "
		             "alp_status_t (raw errno leak?)",
		             c->name,
		             (int)s);
	}
}
