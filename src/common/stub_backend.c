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

#include "alp_internal.h"

/* ------------------------------------------------------------------ */
/* alp_last_error — single-global fallback (no TLS on baremetal).      */
/*                                                                      */
/* Owns the process-wide last-error slot.  Cross-TU writers go through  */
/* alp_internal_set_last_error (declared in alp_internal.h); local      */
/* writers in this file write z_last_error directly for brevity.       */
/* ------------------------------------------------------------------ */

static alp_status_t z_last_error;

#if !defined(ALP_VENDOR_OVERRIDES_PERIPHERAL)
alp_status_t alp_last_error(void)
{
	return z_last_error;
}
#endif
/* When ALP_VENDOR_OVERRIDES_PERIPHERAL=1 the vendor wrapper provides
 * its own alp_last_error reader against a vendor-side static; we
 * keep z_last_error around for the non-peripheral stubs in this
 * file to write into, but reads from it aren't reachable through
 * the public API in that configuration. */

void alp_internal_set_last_error(alp_status_t s)
{
	z_last_error = s;
}

/* ------------------------------------------------------------------ */
/* Delay primitives -- minimal calibrated busy-loop fallback.          */
/*                                                                     */
/* Yocto + baremetal don't (yet) ship a real impl, so the public       */
/* alp_delay_us / alp_delay_ms surface lands here as a portable spin   */
/* loop.  Iteration count is rough -- the function-call + bounds-check */
/* overhead dominates at high us values; tuning is intentionally       */
/* avoided to keep stub_backend.c portable across SoCs.  Apps that     */
/* need cycle-accurate sub-microsecond timing should run on Zephyr     */
/* (where the backend dispatches to k_busy_wait) or wait for the       */
/* per-backend HAL bodies to land.                                     */
/* ------------------------------------------------------------------ */

void alp_delay_us(uint32_t us)
{
	if (us == 0u) return;
	/* ~10 NOPs per us at ~1 GHz with -O0 is in the right ballpark for
     * SoCs in the Apache-2.0 target list.  Volatile prevents the
     * optimiser from collapsing the loop. */
	volatile uint32_t spin = us * 10u;
	while (spin != 0u) {
		--spin;
	}
}

void alp_delay_ms(uint32_t ms)
{
	if (ms == 0u) return;
	/* Defer to the us path so a future calibration improvement
     * benefits both surfaces simultaneously. */
	for (uint32_t i = 0u; i < ms; i++) {
		alp_delay_us(1000u);
	}
}

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
#endif /* !ALP_VENDOR_OVERRIDES_ADC */

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
#endif /* !ALP_VENDOR_OVERRIDES_RTC */

#if !defined(ALP_VENDOR_OVERRIDES_WDT)
alp_wdt_t *alp_wdt_open(uint32_t id, const alp_wdt_config_t *cfg)
{
	(void)id;
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
#endif /* !ALP_VENDOR_OVERRIDES_WDT */

/* ------------------------------------------------------------------ */
/* Higher libraries (camera, iot, audio, ble, security, mproc, display) */
/* ------------------------------------------------------------------ */

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
#endif /* !ALP_VENDOR_OVERRIDES_AUDIO_OUT */

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

/* ------------------------------------------------------------------ */
/* GPU2D (alp/gpu2d.h)                                                 */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Camera ISP (alp/camera.h v0.5 extension)                            */
/* ------------------------------------------------------------------ */

alp_status_t alp_camera_configure_isp(alp_camera_t *c, const alp_camera_isp_config_t *isp)
{
	if (isp == NULL) return ALP_ERR_INVAL;
	(void)c;
	return ALP_ERR_NOSUPPORT;
}

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
