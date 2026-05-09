/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file e1m_pinout.h
 * @brief Global, fixed pin/instance map derived from the E1M
 *        standard.
 *
 * The E1M open standard (`alpCaner/e1m-spec`) fixes the carrier-side
 * pinout: which peripheral instances exist (`I2C0`, `SPI0`, …) and
 * which physical pads carry their default function plus the
 * GPIO-secondary IO numbers (`IO0`, `IO1`, …, `PWM0`–`PWM7`,
 * `ENC0`–`ENC3`).  Because that mapping is **silicon- and
 * carrier-agnostic**, the integers the SDK consumes can be baked
 * here as portable C macros — every E1M-conformant carrier
 * resolves them identically.
 *
 * Derived from `alpCaner/e1m-spec/pinout/v1.json` (E1M v1.0,
 * 35 × 35 mm, 312 pads).  Pinned to `e1m-spec` v1.0.
 *
 * ## Per-carrier feature names
 *
 * Names like `USER_LED_RED` or `ENCODER_SW` are **not** in E1M —
 * they're carrier-specific.  Those live in
 * `<alp/boards/<board>.h>` and reference the macros below for
 * their underlying pin/instance integers.
 *
 * ## Devicetree / overlay invariant
 *
 * The order of entries in a board's `alp,pin-array` `gpios`
 * property MUST match the GPIO-pad enumeration here:
 *
 *   1. `IO0`–`IO25` in numeric order
 *   2. `PWM0`–`PWM7` in numeric order
 *   3. `ENC0_X`, `ENC0_Y`, `ENC1_X`, `ENC1_Y`, … `ENC3_Y`
 *
 * Carriers that don't route a particular pad still leave its slot
 * in `alp,pin-array` (with `status = "disabled"` on the GPIO node)
 * so the indices stay stable.  `alp_gpio_open()` for a non-routed
 * pad returns `NULL`.
 */

#ifndef ALP_E1M_PINOUT_H
#define ALP_E1M_PINOUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Peripheral instance IDs                                             */
/* ================================================================== */

/* I2C — bus_id passed to alp_i2c_open(). */
#define ALP_E1M_I2C0          0u
#define ALP_E1M_I2C1          1u

/* SPI — bus_id passed to alp_spi_open(). */
#define ALP_E1M_SPI0          0u
#define ALP_E1M_SPI1          1u

/* UART — port_id passed to alp_uart_open(). */
#define ALP_E1M_UART0         0u
#define ALP_E1M_UART1         1u

/* I2S */
#define ALP_E1M_I2S0          0u
#define ALP_E1M_I2S1          1u

/* I3C (single instance on E1M v1) */
#define ALP_E1M_I3C0          0u

/* PDM microphone (digital) */
#define ALP_E1M_PDM0          0u
#define ALP_E1M_PDM1          1u

/* CAN (single instance routed on E1M v1) */
#define ALP_E1M_CAN0          0u

/* Ethernet (single MAC routed on E1M v1; ETH1 lives on E1M-X) */
#define ALP_E1M_ETH0          0u

/* MIPI CSI / DSI (single instance routed on E1M v1) */
#define ALP_E1M_CSI0          0u
#define ALP_E1M_DSI0          0u

/* Parallel-camera, SDIO, USB */
#define ALP_E1M_PARCAM0       0u
#define ALP_E1M_SDIO0         0u
#define ALP_E1M_USB0          0u
#define ALP_E1M_USB2          0u

/* Camera LDOs (per-channel power feedback) */
#define ALP_E1M_CAM0          0u
#define ALP_E1M_CAM1          1u

/* DAC outputs */
#define ALP_E1M_DAC0          0u
#define ALP_E1M_DAC1          1u

/* Single-ended ADC channels (ANA_S0..ANA_S7 ⇒ ADC0..ADC7) */
#define ALP_E1M_ADC0          0u
#define ALP_E1M_ADC1          1u
#define ALP_E1M_ADC2          2u
#define ALP_E1M_ADC3          3u
#define ALP_E1M_ADC4          4u
#define ALP_E1M_ADC5          5u
#define ALP_E1M_ADC6          6u
#define ALP_E1M_ADC7          7u

/* JTAG (single instance) */
#define ALP_E1M_JTAG0         0u

/* PCIe — not routed on E1M (35 × 35); present on E1M-X. */

/* ================================================================== */
/* GPIO indices (`pin_id` passed to alp_gpio_open)                    */
/* ================================================================== */

/* General-purpose IOs (silkscreen IO0..IO25 per E1M v1.0).
 * Indices 0..25 — the canonical numeric order. */
#define ALP_E1M_GPIO_IO0       0u  /**< E1M pad L2  */
#define ALP_E1M_GPIO_IO1       1u  /**< E1M pad L1  */
#define ALP_E1M_GPIO_IO2       2u  /**< E1M pad W2  */
#define ALP_E1M_GPIO_IO3       3u  /**< E1M pad AG2 */
#define ALP_E1M_GPIO_IO4       4u  /**< E1M pad AG16 */
#define ALP_E1M_GPIO_IO5       5u  /**< E1M pad AH18 */
#define ALP_E1M_GPIO_IO6       6u  /**< E1M pad AG18 */
#define ALP_E1M_GPIO_IO7       7u  /**< E1M pad AG34 */
#define ALP_E1M_GPIO_IO8       8u  /**< E1M pad AG33 */
#define ALP_E1M_GPIO_IO9       9u  /**< E1M pad AH34 */
#define ALP_E1M_GPIO_IO10     10u  /**< E1M pad AH33 */
#define ALP_E1M_GPIO_IO11     11u  /**< E1M pad A18  */
#define ALP_E1M_GPIO_IO12     12u  /**< Reserved — not present on v1.0 (kept for ABI stability) */
#define ALP_E1M_GPIO_IO13     13u  /**< E1M pad E3   */
#define ALP_E1M_GPIO_IO14     14u  /**< Reserved — not present on v1.0 */
#define ALP_E1M_GPIO_IO15     15u  /**< E1M pad F3   */
#define ALP_E1M_GPIO_IO16     16u  /**< E1M pad G3   */
#define ALP_E1M_GPIO_IO17     17u  /**< E1M pad H3   */
#define ALP_E1M_GPIO_IO18     18u  /**< E1M pad I3   */
#define ALP_E1M_GPIO_IO19     19u  /**< E1M pad J3   */
#define ALP_E1M_GPIO_IO20     20u  /**< E1M pad K3   */
#define ALP_E1M_GPIO_IO21     21u  /**< E1M pad L3   */
#define ALP_E1M_GPIO_IO22     22u  /**< E1M pad M3   */
#define ALP_E1M_GPIO_IO23     23u  /**< E1M pad N3   */
#define ALP_E1M_GPIO_IO24     24u  /**< E1M pad O3   */
#define ALP_E1M_GPIO_IO25     25u  /**< E1M pad P3   */

/* PWM-capable pads (silkscreen PWM0..PWM7).  Indices 26..33. */
#define ALP_E1M_GPIO_PWM0     26u  /**< E1M pad A6 */
#define ALP_E1M_GPIO_PWM1     27u  /**< E1M pad B6 */
#define ALP_E1M_GPIO_PWM2     28u  /**< E1M pad A5 */
#define ALP_E1M_GPIO_PWM3     29u  /**< E1M pad B5 */
#define ALP_E1M_GPIO_PWM4     30u  /**< E1M pad A4 */
#define ALP_E1M_GPIO_PWM5     31u  /**< E1M pad B4 */
#define ALP_E1M_GPIO_PWM6     32u  /**< E1M pad A3 */
#define ALP_E1M_GPIO_PWM7     33u  /**< E1M pad B3 */

/* Quadrature-encoder pads (silkscreen ENCx_X / ENCx_Y).
 * GPIO secondary; the default function is the hardware encoder.
 * Indices 34..41 in (X,Y) pairs per encoder unit. */
#define ALP_E1M_GPIO_ENC0_X   34u  /**< E1M pad A10 */
#define ALP_E1M_GPIO_ENC0_Y   35u  /**< E1M pad B10 */
#define ALP_E1M_GPIO_ENC1_X   36u  /**< E1M pad A9  */
#define ALP_E1M_GPIO_ENC1_Y   37u  /**< E1M pad B9  */
#define ALP_E1M_GPIO_ENC2_X   38u  /**< E1M pad A8  */
#define ALP_E1M_GPIO_ENC2_Y   39u  /**< E1M pad B8  */
#define ALP_E1M_GPIO_ENC3_X   40u  /**< E1M pad A7  */
#define ALP_E1M_GPIO_ENC3_Y   41u  /**< E1M pad B7  */

/** Total number of GPIO-capable indices in this header.
 *  Carriers' `alp,pin-array` arrays must list this many entries. */
#define ALP_E1M_GPIO_COUNT    42u

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_E1M_PINOUT_H */
