/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file e1m_x_pinout.h
 * @brief E1M-X (45 x 65 mm) form-factor instance IDs.
 *
 * Parallel namespace to <alp/e1m_pinout.h>. SoMs in the E1M-X form
 * factor (V2N, V2M families) expose this set; apps include this
 * header instead of e1m_pinout.h.
 *
 * The two namespaces are intentionally NOT compatible by include —
 * picking the right header per form factor is the customer's
 * compile-time signal that their app targets a specific form factor.
 *
 * Derived from `alplabai/e1m-spec/pinout/x-v1.json` (E1M-X v1.0,
 * 45 x 65 mm).  Pinned to `e1m-spec` x-v1.0.
 *
 * ## Per-board feature names
 *
 * Names like `ENCODER_SW` or `USER_LED` are board-specific and
 * live in `<alp/boards/<board>.h>`.  Those boards reference the
 * macros below for their underlying pin/instance integers.
 *
 * ## Portable subset
 *
 * The peripheral instance IDs (E1M_X_PWM0..E1M_X_PWM7, etc.) carry
 * the same integer values as their E1M counterparts — the
 * form-factor distinction is encoded in the symbol name, not the
 * integer.  An app that includes this header in place of
 * e1m_pinout.h and uses only `E1M_X_<CLASS><N>` for
 * `N < E1M_X_<CLASS>_COUNT` stays cross-SoM portable within the
 * E1M-X family (V2N, V2M).
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.6 + 2026-05-18 initial namespace; pinned by e1m-spec x-v1.0.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_E1M_X_PINOUT_H
#define ALP_E1M_X_PINOUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Peripheral instance IDs                                             */
/* ================================================================== */

/* I2C — bus_id passed to alp_i2c_open(). */
#define E1M_X_I2C0          0u
#define E1M_X_I2C1          1u

/* SPI — bus_id passed to alp_spi_open(). */
#define E1M_X_SPI0          0u
#define E1M_X_SPI1          1u

/* UART — port_id passed to alp_uart_open(). */
#define E1M_X_UART0         0u
#define E1M_X_UART1         1u

/* I2S */
#define E1M_X_I2S0          0u
#define E1M_X_I2S1          1u

/* I3C (single instance on E1M-X v1) */
#define E1M_X_I3C0          0u

/* PDM microphone (digital) */
#define E1M_X_PDM0          0u
#define E1M_X_PDM1          1u

/* CAN (single portable instance; E1M-X SoCs may expose more) */
#define E1M_X_CAN0          0u

/* Ethernet (two MAC instances on E1M-X) */
#define E1M_X_ETH0          0u
#define E1M_X_ETH1          1u

/* MIPI CSI / DSI */
#define E1M_X_CSI0          0u
#define E1M_X_DSI0          0u

/* Parallel-camera, SDIO, USB */
#define E1M_X_PARCAM0       0u
#define E1M_X_SDIO0         0u
#define E1M_X_USB0          0u
#define E1M_X_USB2          0u

/* Camera LDOs (per-channel power feedback) */
#define E1M_X_CAM0          0u
#define E1M_X_CAM1          1u

/* PCIe — present on E1M-X (not routed on base E1M 35 x 35). */
#define E1M_X_PCIE0         0u

/* DAC outputs */
#define E1M_X_DAC0          0u
#define E1M_X_DAC1          1u

/* Single-ended ADC channels (ANA_S0..ANA_S7 => ADC0..ADC7) */
#define E1M_X_ADC0          0u
#define E1M_X_ADC1          1u
#define E1M_X_ADC2          2u
#define E1M_X_ADC3          3u
#define E1M_X_ADC4          4u
#define E1M_X_ADC5          5u
#define E1M_X_ADC6          6u
#define E1M_X_ADC7          7u

/* PWM channel IDs — passed as `channel_id` to alp_pwm_open(). */
#define E1M_X_PWM0          0u
#define E1M_X_PWM1          1u
#define E1M_X_PWM2          2u
#define E1M_X_PWM3          3u
#define E1M_X_PWM4          4u
#define E1M_X_PWM5          5u
#define E1M_X_PWM6          6u
#define E1M_X_PWM7          7u

/* Quadrature-encoder IDs — passed as `encoder_id` to alp_qenc_open(). */
#define E1M_X_ENC0          0u
#define E1M_X_ENC1          1u
#define E1M_X_ENC2          2u
#define E1M_X_ENC3          3u

/* Counter / generic-timer instance IDs. */
#define E1M_X_COUNTER0      0u
#define E1M_X_COUNTER1      1u
#define E1M_X_COUNTER2      2u
#define E1M_X_COUNTER3      3u

/* Watchdog instance IDs. */
#define E1M_X_WDT0          0u
#define E1M_X_WDT1          1u

/* RTC instance IDs. */
#define E1M_X_RTC0          0u

/* JTAG (single instance) */
#define E1M_X_JTAG0         0u

/* ================================================================== */
/* E1M-X-spec instance counts (the portability bound)                  */
/* ================================================================== */
/*
 * An app that uses only `E1M_X_<CLASS><N>` for
 * `N < E1M_X_<CLASS>_COUNT` stays cross-SoM portable across all
 * E1M-X-conformant modules.
 */
#define E1M_X_I2C_COUNT        2u
#define E1M_X_SPI_COUNT        2u
#define E1M_X_UART_COUNT       2u
#define E1M_X_I2S_COUNT        2u
#define E1M_X_I3C_COUNT        1u
#define E1M_X_PDM_COUNT        2u
#define E1M_X_CAN_COUNT        1u
#define E1M_X_ETH_COUNT        2u   /**< E1M-X adds ETH1 vs base E1M. */
#define E1M_X_CSI_COUNT        1u
#define E1M_X_DSI_COUNT        1u
#define E1M_X_PARCAM_COUNT     1u
#define E1M_X_SDIO_COUNT       1u
#define E1M_X_USB_COUNT        1u
#define E1M_X_USB2_COUNT       1u
#define E1M_X_PCIE_COUNT       1u   /**< E1M-X adds PCIe vs base E1M. */
#define E1M_X_DAC_COUNT        2u
#define E1M_X_ADC_COUNT        8u
#define E1M_X_PWM_COUNT        8u
#define E1M_X_ENC_COUNT        4u
#define E1M_X_GPIO_IO_COUNT    36u  /**< IO0..IO35; IO33 reserved/not-present per x-v1.0. */

/* ================================================================== */
/* GPIO indices (`pin_id` passed to alp_gpio_open)                    */
/* ================================================================== */

/* General-purpose IOs — portable range IO0..IO25 shared with E1M. */
#define E1M_X_GPIO_IO0       0u  /**< E1M-X pad (see e1m-spec/pinout/x-v1.json) */
#define E1M_X_GPIO_IO1       1u
#define E1M_X_GPIO_IO2       2u
#define E1M_X_GPIO_IO3       3u
#define E1M_X_GPIO_IO4       4u
#define E1M_X_GPIO_IO5       5u
#define E1M_X_GPIO_IO6       6u
#define E1M_X_GPIO_IO7       7u
#define E1M_X_GPIO_IO8       8u
#define E1M_X_GPIO_IO9       9u
#define E1M_X_GPIO_IO10     10u
#define E1M_X_GPIO_IO11     11u
#define E1M_X_GPIO_IO12     12u  /**< Reserved — not present on x-v1.0 (kept for ABI stability) */
#define E1M_X_GPIO_IO13     13u
#define E1M_X_GPIO_IO14     14u  /**< Reserved — not present on x-v1.0 */
#define E1M_X_GPIO_IO15     15u
#define E1M_X_GPIO_IO16     16u
#define E1M_X_GPIO_IO17     17u
#define E1M_X_GPIO_IO18     18u
#define E1M_X_GPIO_IO19     19u
#define E1M_X_GPIO_IO20     20u
#define E1M_X_GPIO_IO21     21u
#define E1M_X_GPIO_IO22     22u
#define E1M_X_GPIO_IO23     23u
#define E1M_X_GPIO_IO24     24u
#define E1M_X_GPIO_IO25     25u

/* E1M-X form-factor GPIO extensions (IO26..IO35).
 *
 * These pads are specific to the E1M-X form factor and do not exist
 * on the base E1M (35 x 35 mm).  On V2N/V2M SoMs they are routed
 * through the GD32G553 IO MCU bridge; the dispatch is transparent to
 * application code using the portable <alp/gpio.h> surface.
 *
 * IO33 has no V2N route and is reserved. */
#define E1M_X_GPIO_IO26     26u
#define E1M_X_GPIO_IO27     27u  /**< V2N pad PB11 via GD32 IO MCU */
#define E1M_X_GPIO_IO28     28u  /**< V2N pad PC2  via GD32 IO MCU */
#define E1M_X_GPIO_IO29     29u  /**< V2N pad PD11 via GD32 IO MCU */
#define E1M_X_GPIO_IO30     30u  /**< V2N pad PD10 via GD32 IO MCU */
#define E1M_X_GPIO_IO31     31u  /**< V2N pad PE12 via GD32 IO MCU */
#define E1M_X_GPIO_IO32     32u  /**< V2N pad PD2  via GD32 IO MCU */
#define E1M_X_GPIO_IO33     33u  /**< Reserved — not present on V2N (kept for ABI stability) */
#define E1M_X_GPIO_IO34     34u  /**< V2N pad PD8  via GD32 IO MCU */
#define E1M_X_GPIO_IO35     35u  /**< V2N pad PD1  via GD32 IO MCU */

/* PWM-capable pads (silkscreen PWM0..PWM7).  Indices 36..43. */
#define E1M_X_GPIO_PWM0     36u
#define E1M_X_GPIO_PWM1     37u
#define E1M_X_GPIO_PWM2     38u
#define E1M_X_GPIO_PWM3     39u
#define E1M_X_GPIO_PWM4     40u
#define E1M_X_GPIO_PWM5     41u
#define E1M_X_GPIO_PWM6     42u
#define E1M_X_GPIO_PWM7     43u

/* Quadrature-encoder pads (silkscreen ENCx_X / ENCx_Y).
 * Indices 44..51 in (X,Y) pairs per encoder unit. */
#define E1M_X_GPIO_ENC0_X   44u
#define E1M_X_GPIO_ENC0_Y   45u
#define E1M_X_GPIO_ENC1_X   46u
#define E1M_X_GPIO_ENC1_Y   47u
#define E1M_X_GPIO_ENC2_X   48u
#define E1M_X_GPIO_ENC2_Y   49u
#define E1M_X_GPIO_ENC3_X   50u
#define E1M_X_GPIO_ENC3_Y   51u

/* Single-ended ADC pads as GPIO indices.  Indices 52..59. */
#define E1M_X_GPIO_ADC0     52u
#define E1M_X_GPIO_ADC1     53u
#define E1M_X_GPIO_ADC2     54u
#define E1M_X_GPIO_ADC3     55u
#define E1M_X_GPIO_ADC4     56u
#define E1M_X_GPIO_ADC5     57u
#define E1M_X_GPIO_ADC6     58u
#define E1M_X_GPIO_ADC7     59u

/* DAC output pads as GPIO indices.  Indices 60..61. */
#define E1M_X_GPIO_DAC0     60u
#define E1M_X_GPIO_DAC1     61u

/** Total number of GPIO-capable indices in this header.
 *  E1M-X boards' `alp,pin-array` arrays must list this many entries. */
#define E1M_X_GPIO_COUNT    62u

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_E1M_X_PINOUT_H */
