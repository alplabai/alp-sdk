/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal opaque-handle definitions for the Zephyr Alp SDK backend.
 * Not part of the public surface — application code must include
 * <alp/peripheral.h>, <alp/pwm.h>, etc. instead.
 */

#ifndef ALP_INTERNAL_ZEPHYR_HANDLES_H_
#define ALP_INTERNAL_ZEPHYR_HANDLES_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/adc.h"
#include "alp/counter.h"
#include "alp/i2s.h"
#include "alp/can.h"
#include "alp/rtc.h"
#include "alp/wdt.h"

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#include <zephyr/drivers/dac.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Pool sizes (Kconfig-tunable; zephyr/Kconfig defines defaults).      */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_I2C_HANDLES
#define CONFIG_ALP_SDK_MAX_I2C_HANDLES   4
#endif
#ifndef CONFIG_ALP_SDK_MAX_SPI_HANDLES
#define CONFIG_ALP_SDK_MAX_SPI_HANDLES   4
#endif
#ifndef CONFIG_ALP_SDK_MAX_UART_HANDLES
#define CONFIG_ALP_SDK_MAX_UART_HANDLES  4
#endif
#ifndef CONFIG_ALP_SDK_MAX_GPIO_HANDLES
#define CONFIG_ALP_SDK_MAX_GPIO_HANDLES  16
#endif
#ifndef CONFIG_ALP_SDK_MAX_PWM_HANDLES
#define CONFIG_ALP_SDK_MAX_PWM_HANDLES   8
#endif
#ifndef CONFIG_ALP_SDK_MAX_ADC_HANDLES
#define CONFIG_ALP_SDK_MAX_ADC_HANDLES   8
#endif
#ifndef CONFIG_ALP_SDK_MAX_COUNTER_HANDLES
#define CONFIG_ALP_SDK_MAX_COUNTER_HANDLES 4
#endif
#ifndef CONFIG_ALP_SDK_MAX_QENC_HANDLES
#define CONFIG_ALP_SDK_MAX_QENC_HANDLES  4
#endif
#ifndef CONFIG_ALP_SDK_MAX_I2S_HANDLES
#define CONFIG_ALP_SDK_MAX_I2S_HANDLES   2
#endif
#ifndef CONFIG_ALP_SDK_MAX_CAN_HANDLES
#define CONFIG_ALP_SDK_MAX_CAN_HANDLES   4
#endif
#ifndef CONFIG_ALP_SDK_MAX_RTC_HANDLES
#define CONFIG_ALP_SDK_MAX_RTC_HANDLES   2
#endif
#ifndef CONFIG_ALP_SDK_MAX_WDT_HANDLES
#define CONFIG_ALP_SDK_MAX_WDT_HANDLES   2
#endif
#ifndef CONFIG_ALP_SDK_MAX_DAC_HANDLES
#define CONFIG_ALP_SDK_MAX_DAC_HANDLES   2
#endif
#ifndef CONFIG_ALP_SDK_MAX_ADC_STREAM_HANDLES
#define CONFIG_ALP_SDK_MAX_ADC_STREAM_HANDLES 2
#endif

/* ------------------------------------------------------------------ */
/* UART RX ring buffer                                                 */
/* ------------------------------------------------------------------ */

/* RX ring buffer state -- only compiled when the feature is enabled
 * so builds without CONFIG_ALP_SDK_UART_RX_RINGBUF don't pull in the
 * LwRB header from this internal include. */
#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)
#include <lwrb/lwrb.h>

struct alp_uart_rx_ringbuf {
    bool                 in_use;
    const struct device *dev;     /* mirror of port->state.dev for ISR use */
    struct alp_uart     *port;    /* back-ref for detach */
    lwrb_t               rb;
};
#endif

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

/* struct alp_gpio is defined in src/backends/gpio/gpio_ops.h.         */

/* ------------------------------------------------------------------ */
/* PWM                                                                 */
/* ------------------------------------------------------------------ */

/* struct alp_pwm is defined in src/backends/pwm/pwm_ops.h.            */

/* ------------------------------------------------------------------ */
/* ADC                                                                 */
/* ------------------------------------------------------------------ */

/* struct alp_adc is defined in src/backends/adc/adc_ops.h.             */

/* struct alp_counter is defined in src/backends/counter/counter_ops.h. */

/* struct alp_qenc is defined in src/backends/qenc/qenc_ops.h.          */

/* ------------------------------------------------------------------ */
/* I2S                                                                 */
/* ------------------------------------------------------------------ */

/* struct alp_i2s is defined in src/backends/i2s/i2s_ops.h.             */

/* ------------------------------------------------------------------ */
/* CAN                                                                 */
/* ------------------------------------------------------------------ */

/* struct alp_can is defined in src/backends/can/can_ops.h.             */

/* struct alp_rtc is defined in src/backends/rtc/rtc_ops.h.             */

/* struct alp_wdt is defined in src/backends/wdt/wdt_ops.h.             */

/* ------------------------------------------------------------------ */
/* DAC                                                                 */
/*                                                                     */
/* `dev == NULL` -> dispatch through the V2N supervisor singleton      */
/* (CONFIG_ALP_SDK_V2N_SUPERVISOR).  Non-NULL `dev` -> resolve via the */
/* alp-dacN DT alias path on the local SoC's Zephyr dac_* driver.      */
/* ------------------------------------------------------------------ */

struct alp_dac {
    bool                  in_use;
    uint32_t              channel_id;
    const struct device  *dev;            /* NULL -> via_bridge */
    uint8_t               channel;        /* hardware channel id when dev != NULL */
    uint16_t              last_mv;        /* most recently programmed setpoint   */
};

/* ------------------------------------------------------------------ */
/* Streaming ADC                                                       */
/*                                                                     */
/* `via_bridge` -> dispatch through the V2N supervisor singleton       */
/* (CONFIG_ALP_SDK_V2N_SUPERVISOR; same singleton serves V2N + V2N-M1, */
/* both of which carry the GD32G553).  Non-bridge SoMs surface         */
/* ALP_ERR_NOSUPPORT at open time; the struct is still defined so the  */
/* pool + handle plumbing compile cleanly without #if guards.          */
/* ------------------------------------------------------------------ */

struct alp_adc_stream {
    bool     in_use;
    bool     via_bridge;
    uint8_t  stream_id; /* backend slot index (0..1 on the V2N family) */
    uint8_t  channel;   /* hardware channel id */
    uint32_t channel_id;
    uint32_t sample_rate_hz;
};

/* ------------------------------------------------------------------ */
/* Last-error helpers — internal use only.                              */
/* Stamps a precise alp_status_t before alp_*_open returns NULL.        */
/* ------------------------------------------------------------------ */

void alp_z_set_last_error(alp_status_t s);
void alp_z_clear_last_error(void);

/* ------------------------------------------------------------------ */
/* Internal pool API — used only by the per-peripheral source files.   */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)
struct alp_uart_rx_ringbuf *alp_z_uart_rx_ringbuf_pool_acquire(void);
void                        alp_z_uart_rx_ringbuf_pool_release(struct alp_uart_rx_ringbuf *h);
#endif

struct alp_dac     *alp_z_dac_pool_acquire(void);
void                alp_z_dac_pool_release(struct alp_dac *h);

struct alp_adc_stream *alp_z_adc_stream_pool_acquire(void);
void                   alp_z_adc_stream_pool_release(struct alp_adc_stream *h);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_INTERNAL_ZEPHYR_HANDLES_H_ */
