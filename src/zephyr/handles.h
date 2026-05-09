/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal opaque-handle definitions for the Zephyr ALP SDK backend.
 * Not part of the public surface — application code must include
 * <alp/peripheral.h> instead.
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
/* Internal pool API — used only by the per-peripheral source files.   */
/* ------------------------------------------------------------------ */

struct alp_i2c   *alp_z_i2c_pool_acquire(void);
void              alp_z_i2c_pool_release(struct alp_i2c *h);

struct alp_spi   *alp_z_spi_pool_acquire(void);
void              alp_z_spi_pool_release(struct alp_spi *h);

struct alp_uart  *alp_z_uart_pool_acquire(void);
void              alp_z_uart_pool_release(struct alp_uart *h);

struct alp_gpio  *alp_z_gpio_pool_acquire(void);
void              alp_z_gpio_pool_release(struct alp_gpio *h);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_INTERNAL_ZEPHYR_HANDLES_H_ */
