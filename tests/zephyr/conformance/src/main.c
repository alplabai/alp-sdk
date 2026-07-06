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
 * EXPECTATIONS COME FROM THE CAPABILITY LAYER (<alp/cap.h>)
 * =========================================================
 *
 * Whether open(valid instance 0) MUST succeed is not hardcoded per
 * platform -- it is derived from alp_has() at runtime, so the suite
 * doubles as a caps<->backend parity check:
 *
 *   - CONFIG_ALP_SOC_<X> selected (any real-SoM build): alp_has(cap)
 *     is authoritative.
 *       cap TRUE  -> open(instance 0) MUST succeed -> FAIL otherwise.
 *                    ("cap true but open failed" = the port broke.)
 *       cap FALSE -> open MUST refuse the documented way -> a handle
 *                    here is a caps<->backend PARITY VIOLATION.
 *                    ("silicon lacks it" = asserted degrade path.)
 *   - CONFIG_ALP_SOC_NONE (native_sim CI): the cap macros are
 *     permissive (UINT16_MAX) and describe no silicon, so the rows
 *     fall back to the per-row `sim_backed` flag: classes the
 *     native_sim overlay backs (gpio / i2c / spi / uart / adc /
 *     counter) must open; the rest assert the failure contract.
 *
 * Rows flagged `optional` (I2C / SPI target mode) never promote to
 * must-open: their class headers document ALP_ERR_NOSUPPORT as a
 * legitimate per-driver refusal even where the silicon has the bus.
 *
 * WATCHDOG ARMING GUARD (CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM)
 * ===========================================================
 *
 * The WDT row's positive open ARMS a real watchdog, and <alp/wdt.h>
 * documents that many M-class watchdogs are write-once-armed --
 * close() cannot stop them, so a successful open on real silicon
 * resets the board mid-suite.  The positive WDT path (cases E/F/G)
 * therefore runs only under CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM:
 *
 *   - default y on ARCH_POSIX (native_sim keeps full coverage; no
 *     watchdog device is wired there, so nothing ever arms), and
 *   - default n everywhere else: SAFE-BY-DEFAULT on hardware.  A
 *     bench operator who wants the WDT positive path opts in and
 *     runs the suite expecting a possible end-of-run reset on
 *     one-shot watchdogs (dead-last verification, not CI).
 *
 * The WDT row's A/B/C/D/H cases never arm anything and always run.
 *
 * TEST MATRIX (classes x cases)
 * =============================
 *
 *   Case key:
 *     A  open(NULL cfg) -> NULL + INVAL       E  open(valid) contract (cap-derived)
 *     B  open(bad instance) -> NULL + error   F  capabilities(open handle) non-NULL
 *     C  close(NULL) idempotent               G  double-close safe
 *     D  capabilities(NULL) -> NULL           H  NULL-handle call -> in-enum refusal
 *
 *   class      | A  | B | C | D  | E on native_sim | cap driving E on real SoMs
 *   -----------+----+---+---+----+-----------------+---------------------------
 *   gpio       | -- | x | x | x  | must-open       | (none -- every SoC has pads)
 *   i2c        | x  | x | x | x  | must-open       | HW_I2C
 *   spi        | x  | x | x | x  | must-open       | HW_SPI
 *   uart       | x  | x | x | x  | must-open       | HW_UART
 *   adc        | x  | x | x | x  | must-open       | HW_ADC   (zephyr,adc-emul)
 *   dac        | x  | x | x | x  | degrade         | HW_DAC
 *   pwm        | x  | x | x | x  | degrade         | HW_PWM
 *   can        | x  | x | x | x  | degrade         | HW_CAN
 *   rtc        | -- | x | x | x  | degrade         | HW_RTC
 *   wdt        | x  | x | x | x  | degrade (guard) | HW_WDT   (arming guard)
 *   counter    | x  | x | x | x  | must-open       | HW_TIMER (native-sim-counter)
 *   qenc       | x  | x | x | x  | degrade         | HW_QENC
 *   i2s        | x  | x | x | x  | degrade         | HW_I2S
 *   i2c_target | x  | x | x | -- | degrade         | HW_I2C   (optional: NOSUPPORT ok)
 *   spi_target | x  | x | x | -- | opens (slave)   | HW_SPI   (optional: NOSUPPORT ok)
 *
 *   gpio / rtc take a plain id instead of a config pointer, so case A
 *   does not apply; the target rows have no capabilities query (D
 *   skip-marked) and i2c_target has no status-returning data call (H
 *   skip-marked).
 *
 *   Degrade paths never skip silently: the refusal itself is the
 *   asserted contract, and TC_PRINT logs whether a row ran the
 *   positive or the failure-contract path -- with "silicon lacks it"
 *   (cap false) distinguished from "no instance on this build".
 *
 * Beyond the table, three ZTESTs cover the v0.9 non-class surfaces:
 * alp_init/alp_deinit lifecycle idempotency, the alp_uart_rx_ringbuf_*
 * contract (feature on or off), and I2C-target config validation.
 *
 * ADDING VENDOR N+1 (or a new class): add ONE conf_class_t row to
 * conf_classes[] below -- three tiny open adapters, the class's
 * close / capabilities functions, a representative NULL-handle
 * call, and the alp_cap_id_t that gates the class.  The generic
 * runners pick the row up automatically; no new ZTEST bodies are
 * needed.
 *
 */

#include <zephyr/ztest.h>

#include "alp/adc.h"
#include "alp/can.h"
#include "alp/cap.h"
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

/* Row marker for classes the capability layer does not model (GPIO:
 * every supported SoC has pads).  alp_cap_name() has no entry for it,
 * so conf_cap_str() special-cases the label. */
#define CONF_CAP_NONE ALP_CAP_ID_COUNT

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
	 *  with an in-enum, non-OK status.  NULL ("skip-marked") when
	 *  the class has no status-returning data call. */
	conf_null_call_fn_t null_handle_call;
	/** Capability that gates the class on real-SoM builds, or
	 *  CONF_CAP_NONE when presence is unconditional. */
	alp_cap_id_t cap;
	/** true when the native_sim overlay backs instance 0 -- the
	 *  positive open path is mandatory on permissive
	 *  (CONFIG_ALP_SOC_NONE) builds. */
	bool sim_backed;
	/** true when the class headers document NOSUPPORT as a
	 *  legitimate refusal even where the silicon has the bus
	 *  (target modes: per-driver support is optional) -- the row
	 *  never promotes to must-open. */
	bool optional;
	/** true when a successful open irreversibly arms hardware (WDT:
	 *  write-once-armed on many M-class SoCs).  The positive path
	 *  then runs only under CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM --
	 *  see the file header. */
	bool arms_hardware;
} conf_class_t;

/** @return true when @p s is a member of the alp_status_t enum. */
static bool conf_status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

/** @return printable capability name for a row's failure messages. */
static const char *conf_cap_str(const conf_class_t *c)
{
	return (c->cap == CONF_CAP_NONE) ? "(none)" : alp_cap_name(c->cap);
}

/** Case E policy: MUST open(instance 0) succeed on this build?
 *  Real-SoM builds derive the answer from the capability layer;
 *  permissive builds fall back to the overlay-backed row flag. */
static bool conf_must_open(const conf_class_t *c)
{
#if defined(CONFIG_ALP_SOC_NONE)
	return c->sim_backed;
#else
	if (c->optional) {
		return false;
	}
	return (c->cap == CONF_CAP_NONE) ? true : alp_has(c->cap);
#endif
}

/** Case E policy: MUST open(instance 0) FAIL on this build?  Only a
 *  real-SoM build can say "the silicon lacks this class" -- a handle
 *  there is a caps<->backend parity violation. */
static bool conf_must_degrade(const conf_class_t *c)
{
#if defined(CONFIG_ALP_SOC_NONE)
	return false;
#else
	return (c->cap != CONF_CAP_NONE) && !alp_has(c->cap);
#endif
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

/* I2C target (slave) mode -- v0.9.  The suite's callbacks are inert:
 * no external controller drives the bus during a conformance run, so
 * they exist only to satisfy the documented "must be non-NULL"
 * config contract. */
static void conf_i2c_tgt_on_write_(uint8_t byte, void *user)
{
	(void)byte;
	(void)user;
}
static alp_status_t conf_i2c_tgt_on_read_(uint8_t *byte, void *user)
{
	(void)user;
	*byte = 0xFF;
	return ALP_OK;
}
static void *i2c_target_open_null_(void)
{
	return alp_i2c_target_open(NULL);
}
static void *i2c_target_open_invalid_(void)
{
	return alp_i2c_target_open(&(alp_i2c_target_config_t){
	    .bus_id        = CONF_BAD_INSTANCE,
	    .own_addr_7bit = 0x2A,
	    .on_write      = conf_i2c_tgt_on_write_,
	    .on_read       = conf_i2c_tgt_on_read_,
	});
}
static void *i2c_target_open_valid_(void)
{
	return alp_i2c_target_open(&(alp_i2c_target_config_t){
	    .bus_id        = 0,
	    .own_addr_7bit = 0x2A,
	    .on_write      = conf_i2c_tgt_on_write_,
	    .on_read       = conf_i2c_tgt_on_read_,
	});
}
static void i2c_target_close_(void *h)
{
	alp_i2c_target_close(h);
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

/* SPI target (slave) mode -- v0.9. */
static void *spi_target_open_null_(void)
{
	return alp_spi_target_open(NULL);
}
static void *spi_target_open_invalid_(void)
{
	return alp_spi_target_open(&(alp_spi_target_config_t){
	    .bus_id        = CONF_BAD_INSTANCE,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	});
}
static void *spi_target_open_valid_(void)
{
	return alp_spi_target_open(&(alp_spi_target_config_t){
	    .bus_id        = 0,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	});
}
static void spi_target_close_(void *h)
{
	alp_spi_target_close(h);
}
static alp_status_t spi_target_null_call_(void)
{
	uint8_t rx     = 0;
	size_t  rx_len = 0;
	return alp_spi_target_transceive(NULL, NULL, &rx, 1, &rx_len, 100);
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

/* WDT.  open_valid_ ARMS a real 1000 ms RESET_SOC watchdog wherever a
 * device is wired -- it runs only behind the arming guard (see the
 * file header); the other adapters never reach the hardware. */
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
	    .cap              = CONF_CAP_NONE, /* every SoC has pads */
	    .sim_backed       = true,          /* gpio_emul pin 0 */
	},
	{
	    .name             = "i2c",
	    .open_null_cfg    = i2c_open_null_,
	    .open_invalid     = i2c_open_invalid_,
	    .open_valid       = i2c_open_valid_,
	    .close            = i2c_close_,
	    .capabilities     = i2c_caps_,
	    .null_handle_call = i2c_null_call_,
	    .cap              = ALP_CAP_ID_HW_I2C,
	    .sim_backed       = true, /* i2c_emul bus 0 */
	},
	{
	    .name             = "spi",
	    .open_null_cfg    = spi_open_null_,
	    .open_invalid     = spi_open_invalid_,
	    .open_valid       = spi_open_valid_,
	    .close            = spi_close_,
	    .capabilities     = spi_caps_,
	    .null_handle_call = spi_null_call_,
	    .cap              = ALP_CAP_ID_HW_SPI,
	    .sim_backed       = true, /* spi_emul bus 0 */
	},
	{
	    .name             = "uart",
	    .open_null_cfg    = uart_open_null_,
	    .open_invalid     = uart_open_invalid_,
	    .open_valid       = uart_open_valid_,
	    .close            = uart_close_,
	    .capabilities     = uart_caps_,
	    .null_handle_call = uart_null_call_,
	    .cap              = ALP_CAP_ID_HW_UART,
	    .sim_backed       = true, /* native_sim uart0 */
	},
	{
	    .name             = "adc",
	    .open_null_cfg    = adc_open_null_,
	    .open_invalid     = adc_open_invalid_,
	    .open_valid       = adc_open_valid_,
	    .close            = adc_close_,
	    .capabilities     = adc_caps_,
	    .null_handle_call = adc_null_call_,
	    .cap              = ALP_CAP_ID_HW_ADC,
	    .sim_backed       = true, /* zephyr,adc-emul channel 0 (overlay) */
	},
	{
	    .name             = "dac",
	    .open_null_cfg    = dac_open_null_,
	    .open_invalid     = dac_open_invalid_,
	    .open_valid       = dac_open_valid_,
	    .close            = dac_close_,
	    .capabilities     = dac_caps_,
	    .null_handle_call = dac_null_call_,
	    .cap              = ALP_CAP_ID_HW_DAC,
	    .sim_backed       = false, /* no DAC emulator in Zephyr */
	},
	{
	    .name             = "pwm",
	    .open_null_cfg    = pwm_open_null_,
	    .open_invalid     = pwm_open_invalid_,
	    .open_valid       = pwm_open_valid_,
	    .close            = pwm_close_,
	    .capabilities     = pwm_caps_,
	    .null_handle_call = pwm_null_call_,
	    .cap              = ALP_CAP_ID_HW_PWM,
	    .sim_backed       = false, /* no PWM controller on native_sim */
	},
	{
	    .name             = "can",
	    .open_null_cfg    = can_open_null_,
	    .open_invalid     = can_open_invalid_,
	    .open_valid       = can_open_valid_,
	    .close            = can_close_,
	    .capabilities     = can_caps_,
	    .null_handle_call = can_null_call_,
	    .cap              = ALP_CAP_ID_HW_CAN,
	    .sim_backed       = false, /* alp-can0 alias not wired */
	},
	{
	    .name             = "rtc",
	    .open_null_cfg    = NULL, /* id-based open: case A n/a */
	    .open_invalid     = rtc_open_invalid_,
	    .open_valid       = rtc_open_valid_,
	    .close            = rtc_close_,
	    .capabilities     = rtc_caps_,
	    .null_handle_call = rtc_null_call_,
	    .cap              = ALP_CAP_ID_HW_RTC,
	    .sim_backed       = false, /* alp-rtc0 alias not wired */
	},
	{
	    .name             = "wdt",
	    .open_null_cfg    = wdt_open_null_,
	    .open_invalid     = wdt_open_invalid_,
	    .open_valid       = wdt_open_valid_,
	    .close            = wdt_close_,
	    .capabilities     = wdt_caps_,
	    .null_handle_call = wdt_null_call_,
	    .cap              = ALP_CAP_ID_HW_WDT,
	    .sim_backed       = false, /* no WDT device wired in the overlay */
	    .arms_hardware    = true,  /* open() installs + starts the watchdog */
	},
	{
	    .name             = "counter",
	    .open_null_cfg    = counter_open_null_,
	    .open_invalid     = counter_open_invalid_,
	    .open_valid       = counter_open_valid_,
	    .close            = counter_close_,
	    .capabilities     = counter_caps_,
	    .null_handle_call = counter_null_call_,
	    .cap              = ALP_CAP_ID_HW_TIMER,
	    .sim_backed       = true, /* native-sim-counter counter0 (overlay) */
	},
	{
	    .name             = "qenc",
	    .open_null_cfg    = qenc_open_null_,
	    .open_invalid     = qenc_open_invalid_,
	    .open_valid       = qenc_open_valid_,
	    .close            = qenc_close_,
	    .capabilities     = qenc_caps_,
	    .null_handle_call = qenc_null_call_,
	    .cap              = ALP_CAP_ID_HW_QENC,
	    .sim_backed       = false, /* no quadrature decoder on native_sim */
	},
	{
	    .name             = "i2s",
	    .open_null_cfg    = i2s_open_null_,
	    .open_invalid     = i2s_open_invalid_,
	    .open_valid       = i2s_open_valid_,
	    .close            = i2s_close_,
	    .capabilities     = i2s_caps_,
	    .null_handle_call = i2s_null_call_,
	    .cap              = ALP_CAP_ID_HW_I2S,
	    .sim_backed       = false, /* no I2S controller on native_sim */
	},
	{
	    .name             = "i2c_target",
	    .open_null_cfg    = i2c_target_open_null_,
	    .open_invalid     = i2c_target_open_invalid_,
	    .open_valid       = i2c_target_open_valid_,
	    .close            = i2c_target_close_,
	    .capabilities     = NULL, /* no capabilities query on the target surface */
	    .null_handle_call = NULL, /* close() is the only handle call; case C covers it */
	    .cap              = ALP_CAP_ID_HW_I2C,
	    .sim_backed       = false, /* i2c_emul has no target-mode support */
	    .optional         = true,  /* per-driver: NOSUPPORT is a documented refusal */
	},
	{
	    .name             = "spi_target",
	    .open_null_cfg    = spi_target_open_null_,
	    .open_invalid     = spi_target_open_invalid_,
	    .open_valid       = spi_target_open_valid_,
	    .close            = spi_target_close_,
	    .capabilities     = NULL, /* no capabilities query on the target surface */
	    .null_handle_call = spi_target_null_call_,
	    .cap              = ALP_CAP_ID_HW_SPI,
	    .sim_backed       = false, /* slave-mode absence surfaces at transceive */
	    .optional         = true,  /* per-driver: NOSUPPORT is a documented refusal */
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

/** Cases E + F: open(valid instance 0), with the expectation derived
 *  from the capability layer (see conf_must_open / conf_must_degrade
 *  and the file header).  Positive path: capabilities(handle) MUST be
 *  non-NULL, then close.  Degrade path: the refusal MUST follow the
 *  documented contract (NULL + in-enum error) -- asserted and logged,
 *  never skipped silently. */
ZTEST(alp_conformance, test_open_valid_instance_contract)
{
	for (size_t i = 0; i < ARRAY_SIZE(conf_classes); i++) {
		const conf_class_t *c = &conf_classes[i];

		if (c->arms_hardware && !IS_ENABLED(CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM)) {
			TC_PRINT("conformance[%s]: positive open ARMS the watchdog; skipped "
			         "(opt in via CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM, expecting "
			         "a reset on one-shot hardware)\n",
			         c->name);
			continue;
		}

		void *h = c->open_valid();
		if (conf_must_open(c)) {
			zassert_not_null(h,
			                 "conformance[%s]: instance 0 must be available here "
			                 "(cap %s, sim_backed %d) but open failed (last_error %d)",
			                 c->name,
			                 conf_cap_str(c),
			                 (int)c->sim_backed,
			                 (int)alp_last_error());
		}
		if (conf_must_degrade(c)) {
			zassert_is_null(h,
			                "conformance[%s]: alp_has(%s) is false -- silicon lacks "
			                "the class -- yet open() produced a handle "
			                "(caps<->backend parity violation)",
			                c->name,
			                conf_cap_str(c));
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
			/* Degrade path.  The failure contract IS the test --
			 * NULL + documented error code. */
			alp_status_t e = alp_last_error();
			zassert_not_equal(
			    e, ALP_OK, "conformance[%s]: failed open must record an error", c->name);
			zassert_true(conf_status_in_enum(e),
			             "conformance[%s]: last_error %d is outside alp_status_t "
			             "(raw errno leak?)",
			             c->name,
			             (int)e);
			if (conf_must_degrade(c)) {
				TC_PRINT("conformance[%s]: degrade path -- silicon lacks %s "
				         "(cap false), open refused with %d "
				         "(failure contract validated)\n",
				         c->name,
				         conf_cap_str(c),
				         (int)e);
			} else {
				TC_PRINT("conformance[%s]: degrade path -- no instance 0 on "
				         "this build, open refused with %d "
				         "(failure contract validated)\n",
				         c->name,
				         (int)e);
			}
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

		if (c->arms_hardware && !IS_ENABLED(CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM)) {
			TC_PRINT("conformance[%s]: double-close needs the arming open; "
			         "skipped (see CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM)\n",
			         c->name);
			continue;
		}

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

		if (c->null_handle_call == NULL) {
			TC_PRINT("conformance[%s]: no status-returning data call "
			         "(skip-marked)\n",
			         c->name);
			continue;
		}
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

/* ------------------------------------------------------------------ */
/* Non-class portable surfaces (v0.9)                                  */
/* ------------------------------------------------------------------ */

/** SDK lifecycle: alp_init / alp_deinit are idempotent, opens keep
 *  working after init, and init-after-deinit re-initialises --
 *  exactly the contract <alp/peripheral.h> documents. */
ZTEST(alp_conformance, test_sdk_lifecycle_init_deinit)
{
	zassert_equal(alp_init(), ALP_OK, "alp_init() must return ALP_OK");
	zassert_equal(alp_init(), ALP_OK, "second alp_init() must be an ALP_OK no-op");

	/* Open-after-init: the class expectations from case E hold
	 * unchanged once the SDK-global init ran (row 0 = gpio). */
	const conf_class_t *gpio_row = &conf_classes[0];
	void               *h        = gpio_row->open_valid();
	if (conf_must_open(gpio_row)) {
		zassert_not_null(h,
		                 "conformance[lifecycle]: gpio open after alp_init() failed "
		                 "(last_error %d)",
		                 (int)alp_last_error());
	}
	if (h != NULL) {
		gpio_row->close(h);
	}

	zassert_equal(alp_deinit(), ALP_OK, "alp_deinit() must return ALP_OK");
	zassert_equal(alp_deinit(), ALP_OK, "second alp_deinit() must be an ALP_OK no-op");
	zassert_equal(alp_init(), ALP_OK, "alp_init() after alp_deinit() must re-initialise");
	zassert_equal(alp_deinit(), ALP_OK, "leave the SDK deinitialised for other suites");
}

/** UART RX ring buffer: NULL-handle surface refuses in-enum without
 *  crashing, and attach on an open port either yields a live (empty)
 *  ring or the documented refusal -- both outcomes are asserted, so
 *  the same test passes with CONFIG_ALP_SDK_UART_RX_RINGBUF on or
 *  off. */
ZTEST(alp_conformance, test_uart_rx_ringbuf_contract)
{
	static uint8_t backing[64];
	uint8_t        dst[4];
	size_t         got = 99;

	/* NULL-handle surface: documented refusals, never a crash. */
	alp_status_t s = alp_uart_rx_ringbuf_pop(NULL, dst, sizeof dst, &got);
	zassert_not_equal(s, ALP_OK, "conformance[uart_rx_ringbuf]: pop(NULL) must refuse");
	zassert_true(conf_status_in_enum(s),
	             "conformance[uart_rx_ringbuf]: pop(NULL) returned %d, outside alp_status_t",
	             (int)s);
	zassert_equal(got, 0u, "conformance[uart_rx_ringbuf]: pop(NULL) must zero *got");
	zassert_equal(
	    alp_uart_rx_ringbuf_count(NULL), 0u, "conformance[uart_rx_ringbuf]: count(NULL) must be 0");
	alp_uart_rx_ringbuf_detach(NULL); /* documented no-op */

	void *rb0 = alp_uart_rx_ringbuf_attach(NULL, backing, sizeof backing);
	zassert_is_null(rb0, "conformance[uart_rx_ringbuf]: attach(NULL port) must refuse");
	alp_status_t e0 = alp_last_error();
	zassert_not_equal(
	    e0, ALP_OK, "conformance[uart_rx_ringbuf]: attach(NULL port) must record an error");
	zassert_true(conf_status_in_enum(e0),
	             "conformance[uart_rx_ringbuf]: attach(NULL port) error %d outside "
	             "alp_status_t",
	             (int)e0);

	/* Attach to a real open port.  Feature-off builds refuse with
	 * NOSUPPORT; feature-on builds return a live ring -- both are
	 * documented outcomes. */
	alp_uart_t *port = (alp_uart_t *)uart_open_valid_();
	if (port == NULL) {
		TC_PRINT("conformance[uart_rx_ringbuf]: no UART instance 0 on this "
		         "build; NULL-handle contract validated, attach path skipped\n");
		return;
	}
	alp_uart_rx_ringbuf_t *rb = alp_uart_rx_ringbuf_attach(port, backing, sizeof backing);
	if (rb != NULL) {
		zassert_equal(alp_uart_rx_ringbuf_count(rb),
		              0u,
		              "conformance[uart_rx_ringbuf]: fresh ring must be empty");
		got = 99;
		zassert_equal(alp_uart_rx_ringbuf_pop(rb, dst, sizeof dst, &got),
		              ALP_OK,
		              "conformance[uart_rx_ringbuf]: pop on an empty ring is OK + 0 bytes");
		zassert_equal(got, 0u, "conformance[uart_rx_ringbuf]: empty ring pops 0 bytes");
		alp_uart_rx_ringbuf_detach(rb);
		TC_PRINT("conformance[uart_rx_ringbuf]: positive path "
		         "(attach + count + pop + detach)\n");
	} else {
		alp_status_t e = alp_last_error();
		zassert_not_equal(
		    e, ALP_OK, "conformance[uart_rx_ringbuf]: refused attach must record an error");
		zassert_true(conf_status_in_enum(e),
		             "conformance[uart_rx_ringbuf]: attach error %d outside alp_status_t",
		             (int)e);
		TC_PRINT("conformance[uart_rx_ringbuf]: degrade path -- attach refused "
		         "with %d (feature off or port unsupported)\n",
		         (int)e);
	}
	uart_close_(port);
}

/** I2C target config validation: the header documents on_write /
 *  on_read as REQUIRED (INVAL when NULL) and 0x08..0x77 as the legal
 *  own-address range.  Only the documented current behavior is
 *  asserted -- driver-level support differences surface in case E. */
ZTEST(alp_conformance, test_i2c_target_config_validation)
{
	alp_i2c_target_t *t = alp_i2c_target_open(&(alp_i2c_target_config_t){
	    .bus_id        = 0,
	    .own_addr_7bit = 0x2A,
	    .on_write      = NULL,
	    .on_read       = NULL,
	});
	zassert_is_null(t, "conformance[i2c_target]: NULL callbacks must be rejected");
	zassert_equal(alp_last_error(),
	              ALP_ERR_INVAL,
	              "conformance[i2c_target]: NULL callbacks must set ALP_ERR_INVAL, got %d",
	              (int)alp_last_error());

	t = alp_i2c_target_open(&(alp_i2c_target_config_t){
	    .bus_id        = 0,
	    .own_addr_7bit = 0xFF, /* beyond 7-bit space */
	    .on_write      = conf_i2c_tgt_on_write_,
	    .on_read       = conf_i2c_tgt_on_read_,
	});
	zassert_is_null(t, "conformance[i2c_target]: out-of-range own address must be rejected");
	alp_status_t e = alp_last_error();
	zassert_not_equal(e, ALP_OK, "conformance[i2c_target]: rejected open must record an error");
	zassert_true(conf_status_in_enum(e),
	             "conformance[i2c_target]: own-address rejection %d outside alp_status_t",
	             (int)e);
}
