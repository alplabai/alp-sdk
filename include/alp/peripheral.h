/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file peripheral.h
 * @brief ALP SDK peripheral abstraction (I2C, SPI, GPIO, UART).
 *
 * Thin C99 surface that the alp-studio pin allocator targets.  The studio
 * picks a peripheral instance per block at codegen time; this header is
 * what the generated init/usage code (and any hand-written application
 * code) calls into.
 *
 * Each peripheral handle is opaque.  The OS-pivoted backend
 * (src/zephyr, src/baremetal, src/yocto) materialises the struct and
 * routes through the vendor wrapper (vendors/alif, vendors/renesas-rzv2n).
 *
 * v0.1 surface — the function bodies are not yet implemented.  The shape
 * is what blocks (blk_button_led, blk_oled_ssd1306, blk_imu_lsm6dso) need.
 */

#ifndef ALP_PERIPHERAL_H
#define ALP_PERIPHERAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Status codes returned by ALP peripheral functions. */
typedef enum {
    ALP_OK              = 0,
    ALP_ERR_INVAL       = -1,   /**< Invalid argument. */
    ALP_ERR_NOT_READY   = -2,   /**< Peripheral not initialised. */
    ALP_ERR_BUSY        = -3,   /**< Peripheral busy. */
    ALP_ERR_TIMEOUT     = -4,   /**< Transfer timed out. */
    ALP_ERR_IO          = -5,   /**< Bus / line error. */
    ALP_ERR_NOSUPPORT   = -6,   /**< Backend lacks this feature. */
    ALP_ERR_NOMEM       = -7    /**< Allocation failure. */
} alp_status_t;

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

/** Pin direction. */
typedef enum {
    ALP_GPIO_INPUT      = 0,
    ALP_GPIO_OUTPUT     = 1
} alp_gpio_dir_t;

/** Pin pull configuration. */
typedef enum {
    ALP_GPIO_PULL_NONE  = 0,
    ALP_GPIO_PULL_UP    = 1,
    ALP_GPIO_PULL_DOWN  = 2
} alp_gpio_pull_t;

/** Edge for interrupt-on-change. */
typedef enum {
    ALP_GPIO_EDGE_NONE      = 0,
    ALP_GPIO_EDGE_RISING    = 1,
    ALP_GPIO_EDGE_FALLING   = 2,
    ALP_GPIO_EDGE_BOTH      = 3
} alp_gpio_edge_t;

typedef struct alp_gpio alp_gpio_t;

typedef void (*alp_gpio_cb_t)(alp_gpio_t *pin, void *user);

/**
 * @brief Acquire a GPIO handle for the given studio-resolved pin id.
 *
 * @param pin_id  Implementation-defined pin id supplied by the
 *                alp-studio pin allocator.  On Zephyr this typically
 *                indexes into a generated devicetree label table.
 * @return  GPIO handle, or NULL on error.
 */
alp_gpio_t *alp_gpio_open(uint32_t pin_id);

alp_status_t alp_gpio_configure(alp_gpio_t *pin,
                                alp_gpio_dir_t dir,
                                alp_gpio_pull_t pull);

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level);
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level);

alp_status_t alp_gpio_irq_enable(alp_gpio_t *pin,
                                 alp_gpio_edge_t edge,
                                 alp_gpio_cb_t cb,
                                 void *user);
alp_status_t alp_gpio_irq_disable(alp_gpio_t *pin);

void alp_gpio_close(alp_gpio_t *pin);

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

typedef struct alp_i2c alp_i2c_t;

typedef struct {
    uint32_t bus_id;        /**< Studio-resolved bus instance id. */
    uint32_t bitrate_hz;    /**< 100k / 400k / 1M typical. */
} alp_i2c_config_t;

alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg);

/** 7-bit-address blocking write. */
alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr,
                           const uint8_t *data, size_t len);

/** 7-bit-address blocking read. */
alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr,
                          uint8_t *data, size_t len);

/** Write-then-read (typical register read pattern). */
alp_status_t alp_i2c_write_read(alp_i2c_t *bus, uint8_t addr,
                                const uint8_t *wdata, size_t wlen,
                                uint8_t *rdata, size_t rlen);

void alp_i2c_close(alp_i2c_t *bus);

/* ------------------------------------------------------------------ */
/* SPI                                                                 */
/* ------------------------------------------------------------------ */

typedef struct alp_spi alp_spi_t;

typedef enum {
    ALP_SPI_MODE_0 = 0,     /**< CPOL=0, CPHA=0 */
    ALP_SPI_MODE_1 = 1,     /**< CPOL=0, CPHA=1 */
    ALP_SPI_MODE_2 = 2,     /**< CPOL=1, CPHA=0 */
    ALP_SPI_MODE_3 = 3      /**< CPOL=1, CPHA=1 */
} alp_spi_mode_t;

typedef struct {
    uint32_t bus_id;
    uint32_t freq_hz;
    alp_spi_mode_t mode;
    uint8_t  bits_per_word; /**< Usually 8. */
    uint32_t cs_pin_id;     /**< Studio-resolved chip-select pin. */
} alp_spi_config_t;

alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg);

alp_status_t alp_spi_transceive(alp_spi_t *bus,
                                const uint8_t *tx, uint8_t *rx,
                                size_t len);

alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len);
alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len);

void alp_spi_close(alp_spi_t *bus);

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

typedef struct alp_uart alp_uart_t;

typedef enum {
    ALP_UART_PARITY_NONE = 0,
    ALP_UART_PARITY_EVEN = 1,
    ALP_UART_PARITY_ODD  = 2
} alp_uart_parity_t;

typedef struct {
    uint32_t port_id;
    uint32_t baudrate;
    uint8_t  data_bits;     /**< Usually 8. */
    uint8_t  stop_bits;     /**< 1 or 2. */
    alp_uart_parity_t parity;
} alp_uart_config_t;

alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg);

alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len);
alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len,
                           uint32_t timeout_ms);

void alp_uart_close(alp_uart_t *port);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_PERIPHERAL_H */
