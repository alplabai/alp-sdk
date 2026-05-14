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
 * Zephyr backends for all four classes (I2C, SPI, GPIO, UART) ship
 * in `src/zephyr/peripheral_{i2c,spi,gpio,uart}.c`; the Yocto and
 * baremetal backends fill in alongside their respective build
 * trees.
 *

 * @par ABI status: [ABI-STABLE]
 *      v0.1 surface; locked across every release since v0.1.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_PERIPHERAL_H
#define ALP_PERIPHERAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Pixel format shared by the display and camera APIs. */
typedef enum {
    ALP_PIXFMT_MONO_VLSB = 0,   /**< 1 bpp, vertical bytes (SSD1306 native). */
    ALP_PIXFMT_RGB565    = 1,
    ALP_PIXFMT_RGB888    = 2,
    ALP_PIXFMT_ARGB8888  = 3
} alp_pixfmt_t;

/** Status codes returned by ALP peripheral functions. */
typedef enum {
    ALP_OK              = 0,
    ALP_ERR_INVAL       = -1,   /**< Invalid argument. */
    ALP_ERR_NOT_READY   = -2,   /**< Peripheral not initialised. */
    ALP_ERR_BUSY        = -3,   /**< Peripheral busy. */
    ALP_ERR_TIMEOUT     = -4,   /**< Transfer timed out. */
    ALP_ERR_IO          = -5,   /**< Bus / line error. */
    ALP_ERR_NOSUPPORT   = -6,   /**< Backend lacks this feature. */
    ALP_ERR_NOMEM       = -7,   /**< Allocation failure. */
    ALP_ERR_OUT_OF_RANGE = -8   /**< Config exceeds the SoC's documented hardware caps. */
} alp_status_t;

/**
 * @brief Read the most recent error encountered on this thread.
 *
 * `alp_*_open` functions return NULL on failure for the v0.1 ABI
 * stability reason; this helper lets callers learn *why*.  Common
 * cases:
 *
 *   - @ref ALP_ERR_INVAL        — NULL config, bus_id out of range
 *   - @ref ALP_ERR_NOT_READY    — DT alias unset or device not ready
 *   - @ref ALP_ERR_NOMEM        — handle pool exhausted
 *   - @ref ALP_ERR_OUT_OF_RANGE — config exceeds the active SoC's caps
 *                                  (e.g. 16-bit ADC requested on a SoC
 *                                  whose ADC tops out at 12 bits)
 *   - @ref ALP_ERR_NOSUPPORT    — Zephyr driver returned -ENOTSUP
 *
 * The value is **thread-local** — concurrent open() calls on
 * different threads don't clobber each other's diagnostic.  A
 * successful open() on a thread clears its slot.
 *
 * @return The thread's last error code, or @ref ALP_OK if no error
 *         has been recorded since the last successful open().
 */
alp_status_t alp_last_error(void);

/* ------------------------------------------------------------------ */
/* Delay primitives                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Busy-wait for at least @p us microseconds.
 *
 * Backends use the platform's cycle-accurate spin primitive
 * (Zephyr's @c k_busy_wait, Yocto's @c clock_nanosleep with a
 * busy-loop fallback, vendor-HAL spin on baremetal).  Does not
 * yield to other threads -- callers needing scheduler-friendly
 * waits should use @ref alp_delay_ms for sub-thread-tick durations
 * or k_msleep / equivalent directly.
 *
 * Useful for sub-millisecond hardware-timing sequences (chip
 * power-on hold times, bus deassert intervals, post-write settle
 * delays).  Precision is platform-defined; the contract is
 * "at least @p us microseconds elapse before return".
 *
 * @param[in] us  Microseconds to spin.  0 = no-op.
 */
void alp_delay_us(uint32_t us);

/**
 * @brief Sleep the calling thread for at least @p ms milliseconds.
 *
 * Yields to the scheduler so other threads can run during the
 * wait (unlike @ref alp_delay_us).  On baremetal this falls
 * through to a calibrated busy-loop since there's no scheduler.
 *
 * @param[in] ms  Milliseconds to sleep.  0 = no-op.
 */
void alp_delay_ms(uint32_t ms);

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

/** @brief Configure a pin's direction + pull-up / pull-down resistors. */
alp_status_t alp_gpio_configure(alp_gpio_t *pin,
                                alp_gpio_dir_t dir,
                                alp_gpio_pull_t pull);

/** @brief Drive an output pin to @p level. */
alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level);
/** @brief Read the current level of an input pin. */
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level);

/** @brief Register an edge-triggered callback for @p pin. */
alp_status_t alp_gpio_irq_enable(alp_gpio_t *pin,
                                 alp_gpio_edge_t edge,
                                 alp_gpio_cb_t cb,
                                 void *user);
/** @brief Tear down the IRQ registration from @ref alp_gpio_irq_enable. */
alp_status_t alp_gpio_irq_disable(alp_gpio_t *pin);

/** @brief Release the GPIO handle.  Idempotent on NULL. */
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

/** @brief Release the I2C bus handle.  Idempotent on NULL. */
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

/** @brief Half-duplex SPI write (no MISO read). */
alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len);
/** @brief Half-duplex SPI read (no MOSI write). */
alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len);

/** @brief Release the SPI bus handle.  Idempotent on NULL. */
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

/** @brief Blocking UART write. */
alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len);
/** @brief Blocking UART read with millisecond timeout. */
alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len,
                           uint32_t timeout_ms);

/** @brief Release the UART port handle.  Idempotent on NULL. */
void alp_uart_close(alp_uart_t *port);

/* ------------------------------------------------------------------ */
/* UART -- byte-granular RX ring buffer (optional)                     */
/*                                                                     */
/* Opt-in helper layered on top of an open alp_uart_t.  Once attached, */
/* the SDK's interrupt-driven RX path drains the controller FIFO into  */
/* a caller-supplied backing store on every byte; the consumer thread  */
/* pops bytes via alp_uart_rx_ringbuf_pop without polling the device.  */
/*                                                                     */
/* Availability:                                                       */
/*   - Zephyr: requires CONFIG_ALP_SDK_UART_RX_RINGBUF=y plus           */
/*     CONFIG_UART_INTERRUPT_DRIVEN=y.  Other backends return           */
/*     NULL / ALP_ERR_NOSUPPORT today (Yocto-side termios already      */
/*     buffers in-kernel; baremetal lands once UART IRQ wiring does).  */
/*                                                                     */
/* Concurrency: alp_uart_rx_ringbuf_pop / _count run from the          */
/* consumer thread only; the producer side (UART IRQ) runs inside      */
/* the SDK and uses LwRB's single-producer-single-consumer guarantees. */
/* ------------------------------------------------------------------ */

typedef struct alp_uart_rx_ringbuf alp_uart_rx_ringbuf_t;

/**
 * @brief Attach a byte-granular RX ring buffer to an open UART port.
 *
 * @param port           UART handle from alp_uart_open().
 * @param backing        Caller-owned buffer; must outlive the returned
 *                       handle.  Should be >= 64 bytes to absorb the
 *                       worst-case FIFO drain latency.
 * @param backing_size   Capacity in bytes (one byte is reserved for
 *                       empty / full disambiguation, so usable
 *                       capacity is @p backing_size - 1).
 *
 * @return Handle on success; NULL with alp_last_error() set on failure.
 *         Returns NULL + ALP_ERR_NOSUPPORT on builds without
 *         CONFIG_ALP_SDK_UART_RX_RINGBUF.
 */
alp_uart_rx_ringbuf_t *alp_uart_rx_ringbuf_attach(alp_uart_t *port,
                                                  uint8_t    *backing,
                                                  size_t      backing_size);

/**
 * @brief Pop up to @p max_len bytes out of the ring.
 *
 * Non-blocking.  Writes the number of bytes actually copied into
 * @p got (may be zero if the ring is empty).
 *
 * @param rb         Handle from alp_uart_rx_ringbuf_attach.
 * @param out        Caller-supplied destination buffer.
 * @param max_len    Maximum bytes to copy.
 * @param got        [out] Bytes actually copied; may be NULL.
 *
 * @return ALP_OK on success.  ALP_ERR_NOT_READY if @p rb is NULL or
 *         detached.  ALP_ERR_INVAL if @p out is NULL with @p max_len > 0.
 */
alp_status_t alp_uart_rx_ringbuf_pop(alp_uart_rx_ringbuf_t *rb,
                                     uint8_t *out, size_t max_len,
                                     size_t *got);

/**
 * @brief Number of bytes currently buffered.
 *
 * Safe to call from the consumer thread.  Returns zero on a NULL or
 * detached handle.
 *
 * @param rb   Handle from alp_uart_rx_ringbuf_attach.
 * @return Buffered byte count (range 0 .. backing_size - 1).
 */
size_t alp_uart_rx_ringbuf_count(const alp_uart_rx_ringbuf_t *rb);

/**
 * @brief Detach the ring buffer and release the handle.
 *
 * Disables the IRQ-driven RX path on the underlying port and returns
 * the slot to the pool.  Idempotent on NULL.  The caller's backing
 * store may be reused or freed once this returns.
 *
 * @param rb   Handle from alp_uart_rx_ringbuf_attach.
 */
void alp_uart_rx_ringbuf_detach(alp_uart_rx_ringbuf_t *rb);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_PERIPHERAL_H */
