/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal opaque-handle definitions for the Zephyr ALP SDK backend.
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
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

struct alp_i2c {
    bool                in_use;
    uint32_t            bus_id;
    const struct device *dev;
    alp_i2c_config_t    cfg;
};

/* ------------------------------------------------------------------ */
/* SPI                                                                 */
/* ------------------------------------------------------------------ */

struct alp_spi {
    bool                in_use;
    uint32_t            bus_id;
    const struct device *dev;
    alp_spi_config_t    cfg;
    struct spi_config   zspi_cfg;
    struct gpio_dt_spec cs_spec;        /* zeroed when no CS gpio resolved */
    bool                cs_present;
    struct spi_cs_control cs_ctrl;
};

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

struct alp_uart {
    bool                in_use;
    uint32_t            port_id;
    const struct device *dev;
    alp_uart_config_t   cfg;
};

/* RX ring buffer state -- only compiled when the feature is enabled
 * so builds without CONFIG_ALP_SDK_UART_RX_RINGBUF don't pull in the
 * LwRB header from this internal include. */
#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)
#include <lwrb/lwrb.h>

struct alp_uart_rx_ringbuf {
    bool                 in_use;
    const struct device *dev;     /* mirror of port->dev for ISR use */
    struct alp_uart     *port;    /* back-ref for detach */
    lwrb_t               rb;
};
#endif

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

struct alp_gpio {
    bool                  in_use;
    uint32_t              pin_id;
    struct gpio_dt_spec   spec;
    alp_gpio_dir_t        dir;
    alp_gpio_pull_t       pull;
    alp_gpio_edge_t       edge;
    alp_gpio_cb_t         cb;
    void                  *cb_user;
    struct gpio_callback  zcb;
};

/* ------------------------------------------------------------------ */
/* PWM                                                                 */
/* ------------------------------------------------------------------ */

struct alp_pwm {
    bool                  in_use;
    uint32_t              channel_id;
    const struct device  *dev;
    uint32_t              channel;       /* hardware channel within @ref dev */
    uint32_t              period_ns;
    uint32_t              flags;         /* zephyr pwm_flags_t */
};

/* ------------------------------------------------------------------ */
/* ADC                                                                 */
/* ------------------------------------------------------------------ */

struct alp_adc {
    bool                  in_use;
    uint32_t              channel_id;
    const struct device  *dev;
    uint8_t               channel;       /* hardware channel id */
    uint8_t               resolution;
    uint8_t               oversampling;
    uint8_t               gain;          /* zephyr adc_gain encoded enum */
    uint8_t               reference;     /* zephyr adc_reference encoded enum */
    uint16_t              vref_mv;
    uint16_t              acquisition_us;
    int16_t               sample_buf;    /* room for one sample */
};

/* ------------------------------------------------------------------ */
/* Counter                                                             */
/* ------------------------------------------------------------------ */

struct alp_counter {
    bool                       in_use;
    uint32_t                   counter_id;
    const struct device       *dev;
    alp_counter_alarm_cb_t     alarm_cb;
    void                      *alarm_user;
};

/* ------------------------------------------------------------------ */
/* Quadrature encoder                                                  */
/* ------------------------------------------------------------------ */

struct alp_qenc {
    bool                  in_use;
    uint32_t              encoder_id;
    const struct device  *dev;
    int32_t               last_position;  /* monotonic accumulator */
};

/* ------------------------------------------------------------------ */
/* I2S                                                                 */
/* ------------------------------------------------------------------ */

struct alp_i2s {
    bool                  in_use;
    uint32_t              bus_id;
    const struct device  *dev;
    alp_i2s_config_t      cfg;
    struct k_mem_slab     mem_slab;
    uint8_t              *slab_buf;       /* allocated lazily on first start */
    size_t                slab_buf_bytes;
    bool                  started;
};

/* ------------------------------------------------------------------ */
/* CAN                                                                 */
/* ------------------------------------------------------------------ */

struct alp_can {
    bool                  in_use;
    uint32_t              bus_id;
    const struct device  *dev;
    alp_can_config_t      cfg;
    bool                  started;
};

/* ------------------------------------------------------------------ */
/* RTC                                                                 */
/* ------------------------------------------------------------------ */

struct alp_rtc {
    bool                  in_use;
    uint32_t              rtc_id;
    const struct device  *dev;
};

/* ------------------------------------------------------------------ */
/* Watchdog                                                            */
/* ------------------------------------------------------------------ */

struct alp_wdt {
    bool                  in_use;
    uint32_t              wdt_id;
    const struct device  *dev;
    int                   channel_id;     /* zephyr wdt_install_timeout return */
    alp_wdt_config_t      cfg;
};

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

struct alp_i2c     *alp_z_i2c_pool_acquire(void);
void                alp_z_i2c_pool_release(struct alp_i2c *h);

struct alp_spi     *alp_z_spi_pool_acquire(void);
void                alp_z_spi_pool_release(struct alp_spi *h);

struct alp_uart    *alp_z_uart_pool_acquire(void);
void                alp_z_uart_pool_release(struct alp_uart *h);

#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)
struct alp_uart_rx_ringbuf *alp_z_uart_rx_ringbuf_pool_acquire(void);
void                        alp_z_uart_rx_ringbuf_pool_release(struct alp_uart_rx_ringbuf *h);
#endif

struct alp_gpio    *alp_z_gpio_pool_acquire(void);
void                alp_z_gpio_pool_release(struct alp_gpio *h);

struct alp_pwm     *alp_z_pwm_pool_acquire(void);
void                alp_z_pwm_pool_release(struct alp_pwm *h);

struct alp_adc     *alp_z_adc_pool_acquire(void);
void                alp_z_adc_pool_release(struct alp_adc *h);

struct alp_counter *alp_z_counter_pool_acquire(void);
void                alp_z_counter_pool_release(struct alp_counter *h);

struct alp_qenc    *alp_z_qenc_pool_acquire(void);
void                alp_z_qenc_pool_release(struct alp_qenc *h);

struct alp_i2s     *alp_z_i2s_pool_acquire(void);
void                alp_z_i2s_pool_release(struct alp_i2s *h);

struct alp_can     *alp_z_can_pool_acquire(void);
void                alp_z_can_pool_release(struct alp_can *h);

struct alp_rtc     *alp_z_rtc_pool_acquire(void);
void                alp_z_rtc_pool_release(struct alp_rtc *h);

struct alp_wdt     *alp_z_wdt_pool_acquire(void);
void                alp_z_wdt_pool_release(struct alp_wdt *h);

struct alp_dac     *alp_z_dac_pool_acquire(void);
void                alp_z_dac_pool_release(struct alp_dac *h);

struct alp_adc_stream *alp_z_adc_stream_pool_acquire(void);
void                   alp_z_adc_stream_pool_release(struct alp_adc_stream *h);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_INTERNAL_ZEPHYR_HANDLES_H_ */
