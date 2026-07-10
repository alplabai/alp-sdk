/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared stub backend for the Alp SDK.
 *
 * Every public `alp_*` function in the SDK has a default
 * NOSUPPORT implementation here.  Backends that do real work
 * (currently `src/zephyr/`) override selectively via per-class
 * Kconfig and CMake gating; backends without a working impl yet
 * (`src/baremetal/`, `src/yocto/`) compile this file alone so the
 * resulting library is link-complete.
 *
 * This means apps targeting baremetal or yocto get a `libalp_sdk.a`
 * (or `.so`) that exports every documented entry point but every
 * call returns ALP_ERR_NOSUPPORT (or NULL for `*_open`).  Real
 * implementations land per the per-backend roadmap in VERSIONS.md
 * (baremetal v0.2 for AEN; yocto v0.4 first-class).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/adc.h"
#include "alp/counter.h"
#include "alp/i2s.h"
#include "alp/can.h"
#include "alp/rtc.h"
#include "alp/wdt.h"
#include "alp/display.h"
#include "alp/camera.h"
#include "alp/iot.h"
#include "alp/audio.h"
#include "alp/ble.h"
#include "alp/security.h"
#include "alp/mproc.h"
#include "alp/inference.h"
#include "alp/storage.h"
#include "alp/usb.h"
#include "alp/power.h"
#include "alp/gpu2d.h"
#include "alp/model.h"
#include "alp/rpc.h"

#include "alp_internal.h"

/* ------------------------------------------------------------------ */
/* alp_last_error — one canonical last-error slot, thread-local on a   */
/* hosted Linux target (ALP_LAST_ERROR_TLS, see alp_internal.h).       */
/*                                                                      */
/* This is the single storage every non-Zephyr layer reads/writes:     */
/* cross-TU writers (incl. the vendor/<som> peripheral wrappers under  */
/* ALP_VENDOR_OVERRIDES_PERIPHERAL) go through alp_internal_set_last_-  */
/* error; local writers in this file write z_last_error directly for   */
/* brevity.  Defined unconditionally -- no vendor build owns a         */
/* separate static or a duplicate alp_last_error reader anymore.       */
/* ------------------------------------------------------------------ */

static ALP_LAST_ERROR_TLS alp_status_t z_last_error;

alp_status_t alp_last_error(void)
{
	return z_last_error;
}

void alp_internal_set_last_error(alp_status_t s)
{
	z_last_error = s;
}

/* ------------------------------------------------------------------ */
/* Delay primitives.                                                   */
/*                                                                     */
/* On a Linux host (the real Yocto target, and the ALP_SOM=none        */
/* "baremetal" plain-CMake build, which -- absent a vendor cross       */
/* toolchain file -- also compiles and runs natively on the CI host)   */
/* clock_nanosleep(CLOCK_MONOTONIC) gives an accurate, scheduler-       */
/* yielding wait; the loop below retries across EINTR (the request is  */
/* relative, so clock_nanosleep rewrites `ts` with the remaining time   */
/* on interruption) so a signal never truncates the sleep short of the  */
/* contract's "at least" floor.                                        */
/*                                                                     */
/* A genuine non-Linux bare-metal target (no vendor HAL delay override  */
/* exists yet -- see vendors/<som>/) has no clock to measure against,   */
/* so it falls through to a busy-loop.  The loop deliberately           */
/* over-provisions its per-microsecond iteration count rather than risk */
/* an early return; slower cores simply overshoot; "at least us elapses"*/
/* never becomes "well under us".  A vendor HAL bring-up should replace  */
/* it with a cycle-counter-driven wait (SysTick / DWT->CYCCNT / core     */
/* timer) once one lands.                                               */
/* ------------------------------------------------------------------ */

#if defined(__linux__)

#include <errno.h>
#include <time.h>

static void z_delay_clock_nanosleep(long sec, long nsec)
{
	struct timespec ts = { .tv_sec = sec, .tv_nsec = nsec };
	while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {
		/* `ts` now holds the remaining time; retry until it elapses. */
	}
}

void alp_delay_us(uint32_t us)
{
	if (us == 0u) return;
	z_delay_clock_nanosleep((long)(us / 1000000u), (long)(us % 1000000u) * 1000L);
}

void alp_delay_ms(uint32_t ms)
{
	if (ms == 0u) return;
	z_delay_clock_nanosleep((long)(ms / 1000u), (long)(ms % 1000u) * 1000000L);
}

#else /* !__linux__ -- no OS clock; fall back to an over-provisioned spin */

/* Deliberately large: chosen so even a multi-GHz core still spins for
 * at least 1 us per iteration of the outer loop below.  Overflow-safe
 * by construction -- the multiplication is bounded to one us worth of
 * spins per outer-loop pass instead of `us * SPINS_PER_US` in one shot. */
#define ALP_DELAY_STUB_SPINS_PER_US 100000u

void alp_delay_us(uint32_t us)
{
	for (uint32_t i = 0u; i < us; i++) {
		volatile uint32_t spin = ALP_DELAY_STUB_SPINS_PER_US;
		while (spin != 0u) {
			--spin;
		}
	}
}

void alp_delay_ms(uint32_t ms)
{
	if (ms == 0u) return;
	for (uint32_t i = 0u; i < ms; i++) {
		alp_delay_us(1000u);
	}
}

#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* I2C / SPI / GPIO / UART (peripheral.h)                              */
/*                                                                     */
/* Each class is independently gateable so a backend can override one  */
/* class (e.g. Yocto I2C against /dev/i2c-N) while the others fall     */
/* back to NOSUPPORT.  The umbrella `ALP_VENDOR_OVERRIDES_PERIPHERAL`  */
/* macro is preserved for backends that provide all four at once      */
/* (vendors/alif/ etc.); it implies every per-class macro below.       */
/* ------------------------------------------------------------------ */

#if defined(ALP_VENDOR_OVERRIDES_PERIPHERAL)
#define ALP_VENDOR_OVERRIDES_I2C  1
#define ALP_VENDOR_OVERRIDES_SPI  1
#define ALP_VENDOR_OVERRIDES_GPIO 1
#define ALP_VENDOR_OVERRIDES_UART 1
#endif

#if !defined(ALP_VENDOR_OVERRIDES_I2C)
alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_i2c_write(alp_i2c_t *b, uint8_t a, const uint8_t *d, size_t l)
{
	(void)b;
	(void)a;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2c_read(alp_i2c_t *b, uint8_t a, uint8_t *d, size_t l)
{
	(void)b;
	(void)a;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_i2c_write_read(alp_i2c_t *b, uint8_t a, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
	(void)b;
	(void)a;
	(void)w;
	(void)wl;
	(void)r;
	(void)rl;
	return ALP_ERR_NOSUPPORT;
}
void alp_i2c_close(alp_i2c_t *b)
{
	(void)b;
}
#endif /* !ALP_VENDOR_OVERRIDES_I2C */

/* Unguarded: neither the Linux i2c-dev wrapper (src/yocto/peripheral_i2c.c,
 * ALP_VENDOR_OVERRIDES_I2C=1 on that path) nor the plain stub above
 * implements alp_i2c_capabilities -- only the Zephyr registry dispatcher
 * (src/i2c_dispatch.c, never compiled outside Zephyr) does.  Compiled
 * unconditionally here so every non-Zephyr build exports the symbol (#593). */
const alp_capabilities_t *alp_i2c_capabilities(const alp_i2c_t *b)
{
	(void)b;
	return NULL;
}

/* I2C target (slave) mode -- Zephyr-only today.  Gated independently
 * of ALP_VENDOR_OVERRIDES_I2C so a vendor wrapper can adopt target
 * mode later without re-implementing the controller-mode surface. */
#if !defined(ALP_VENDOR_OVERRIDES_I2C_TARGET)
alp_i2c_target_t *alp_i2c_target_open(const alp_i2c_target_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
void alp_i2c_target_close(alp_i2c_target_t *t)
{
	(void)t;
}
#endif /* !ALP_VENDOR_OVERRIDES_I2C_TARGET */

#if !defined(ALP_VENDOR_OVERRIDES_SPI)
alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_spi_transceive(alp_spi_t *b, const uint8_t *t, uint8_t *r, size_t l)
{
	(void)b;
	(void)t;
	(void)r;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_spi_write(alp_spi_t *b, const uint8_t *t, size_t l)
{
	(void)b;
	(void)t;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_spi_read(alp_spi_t *b, uint8_t *r, size_t l)
{
	(void)b;
	(void)r;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
void alp_spi_close(alp_spi_t *b)
{
	(void)b;
}
#endif /* !ALP_VENDOR_OVERRIDES_SPI */

/* Unguarded -- same reasoning as alp_i2c_capabilities above: no Linux
 * spidev wrapper or plain stub implements it; only the Zephyr registry
 * dispatcher (src/spi_dispatch.c) does (#593). */
const alp_capabilities_t *alp_spi_capabilities(const alp_spi_t *b)
{
	(void)b;
	return NULL;
}

/* SPI target (slave) mode -- Zephyr-only today.  Gated independently
 * of ALP_VENDOR_OVERRIDES_SPI so a vendor wrapper can adopt target
 * mode later without re-implementing the controller-mode surface. */
#if !defined(ALP_VENDOR_OVERRIDES_SPI_TARGET)
alp_spi_target_t *alp_spi_target_open(const alp_spi_target_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_spi_target_transceive(
    alp_spi_target_t *b, const uint8_t *t, uint8_t *r, size_t l, size_t *rl, uint32_t to_ms)
{
	(void)b;
	(void)t;
	(void)r;
	(void)l;
	(void)to_ms;
	if (rl != NULL) *rl = 0;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_spi_target_close(alp_spi_target_t *t)
{
	(void)t;
	return ALP_OK; /* nothing was ever opened -- close is a no-op */
}
#endif /* !ALP_VENDOR_OVERRIDES_SPI_TARGET */

#if !defined(ALP_VENDOR_OVERRIDES_GPIO)
alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	(void)pin_id;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_gpio_configure(alp_gpio_t *p, alp_gpio_dir_t d, alp_gpio_pull_t pu)
{
	(void)p;
	(void)d;
	(void)pu;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_write(alp_gpio_t *p, bool l)
{
	(void)p;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_read(alp_gpio_t *p, bool *l)
{
	(void)p;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_irq_enable(alp_gpio_t *p, alp_gpio_edge_t e, alp_gpio_cb_t cb, void *u)
{
	(void)p;
	(void)e;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_irq_disable(alp_gpio_t *p)
{
	(void)p;
	return ALP_ERR_NOSUPPORT;
}
void alp_gpio_close(alp_gpio_t *p)
{
	(void)p;
}
#endif /* !ALP_VENDOR_OVERRIDES_GPIO */

/* Unguarded -- same reasoning as alp_i2c_capabilities above: no Linux
 * gpiochip wrapper or plain stub implements it; only the Zephyr registry
 * dispatcher (src/gpio_dispatch.c) does (#593). */
const alp_capabilities_t *alp_gpio_capabilities(const alp_gpio_t *p)
{
	(void)p;
	return NULL;
}

#if !defined(ALP_VENDOR_OVERRIDES_UART)
alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_uart_write(alp_uart_t *p, const uint8_t *d, size_t l)
{
	(void)p;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_uart_read(alp_uart_t *p, uint8_t *d, size_t l, uint32_t t)
{
	(void)p;
	(void)d;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_uart_close(alp_uart_t *p)
{
	(void)p;
}
#endif /* !ALP_VENDOR_OVERRIDES_UART */

/* Unguarded -- same reasoning as alp_i2c_capabilities above: no Linux
 * termios wrapper or plain stub implements it; only the Zephyr registry
 * dispatcher (src/uart_dispatch.c) does (#593). */
const alp_capabilities_t *alp_uart_capabilities(const alp_uart_t *p)
{
	(void)p;
	return NULL;
}

/* UART RX ring buffer -- Zephyr-only today (CONFIG_ALP_SDK_UART_RX_RINGBUF).
 * No Yocto or baremetal backend overrides these yet, so the stubs
 * here are the canonical NOSUPPORT path on every non-Zephyr build.
 * Gated independently of ALP_VENDOR_OVERRIDES_UART so a backend can
 * adopt the ringbuf later without re-implementing the entire UART
 * surface. */
#if !defined(ALP_VENDOR_OVERRIDES_UART_RX_RINGBUF)
alp_uart_rx_ringbuf_t *
alp_uart_rx_ringbuf_attach(alp_uart_t *port, uint8_t *backing, size_t backing_size)
{
	(void)port;
	(void)backing;
	(void)backing_size;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_uart_rx_ringbuf_pop(alp_uart_rx_ringbuf_t *rb, uint8_t *out, size_t max_len, size_t *got)
{
	(void)rb;
	(void)out;
	(void)max_len;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
size_t alp_uart_rx_ringbuf_count(const alp_uart_rx_ringbuf_t *rb)
{
	(void)rb;
	return 0;
}
void alp_uart_rx_ringbuf_detach(alp_uart_rx_ringbuf_t *rb)
{
	(void)rb;
}
#endif /* !ALP_VENDOR_OVERRIDES_UART_RX_RINGBUF */

/* ------------------------------------------------------------------ */
/* PWM / ADC / Counter / QEnc / I2S / CAN / RTC / WDT (v0.2)           */
/* ------------------------------------------------------------------ */

#if !defined(ALP_VENDOR_OVERRIDES_PWM)
alp_pwm_t *alp_pwm_open(const alp_pwm_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_pwm_set_duty(alp_pwm_t *p, uint32_t n)
{
	(void)p;
	(void)n;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_pwm_set_period(alp_pwm_t *p, uint32_t n)
{
	(void)p;
	(void)n;
	return ALP_ERR_NOSUPPORT;
}
void alp_pwm_close(alp_pwm_t *p)
{
	(void)p;
}
const alp_capabilities_t *alp_pwm_capabilities(const alp_pwm_t *p)
{
	(void)p;
	return NULL;
}
alp_status_t alp_pwm_configure(alp_pwm_t      *pwm,
                               alp_pwm_align_t align_mode,
                               uint32_t        dead_time_ns,
                               uint8_t         break_cfg)
{
	(void)pwm;
	(void)align_mode;
	(void)dead_time_ns;
	(void)break_cfg;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_pwm_single_pulse(alp_pwm_t *pwm, uint32_t pulse_ns)
{
	(void)pwm;
	(void)pulse_ns;
	return ALP_ERR_NOSUPPORT;
}
alp_pwm_capture_t *alp_pwm_capture_open(const alp_pwm_capture_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_pwm_capture_read(alp_pwm_capture_t *cap, uint32_t *period_ns_out, uint32_t *pulse_ns_out)
{
	(void)cap;
	if (period_ns_out != NULL) *period_ns_out = 0;
	if (pulse_ns_out != NULL) *pulse_ns_out = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_pwm_capture_close(alp_pwm_capture_t *cap)
{
	(void)cap;
}
#endif /* !ALP_VENDOR_OVERRIDES_PWM */

#if !defined(ALP_VENDOR_OVERRIDES_ADC)
alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_adc_read_raw(alp_adc_t *a, int32_t *r)
{
	(void)a;
	(void)r;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_adc_read_uv(alp_adc_t *a, int32_t *u)
{
	(void)a;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_close(alp_adc_t *a)
{
	(void)a;
}
const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *a)
{
	(void)a;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_ADC */

/* ADC filter/stream/spectrum (alp/adc.h v0.5.x composition layer) --
 * unguarded (no ALP_VENDOR_OVERRIDES_ADC mute): unlike one-shot ADC
 * open/read/close, this trio has no dispatcher-owned implementation
 * anywhere -- the only real body is Zephyr-only (V2N GD32 supervisor
 * DMA-backed stream slots, src/zephyr/peripheral_adc.c), so Yocto's
 * adc_dispatch.c (which mutes the block above) leaves these three
 * undefined without a stub of their own (#593). */
alp_adc_stream_t *alp_adc_stream_open(const alp_adc_stream_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_adc_stream_read_mv(alp_adc_stream_t *stream, uint16_t *mv, size_t cap, size_t *got)
{
	(void)stream;
	(void)mv;
	(void)cap;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_stream_close(alp_adc_stream_t *stream)
{
	(void)stream;
}
alp_adc_filter_t *alp_adc_filter_open(const alp_adc_filter_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_adc_filter_read_mv(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap, size_t *got)
{
	(void)filter;
	(void)out_mv;
	(void)cap;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_filter_close(alp_adc_filter_t *filter)
{
	(void)filter;
}
alp_adc_spectrum_t *alp_adc_spectrum_open(const alp_adc_spectrum_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_adc_spectrum_read_bins(alp_adc_spectrum_t *spec, float *bins, size_t cap, size_t *got)
{
	(void)spec;
	(void)bins;
	(void)cap;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_adc_spectrum_close(alp_adc_spectrum_t *spec)
{
	(void)spec;
}

#if !defined(ALP_VENDOR_OVERRIDES_COUNTER)
alp_counter_t *alp_counter_open(const alp_counter_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_counter_start(alp_counter_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_stop(alp_counter_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_get_value(alp_counter_t *c, uint32_t *t)
{
	(void)c;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_us_to_ticks(alp_counter_t *c, uint32_t u, uint32_t *t)
{
	(void)c;
	(void)u;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_set_alarm(alp_counter_t *c, uint32_t t, alp_counter_alarm_cb_t cb, void *u)
{
	(void)c;
	(void)t;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_cancel_alarm(alp_counter_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
void alp_counter_close(alp_counter_t *c)
{
	(void)c;
}
const alp_capabilities_t *alp_counter_capabilities(const alp_counter_t *c)
{
	(void)c;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_COUNTER */

alp_qenc_t *alp_qenc_open(const alp_qenc_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_qenc_get_position(alp_qenc_t *e, int32_t *p)
{
	(void)e;
	(void)p;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_qenc_reset_position(alp_qenc_t *e)
{
	(void)e;
	return ALP_ERR_NOSUPPORT;
}
void alp_qenc_close(alp_qenc_t *e)
{
	(void)e;
}
const alp_capabilities_t *alp_qenc_capabilities(const alp_qenc_t *e)
{
	(void)e;
	return NULL;
}

#if !defined(ALP_VENDOR_OVERRIDES_I2S)
alp_i2s_t *alp_i2s_open(const alp_i2s_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_i2s_start(alp_i2s_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2s_stop(alp_i2s_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2s_write(alp_i2s_t *i, const void *b, size_t bytes, uint32_t t)
{
	(void)i;
	(void)b;
	(void)bytes;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2s_read(alp_i2s_t *i, void *b, size_t bytes, size_t *o, uint32_t t)
{
	(void)i;
	(void)b;
	(void)bytes;
	(void)o;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_i2s_close(alp_i2s_t *i)
{
	(void)i;
}
const alp_capabilities_t *alp_i2s_capabilities(const alp_i2s_t *i)
{
	(void)i;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_I2S */

#if !defined(ALP_VENDOR_OVERRIDES_CAN)
alp_can_t *alp_can_open(const alp_can_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_can_start(alp_can_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_stop(alp_can_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_send(alp_can_t *c, const alp_can_frame_t *f, uint32_t t)
{
	(void)c;
	(void)f;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_add_filter(
    alp_can_t *c, const alp_can_filter_t *f, alp_can_rx_cb_t cb, void *u, int32_t *id)
{
	(void)c;
	(void)f;
	(void)cb;
	(void)u;
	(void)id;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_remove_filter(alp_can_t *c, int32_t id)
{
	(void)c;
	(void)id;
	return ALP_ERR_NOSUPPORT;
}
void alp_can_close(alp_can_t *c)
{
	(void)c;
}
const alp_capabilities_t *alp_can_capabilities(const alp_can_t *c)
{
	(void)c;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_CAN */

#if !defined(ALP_VENDOR_OVERRIDES_RTC)
alp_rtc_t *alp_rtc_open(uint32_t rtc_id)
{
	(void)rtc_id;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_rtc_set_time(alp_rtc_t *r, const alp_rtc_time_t *t)
{
	(void)r;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_rtc_get_time(alp_rtc_t *r, alp_rtc_time_t *t)
{
	(void)r;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_rtc_close(alp_rtc_t *r)
{
	(void)r;
}
const alp_capabilities_t *alp_rtc_capabilities(const alp_rtc_t *r)
{
	(void)r;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_RTC */

#if !defined(ALP_VENDOR_OVERRIDES_WDT)
alp_wdt_t *alp_wdt_open(const alp_wdt_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_wdt_feed(alp_wdt_t *w)
{
	(void)w;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_wdt_disable(alp_wdt_t *w)
{
	(void)w;
	return ALP_ERR_NOSUPPORT;
}
void alp_wdt_close(alp_wdt_t *w)
{
	(void)w;
}
const alp_capabilities_t *alp_wdt_capabilities(const alp_wdt_t *w)
{
	(void)w;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_WDT */

/* ------------------------------------------------------------------ */
/* Higher libraries (camera, iot, audio, ble, security, mproc, display) */
/* ------------------------------------------------------------------ */

#if !defined(ALP_VENDOR_OVERRIDES_DISPLAY)
alp_display_t *alp_display_open(const alp_display_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_display_clear(alp_display_t *d)
{
	(void)d;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_display_print(alp_display_t *d, const char *s)
{
	(void)d;
	(void)s;
	return ALP_ERR_NOSUPPORT;
}
void alp_display_close(alp_display_t *d)
{
	(void)d;
}
#endif /* !ALP_VENDOR_OVERRIDES_DISPLAY */

#if !defined(ALP_VENDOR_OVERRIDES_CAMERA)
alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_camera_start(alp_camera_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_camera_stop(alp_camera_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_camera_capture(alp_camera_t *c, alp_camera_frame_t *o, uint32_t t)
{
	(void)c;
	(void)o;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_camera_release(alp_camera_t *c, alp_camera_frame_t *f)
{
	(void)c;
	(void)f;
	return ALP_ERR_NOSUPPORT;
}
void alp_camera_close(alp_camera_t *c)
{
	(void)c;
}
#endif /* !ALP_VENDOR_OVERRIDES_CAMERA */

#if !defined(ALP_VENDOR_OVERRIDES_WIFI)
alp_wifi_t *alp_wifi_open(void)
{
	return NULL;
}
alp_status_t alp_wifi_connect(alp_wifi_t *w, const alp_wifi_credentials_t *c, uint32_t t)
{
	(void)w;
	(void)c;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_wifi_disconnect(alp_wifi_t *w)
{
	(void)w;
	return ALP_ERR_NOSUPPORT;
}
void alp_wifi_close(alp_wifi_t *w)
{
	(void)w;
}
const alp_capabilities_t *alp_wifi_capabilities(const alp_wifi_t *w)
{
	(void)w;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_WIFI */

#if !defined(ALP_VENDOR_OVERRIDES_MQTT)
alp_mqtt_t *alp_mqtt_open(const alp_mqtt_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_mqtt_connect(alp_mqtt_t *m, uint32_t t)
{
	(void)m;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_mqtt_publish(alp_mqtt_t *m, const char *t, const uint8_t *p, size_t l, alp_mqtt_qos_t q, bool r)
{
	(void)m;
	(void)t;
	(void)p;
	(void)l;
	(void)q;
	(void)r;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_mqtt_subscribe(alp_mqtt_t *m, const char *f, alp_mqtt_qos_t q, alp_mqtt_msg_cb_t cb, void *u)
{
	(void)m;
	(void)f;
	(void)q;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_mqtt_loop(alp_mqtt_t *m, uint32_t t)
{
	(void)m;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_mqtt_close(alp_mqtt_t *m)
{
	(void)m;
}
const alp_capabilities_t *alp_mqtt_capabilities(const alp_mqtt_t *m)
{
	(void)m;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_MQTT */

#if !defined(ALP_VENDOR_OVERRIDES_AUDIO_IN)
alp_audio_in_t *alp_audio_in_open(const alp_audio_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_audio_in_start(alp_audio_in_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_in_stop(alp_audio_in_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_in_read(alp_audio_in_t *i, void *b, size_t f, size_t *o, uint32_t t)
{
	(void)i;
	(void)b;
	(void)f;
	(void)t;
	if (o != NULL) *o = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_audio_in_close(alp_audio_in_t *i)
{
	(void)i;
}
const alp_capabilities_t *alp_audio_in_capabilities(const alp_audio_in_t *i)
{
	(void)i;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_AUDIO_IN */

#if !defined(ALP_VENDOR_OVERRIDES_AUDIO_OUT)
alp_audio_out_t *alp_audio_out_open(const alp_audio_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_audio_out_start(alp_audio_out_t *o)
{
	(void)o;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_out_stop(alp_audio_out_t *o)
{
	(void)o;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_audio_out_write(alp_audio_out_t *o, const void *b, size_t f, size_t *of, uint32_t t)
{
	(void)o;
	(void)b;
	(void)f;
	(void)t;
	if (of != NULL) *of = 0;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_audio_out_set_volume(alp_audio_out_t *o, uint8_t v)
{
	(void)o;
	(void)v;
	return ALP_ERR_NOSUPPORT;
}
void alp_audio_out_close(alp_audio_out_t *o)
{
	(void)o;
}
const alp_capabilities_t *alp_audio_out_capabilities(const alp_audio_out_t *o)
{
	(void)o;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_AUDIO_OUT */

#if !defined(ALP_VENDOR_OVERRIDES_BLE)
alp_ble_t *alp_ble_open(void)
{
	return NULL;
}
void alp_ble_close(alp_ble_t *b)
{
	(void)b;
}
alp_status_t alp_ble_advertise_start(alp_ble_t *b, const alp_ble_adv_config_t *c)
{
	(void)b;
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_advertise_stop(alp_ble_t *b)
{
	(void)b;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_register_service(alp_ble_t                   *b,
                                           const alp_ble_service_def_t *d,
                                           alp_ble_attr_handle_t       *h)
{
	(void)b;
	(void)d;
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_notify(
    alp_ble_t *b, alp_ble_conn_t *c, alp_ble_attr_handle_t h, const uint8_t *p, size_t l)
{
	(void)b;
	(void)c;
	(void)h;
	(void)p;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_scan_start(alp_ble_t *b, bool a, alp_ble_scan_cb_t cb, void *u)
{
	(void)b;
	(void)a;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_scan_stop(alp_ble_t *b)
{
	(void)b;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_ble_connect(alp_ble_t *b, const alp_ble_addr_t *p, uint32_t t, alp_ble_conn_t **out)
{
	(void)b;
	(void)p;
	(void)t;
	if (out) *out = NULL;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_disconnect(alp_ble_conn_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_read(
    alp_ble_conn_t *c, alp_ble_attr_handle_t h, uint8_t *o, size_t cap, size_t *ol, uint32_t t)
{
	(void)c;
	(void)h;
	(void)o;
	(void)cap;
	(void)ol;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_ble_gatt_write(
    alp_ble_conn_t *c, alp_ble_attr_handle_t h, const uint8_t *d, size_t l, uint32_t t)
{
	(void)c;
	(void)h;
	(void)d;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
const alp_capabilities_t *alp_ble_capabilities(const alp_ble_t *ble)
{
	(void)ble;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_BLE */

#if !defined(ALP_VENDOR_OVERRIDES_SECURITY)
alp_hash_t *alp_hash_open(alp_hash_alg_t a)
{
	(void)a;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *d, size_t l)
{
	(void)h;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_hash_finish(alp_hash_t *h, uint8_t *o, size_t cap, size_t *ol)
{
	(void)h;
	(void)o;
	(void)cap;
	if (ol != NULL) *ol = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_hash_close(alp_hash_t *h)
{
	(void)h;
}
const alp_capabilities_t *alp_hash_capabilities(const alp_hash_t *h)
{
	(void)h;
	return NULL;
}
alp_aead_t *alp_aead_open(alp_aead_alg_t a, const uint8_t *k, size_t kl)
{
	(void)a;
	(void)k;
	(void)kl;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_aead_encrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         il,
                              const uint8_t *aad,
                              size_t         al,
                              const uint8_t *p,
                              size_t         pl,
                              uint8_t       *co,
                              uint8_t       *t,
                              size_t         tl)
{
	(void)a;
	(void)iv;
	(void)il;
	(void)aad;
	(void)al;
	(void)p;
	(void)pl;
	(void)co;
	(void)t;
	(void)tl;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_aead_decrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         il,
                              const uint8_t *aad,
                              size_t         al,
                              const uint8_t *c,
                              size_t         cl,
                              const uint8_t *t,
                              size_t         tl,
                              uint8_t       *po)
{
	(void)a;
	(void)iv;
	(void)il;
	(void)aad;
	(void)al;
	(void)c;
	(void)cl;
	(void)t;
	(void)tl;
	(void)po;
	return ALP_ERR_NOSUPPORT;
}
void alp_aead_close(alp_aead_t *a)
{
	(void)a;
}
const alp_capabilities_t *alp_aead_capabilities(const alp_aead_t *a)
{
	(void)a;
	return NULL;
}
alp_status_t alp_random_bytes(uint8_t *o, size_t l)
{
	(void)o;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
#endif /* !ALP_VENDOR_OVERRIDES_SECURITY */

alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_shmem_view(alp_shmem_t *s, void **b, size_t *o)
{
	(void)s;
	if (b) *b = NULL;
	if (o) *o = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_shmem_close(alp_shmem_t *s)
{
	(void)s;
}
const alp_capabilities_t *alp_shmem_capabilities(const alp_shmem_t *s)
{
	(void)s;
	return NULL;
}
alp_mbox_t *alp_mbox_open(const alp_mbox_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_mbox_send(alp_mbox_t *m, const void *d, size_t l, uint32_t t)
{
	(void)m;
	(void)d;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_mbox_set_callback(alp_mbox_t *m, alp_mbox_msg_cb_t cb, void *u)
{
	(void)m;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
void alp_mbox_close(alp_mbox_t *m)
{
	(void)m;
}
const alp_capabilities_t *alp_mbox_capabilities(const alp_mbox_t *m)
{
	(void)m;
	return NULL;
}
alp_hwsem_t *alp_hwsem_open(uint32_t id)
{
	(void)id;
	return NULL;
}
alp_status_t alp_hwsem_try_lock(alp_hwsem_t *s)
{
	(void)s;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_hwsem_lock(alp_hwsem_t *s, uint32_t t)
{
	(void)s;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_hwsem_unlock(alp_hwsem_t *s)
{
	(void)s;
	return ALP_ERR_NOSUPPORT;
}
void alp_hwsem_close(alp_hwsem_t *s)
{
	(void)s;
}
const alp_capabilities_t *alp_hwsem_capabilities(const alp_hwsem_t *s)
{
	(void)s;
	return NULL;
}
alp_status_t alp_mproc_boot_core(alp_core_id_t core, uintptr_t entry_addr)
{
	(void)core;
	(void)entry_addr;
	return ALP_ERR_NOSUPPORT;
}

/* The Yocto backend overrides these via inference_yocto.c so the
 * dispatcher can route ALP_INFERENCE_BACKEND_DEEPX_DXM1 (etc.) to a
 * real NPU adapter -- in that case ALP_VENDOR_OVERRIDES_INFERENCE
 * is set and the stub bodies below are excluded from the link. */
#if !defined(ALP_VENDOR_OVERRIDES_INFERENCE)
alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
size_t alp_inference_num_inputs(alp_inference_t *i)
{
	(void)i;
	return 0u;
}
size_t alp_inference_num_outputs(alp_inference_t *i)
{
	(void)i;
	return 0u;
}
alp_status_t alp_inference_get_input(alp_inference_t *i, size_t idx, alp_inference_tensor_t *o)
{
	(void)i;
	(void)idx;
	if (o) *o = (alp_inference_tensor_t){ 0 };
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_inference_get_output(alp_inference_t *i, size_t idx, alp_inference_tensor_t *o)
{
	(void)i;
	(void)idx;
	if (o) *o = (alp_inference_tensor_t){ 0 };
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_inference_invoke(alp_inference_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
void alp_inference_close(alp_inference_t *i)
{
	(void)i;
}
#endif /* !ALP_VENDOR_OVERRIDES_INFERENCE */

/* Unguarded -- inference_yocto.c (ALP_VENDOR_OVERRIDES_INFERENCE=1 on
 * Yocto) routes open/invoke/etc. to the real NPU adapter but never
 * implements alp_inference_capabilities; only the Zephyr registry
 * dispatcher (src/inference_dispatch.c, never compiled outside Zephyr)
 * does (#593). */
const alp_capabilities_t *alp_inference_capabilities(const alp_inference_t *i)
{
	(void)i;
	return NULL;
}

alp_storage_t *alp_storage_open(const alp_storage_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_storage_get_info(alp_storage_t *storage, alp_storage_info_t *info)
{
	(void)storage;
	if (info) *info = (alp_storage_info_t){ 0 };
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_read(alp_storage_t *storage, uint64_t o, void *d, size_t l)
{
	(void)storage;
	(void)o;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_write(alp_storage_t *storage, uint64_t o, const void *d, size_t l)
{
	(void)storage;
	(void)o;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_erase(alp_storage_t *storage, uint64_t o, uint64_t l)
{
	(void)storage;
	(void)o;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_storage_sync(alp_storage_t *storage)
{
	(void)storage;
	return ALP_ERR_NOSUPPORT;
}
void alp_storage_close(alp_storage_t *storage)
{
	(void)storage;
}
const alp_capabilities_t *alp_storage_capabilities(const alp_storage_t *storage)
{
	(void)storage;
	return NULL;
}

alp_usb_dev_t *alp_usb_device_open(const alp_usb_device_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_usb_device_enable(alp_usb_dev_t *d)
{
	(void)d;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_device_disable(alp_usb_dev_t *d)
{
	(void)d;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_device_write(alp_usb_dev_t *d, const uint8_t *b, size_t l, uint32_t t)
{
	(void)d;
	(void)b;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_device_read(alp_usb_dev_t *d, uint8_t *b, size_t l, size_t *o, uint32_t t)
{
	(void)d;
	(void)b;
	(void)l;
	(void)t;
	if (o) *o = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_usb_device_close(alp_usb_dev_t *d)
{
	(void)d;
}
const alp_capabilities_t *alp_usb_capabilities(const alp_usb_dev_t *dev)
{
	(void)dev;
	return NULL;
}
alp_usb_host_t *alp_usb_host_open(void)
{
	return NULL;
}
alp_status_t alp_usb_host_enable(alp_usb_host_t *h)
{
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_usb_host_disable(alp_usb_host_t *h)
{
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
void alp_usb_host_close(alp_usb_host_t *h)
{
	(void)h;
}

/* ------------------------------------------------------------------ */
/* power (alp/power.h)                                                 */
/* ------------------------------------------------------------------ */

#if !defined(ALP_VENDOR_OVERRIDES_POWER)
alp_power_t *alp_power_open(void)
{
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_power_configure_wake_source(alp_power_t *p, uint32_t wake_bitmap)
{
	(void)p;
	(void)wake_bitmap;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_power_request_sleep(alp_power_t           *p,
                                     alp_power_mode_t       mode,
                                     uint32_t               wake_after_ms,
                                     alp_power_wake_info_t *info)
{
	(void)p;
	(void)mode;
	(void)wake_after_ms;
	(void)info;
	return ALP_ERR_NOSUPPORT;
}
void alp_power_close(alp_power_t *p)
{
	(void)p;
}
#endif /* !ALP_VENDOR_OVERRIDES_POWER */

/* ------------------------------------------------------------------ */
/* GPU2D (alp/gpu2d.h)                                                 */
/* ------------------------------------------------------------------ */

/* Muted when the OS backend compiles the real class dispatcher
 * (src/gpu2d_dispatch.c + the portable sw_fallback) -- the Yocto
 * build does, so Linux apps get the REAL CPU fill/blit/blend
 * instead of NOSUPPORT (same #33 migration pattern as rtc/wdt/
 * can/pwm/adc/i2s/counter above). */
#if !defined(ALP_VENDOR_OVERRIDES_GPU2D)
alp_gpu2d_t *alp_gpu2d_open(void)
{
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t               *g,
                                 const alp_gpu2d_surface_t *dst,
                                 uint32_t                   x,
                                 uint32_t                   y,
                                 uint32_t                   w,
                                 uint32_t                   h,
                                 uint32_t                   argb_color)
{
	(void)g;
	(void)dst;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)argb_color;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpu2d_blit(alp_gpu2d_t               *g,
                            const alp_gpu2d_surface_t *src,
                            uint32_t                   sx,
                            uint32_t                   sy,
                            const alp_gpu2d_surface_t *dst,
                            uint32_t                   dx,
                            uint32_t                   dy,
                            uint32_t                   w,
                            uint32_t                   h)
{
	(void)g;
	(void)src;
	(void)sx;
	(void)sy;
	(void)dst;
	(void)dx;
	(void)dy;
	(void)w;
	(void)h;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpu2d_blend(alp_gpu2d_t               *g,
                             const alp_gpu2d_surface_t *src,
                             uint32_t                   sx,
                             uint32_t                   sy,
                             const alp_gpu2d_surface_t *dst,
                             uint32_t                   dx,
                             uint32_t                   dy,
                             uint32_t                   w,
                             uint32_t                   h,
                             alp_gpu2d_blend_mode_t     mode)
{
	(void)g;
	(void)src;
	(void)sx;
	(void)sy;
	(void)dst;
	(void)dx;
	(void)dy;
	(void)w;
	(void)h;
	(void)mode;
	return ALP_ERR_NOSUPPORT;
}
void alp_gpu2d_close(alp_gpu2d_t *g)
{
	(void)g;
}
const alp_capabilities_t *alp_gpu2d_capabilities(const alp_gpu2d_t *g)
{
	(void)g;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_GPU2D */

/* ------------------------------------------------------------------ */
/* Camera ISP (alp/camera.h v0.5 extension)                            */
/* ------------------------------------------------------------------ */

#if !defined(ALP_VENDOR_OVERRIDES_CAMERA)
alp_status_t alp_camera_configure_isp(alp_camera_t *c, const alp_camera_isp_config_t *isp)
{
	if (isp == NULL) return ALP_ERR_INVAL;
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
#endif /* !ALP_VENDOR_OVERRIDES_CAMERA */

/* ------------------------------------------------------------------ */
/* Storage inline-AES (alp/storage.h v0.5 extension)                   */
/* ------------------------------------------------------------------ */

alp_status_t alp_storage_configure_inline_aes(alp_storage_t                  *storage,
                                              const alp_storage_aes_config_t *cfg)
{
	if (cfg == NULL) return ALP_ERR_INVAL;
	(void)storage;
	return ALP_ERR_NOSUPPORT;
}

/* ------------------------------------------------------------------ */
/* RPC (alp/rpc.h) -- framed RPC over OpenAMP / RPMsg.                  */
/*                                                                      */
/* Bare-metal has no OpenAMP transport, so <alp/rpc.h>'s own doc        */
/* comment calls out this exact NOSUPPORT stub as the bare-metal path.  */
/* Guarded (ALP_VENDOR_OVERRIDES_RPC): Yocto's rpc_dispatch.c (the      */
/* registry-pattern class owner, routing to backends/rpc/yocto_drv.c)  */
/* is compiled unconditionally in src/yocto/CMakeLists.txt and already  */
/* defines every alp_rpc_* symbol, so it mutes this block there to     */
/* avoid a duplicate-definition link error (#607 install-review build; */
/* the old top-of-block comment claiming "no vendor ever overrides     */
/* RPC" predates this stub existing at all). */
/* ------------------------------------------------------------------ */

#if !defined(ALP_VENDOR_OVERRIDES_RPC)
alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
void alp_rpc_close(alp_rpc_channel_t *ch)
{
	(void)ch;
}
const alp_capabilities_t *alp_rpc_capabilities(const alp_rpc_channel_t *ch)
{
	(void)ch;
	return NULL;
}
alp_status_t
alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	(void)ch;
	(void)method;
	(void)cb;
	(void)user;
	return ALP_ERR_NOT_READY;
}
alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
	(void)ch;
	(void)method;
	return ALP_ERR_NOT_READY;
}
alp_status_t
alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload, size_t len)
{
	(void)ch;
	(void)method;
	(void)payload;
	(void)len;
	return ALP_ERR_NOT_READY;
}
alp_status_t alp_rpc_call(alp_rpc_channel_t *ch,
                          const char        *method,
                          const void        *req,
                          size_t             req_len,
                          void              *resp,
                          size_t            *resp_len,
                          uint32_t           timeout_ms)
{
	(void)ch;
	(void)method;
	(void)req;
	(void)req_len;
	(void)resp;
	(void)timeout_ms;
	if (resp_len != NULL) *resp_len = 0;
	return ALP_ERR_NOT_READY;
}
#endif /* !ALP_VENDOR_OVERRIDES_RPC */

/* ------------------------------------------------------------------ */
/* Model reader (alp/model.h) -- .alpmodel container parser.            */
/*                                                                      */
/* The real body (src/common/alp_model.c) decodes the CBOR manifest     */
/* via zcbor, a Zephyr-only west module today (see west.yml) -- no      */
/* plain-CMake (baremetal/yocto) build vendors it, so alp_model_parse   */
/* has no non-Zephyr implementation yet.  This is an explicit,          */
/* documented stub (issue #593), not an oversight: baremetal/yocto      */
/* apps that call alp_model_parse directly get ALP_ERR_NOSUPPORT.       */
/* alp_inference_open_alpmodel (src/common/alp_model_loader.c) is       */
/* OS-agnostic and already degrades to its own NOSUPPORT body when      */
/* CONFIG_ALP_SDK_MODEL_READER is unset, so it's compiled for real      */
/* (not stubbed) on every OS -- see src/baremetal/CMakeLists.txt /       */
/* src/yocto/CMakeLists.txt.                                            */
/* ------------------------------------------------------------------ */
alp_status_t alp_model_parse(const uint8_t *data, size_t size, alp_model_t *out)
{
	(void)data;
	(void)size;
	(void)out;
	return ALP_ERR_NOSUPPORT;
}
