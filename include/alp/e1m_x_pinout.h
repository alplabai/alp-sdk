/*
 * Copyright 2026 Alp Lab AB
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
 *      2026-05-24 additive sync to the full x-v1.0 connector: I2C2/3,
 *      SPI2, CAN1, CSI2/3, DSI1, USB1, PCIE1, plus the new LCD0
 *      parallel-RGB class.  Purely additive — every pre-existing
 *      instance ID and GPIO index keeps its value, so the ABI stays
 *      stable.  See docs/abi-markers.md for the convention.
 */

#ifndef ALP_E1M_X_PINOUT_H
#define ALP_E1M_X_PINOUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/* The pad / instance #define tables below are column-aligned for
 * readability.  clang-format-14 carries no AlignConsecutiveMacros in
 * .clang-format, so it would collapse the alignment to a single space;
 * the off/on guard keeps the table aligned -- same rationale as the
 * generated alp_*_routes.h board headers. */

/* ================================================================== */
/* Peripheral instance IDs                                             */
/* ================================================================== */

/* I2C — bus_id passed to alp_i2c_open().
 * All four instances are defined by e1m-spec x-v1.0 (module connector
 * ASS6880 LGA496): I2C0 AH1/AH2, I2C1 AQ47/AR47, I2C2 E63/E64,
 * I2C3 A23/A24. */
#define E1M_X_I2C0          0u
#define E1M_X_I2C1          1u
#define E1M_X_I2C2          2u
#define E1M_X_I2C3          3u

/* SPI — bus_id passed to alp_spi_open(). */
#define E1M_X_SPI0          0u
#define E1M_X_SPI1          1u
#define E1M_X_SPI2          2u

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

/* CAN (E1M-X exposes two transceivers) */
#define E1M_X_CAN0          0u
#define E1M_X_CAN1          1u

/* Ethernet (two MAC instances on E1M-X) */
#define E1M_X_ETH0          0u
#define E1M_X_ETH1          1u

/* MIPI CSI / DSI — the E1M-X connector carries 4x CSI + 2x DSI. */
#define E1M_X_CSI0          0u
#define E1M_X_CSI1          1u
#define E1M_X_CSI2          2u
#define E1M_X_CSI3          3u
#define E1M_X_DSI0          0u
#define E1M_X_DSI1          1u

/* Parallel RGB LCD (single controller on E1M-X v1).
 * Pads LCD_B0..LCD_B23 (24-bit) + LCD_HSYNC + LCD_VSYNC; the x-v1.0
 * connector exposes no separate PCLK/DE pad. */
#define E1M_X_LCD0          0u

/* Parallel-camera, SDIO, USB.  USB0/USB1 = the two USB3.x ports;
 * USB2 = the USB2.0-only port (separate class). */
#define E1M_X_PARCAM0       0u
#define E1M_X_SDIO0         0u
#define E1M_X_USB0          0u
#define E1M_X_USB1          1u
#define E1M_X_USB2          0u

/* Camera LDOs (per-channel power feedback) */
#define E1M_X_CAM0          0u
#define E1M_X_CAM1          1u

/* PCIe — present on E1M-X (not routed on base E1M 35 x 35).
 * The connector carries two x4 PCIe lane groups. */
#define E1M_X_PCIE0         0u
#define E1M_X_PCIE1         1u

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
#define E1M_X_I2C_COUNT        4u
#define E1M_X_SPI_COUNT        3u
#define E1M_X_UART_COUNT       2u
#define E1M_X_I2S_COUNT        2u
#define E1M_X_I3C_COUNT        1u
#define E1M_X_PDM_COUNT        2u
#define E1M_X_CAN_COUNT        2u
#define E1M_X_ETH_COUNT        2u   /**< E1M-X adds ETH1 vs base E1M. */
#define E1M_X_CSI_COUNT        4u
#define E1M_X_DSI_COUNT        2u
#define E1M_X_LCD_COUNT        1u   /**< Parallel RGB LCD (24-bit + H/VSYNC). */
#define E1M_X_PARCAM_COUNT     1u
#define E1M_X_SDIO_COUNT       1u
#define E1M_X_USB_COUNT        2u
#define E1M_X_USB2_COUNT       1u
#define E1M_X_PCIE_COUNT       2u   /**< E1M-X adds PCIe vs base E1M. */
#define E1M_X_DAC_COUNT        2u
#define E1M_X_ADC_COUNT        8u
#define E1M_X_PWM_COUNT        8u
#define E1M_X_ENC_COUNT        4u
#define E1M_X_GPIO_IO_COUNT    36u  /**< IO0..IO35; IO7 & IO23 have no pad on x-v1.0 (indices kept for ABI). */

/* ================================================================== */
/* GPIO indices (`pin_id` passed to alp_gpio_open)                    */
/* ================================================================== */

/* General-purpose IOs — portable range IO0..IO25 shared with E1M.
 * Trailing comment = the E1M-X connector pad (x-v1.json); the logical
 * index does not reveal the physical pad, so it is recorded here. */
#define E1M_X_GPIO_IO0       0u  /**< pad L2 */
#define E1M_X_GPIO_IO1       1u  /**< pad M2 */
#define E1M_X_GPIO_IO2       2u  /**< pad N2 */
#define E1M_X_GPIO_IO3       3u  /**< pad O2 */
#define E1M_X_GPIO_IO4       4u  /**< pad AF2 */
#define E1M_X_GPIO_IO5       5u  /**< pad AG2 */
#define E1M_X_GPIO_IO6       6u  /**< pad AL2 */
#define E1M_X_GPIO_IO7       7u  /**< no pad on x-v1.0 — index reserved for ABI stability */
#define E1M_X_GPIO_IO8       8u  /**< pad AR21 */
#define E1M_X_GPIO_IO9       9u  /**< pad AQ21 */
#define E1M_X_GPIO_IO10     10u  /**< pad AR22 */
#define E1M_X_GPIO_IO11     11u  /**< pad AQ22 */
#define E1M_X_GPIO_IO12     12u  /**< pad AR23 */
#define E1M_X_GPIO_IO13     13u  /**< pad AQ23 */
#define E1M_X_GPIO_IO14     14u  /**< pad AR24 */
#define E1M_X_GPIO_IO15     15u  /**< pad AQ24 */
#define E1M_X_GPIO_IO16     16u  /**< pad AR25 */
#define E1M_X_GPIO_IO17     17u  /**< pad AQ26 */
#define E1M_X_GPIO_IO18     18u  /**< pad AR26 */
#define E1M_X_GPIO_IO19     19u  /**< pad AQ27 */
#define E1M_X_GPIO_IO20     20u  /**< pad AR27 */
#define E1M_X_GPIO_IO21     21u  /**< pad AQ28 */
#define E1M_X_GPIO_IO22     22u  /**< pad AR28 */
#define E1M_X_GPIO_IO23     23u  /**< no pad on x-v1.0 — index reserved for ABI stability */
#define E1M_X_GPIO_IO24     24u  /**< pad AR29 */
#define E1M_X_GPIO_IO25     25u  /**< pad F63 */

/* E1M-X form-factor GPIO extensions (IO26..IO35).
 *
 * These pads are specific to the E1M-X form factor and do not exist
 * on the base E1M (35 x 35 mm).  On V2N/V2M SoMs they are routed
 * through the GD32G553 IO MCU bridge; the dispatch is transparent to
 * application code using the portable <alp/gpio.h> surface.
 *
 * Trailing comment = E1M-X connector pad (x-v1.json) -> V2N routing.
 * IO33's pad exists on x-v1.0 but has no V2N route. */
#define E1M_X_GPIO_IO26     26u  /**< pad F64 */
#define E1M_X_GPIO_IO27     27u  /**< pad AN63 -> V2N PB11 via GD32 IO MCU */
#define E1M_X_GPIO_IO28     28u  /**< pad AN64 -> V2N PC2  via GD32 IO MCU */
#define E1M_X_GPIO_IO29     29u  /**< pad AO63 -> V2N PD11 via GD32 IO MCU */
#define E1M_X_GPIO_IO30     30u  /**< pad AO64 -> V2N PD10 via GD32 IO MCU */
#define E1M_X_GPIO_IO31     31u  /**< pad AP63 -> V2N PE12 via GD32 IO MCU */
#define E1M_X_GPIO_IO32     32u  /**< pad AP64 -> V2N PD2  via GD32 IO MCU */
#define E1M_X_GPIO_IO33     33u  /**< pad AQ63 -> no V2N route (index kept for ABI) */
#define E1M_X_GPIO_IO34     34u  /**< pad AQ64 -> V2N PD8  via GD32 IO MCU */
#define E1M_X_GPIO_IO35     35u  /**< pad A25  -> V2N PD1  via GD32 IO MCU */

/* PWM-capable pads (silkscreen PWM0..PWM7).  Indices 36..43. */
#define E1M_X_GPIO_PWM0     36u  /**< pad A7 */
#define E1M_X_GPIO_PWM1     37u  /**< pad B7 */
#define E1M_X_GPIO_PWM2     38u  /**< pad A6 */
#define E1M_X_GPIO_PWM3     39u  /**< pad B6 */
#define E1M_X_GPIO_PWM4     40u  /**< pad A5 */
#define E1M_X_GPIO_PWM5     41u  /**< pad B5 */
#define E1M_X_GPIO_PWM6     42u  /**< pad A4 */
#define E1M_X_GPIO_PWM7     43u  /**< pad B4 */

/* Quadrature-encoder pads (silkscreen ENCx_X / ENCx_Y).
 * Indices 44..51 in (X,Y) pairs per encoder unit. */
#define E1M_X_GPIO_ENC0_X   44u  /**< pad A12 */
#define E1M_X_GPIO_ENC0_Y   45u  /**< pad B12 */
#define E1M_X_GPIO_ENC1_X   46u  /**< pad A11 */
#define E1M_X_GPIO_ENC1_Y   47u  /**< pad B11 */
#define E1M_X_GPIO_ENC2_X   48u  /**< pad A10 */
#define E1M_X_GPIO_ENC2_Y   49u  /**< pad B10 */
#define E1M_X_GPIO_ENC3_X   50u  /**< pad A9 */
#define E1M_X_GPIO_ENC3_Y   51u  /**< pad B9 */

/* Single-ended ADC pads as GPIO indices.  Indices 52..59. */
#define E1M_X_GPIO_ADC0     52u  /**< pad A17 (ANA_S0) */
#define E1M_X_GPIO_ADC1     53u  /**< pad B17 (ANA_S1) */
#define E1M_X_GPIO_ADC2     54u  /**< pad A16 (ANA_S2) */
#define E1M_X_GPIO_ADC3     55u  /**< pad B16 (ANA_S3) */
#define E1M_X_GPIO_ADC4     56u  /**< pad A15 (ANA_S4) */
#define E1M_X_GPIO_ADC5     57u  /**< pad B15 (ANA_S5) */
#define E1M_X_GPIO_ADC6     58u  /**< pad A14 (ANA_S6) */
#define E1M_X_GPIO_ADC7     59u  /**< pad B14 (ANA_S7) */

/* DAC output pads as GPIO indices.  Indices 60..61. */
#define E1M_X_GPIO_DAC0     60u  /**< pad A19 */
#define E1M_X_GPIO_DAC1     61u  /**< pad B19 */

/* ------------------------------------------------------------------ */
/* E1M-X connector single-ended digital pads (2026-05-24).            */
/*                                                                    */
/* Appended at index >= 62 so every index above keeps its value — the */
/* extension is ABI-safe.  Differential pairs (CSI/DSI/PCIe/USB/ETH)  */
/* are not GPIO-capable and are intentionally omitted.                */
/* ------------------------------------------------------------------ */

/* I2C2 / I2C3 pads (silkscreen I2Cx_SDA / I2Cx_SCL).  Indices 62..65. */
#define E1M_X_GPIO_I2C2_SDA  62u  /**< pad E63 */
#define E1M_X_GPIO_I2C2_SCL  63u  /**< pad E64 */
#define E1M_X_GPIO_I2C3_SDA  64u  /**< pad A23 */
#define E1M_X_GPIO_I2C3_SCL  65u  /**< pad A24 */

/* SPI2 pads (silkscreen SPI2_*).  Indices 66..70. */
#define E1M_X_GPIO_SPI2_MISO 66u  /**< pad B63 */
#define E1M_X_GPIO_SPI2_MOSI 67u  /**< pad B64 */
#define E1M_X_GPIO_SPI2_SCLK 68u  /**< pad C63 */
#define E1M_X_GPIO_SPI2_CS0  69u  /**< pad C64 */
#define E1M_X_GPIO_SPI2_CS1  70u  /**< pad D64 */

/* CAN1 pads (silkscreen CAN1H/CAN1_TX, CAN1L/CAN1_RX).  Indices 71..72. */
#define E1M_X_GPIO_CAN1_H    71u  /**< pad B24 */
#define E1M_X_GPIO_CAN1_L    72u  /**< pad B25 */

/* Parallel RGB LCD pads (silkscreen LCD_B0..LCD_B23, LCD_HSYNC,
 * LCD_VSYNC).  Indices 73..98. */
#define E1M_X_GPIO_LCD_B0    73u  /**< pad D3 */
#define E1M_X_GPIO_LCD_B1    74u  /**< pad E3 */
#define E1M_X_GPIO_LCD_B2    75u  /**< pad F3 */
#define E1M_X_GPIO_LCD_B3    76u  /**< pad G3 */
#define E1M_X_GPIO_LCD_B4    77u  /**< pad H3 */
#define E1M_X_GPIO_LCD_B5    78u  /**< pad I3 */
#define E1M_X_GPIO_LCD_B6    79u  /**< pad J3 */
#define E1M_X_GPIO_LCD_B7    80u  /**< pad K3 */
#define E1M_X_GPIO_LCD_B8    81u  /**< pad L3 */
#define E1M_X_GPIO_LCD_B9    82u  /**< pad M3 */
#define E1M_X_GPIO_LCD_B10   83u  /**< pad N3 */
#define E1M_X_GPIO_LCD_B11   84u  /**< pad O3 */
#define E1M_X_GPIO_LCD_B12   85u  /**< pad P3 */
#define E1M_X_GPIO_LCD_B13   86u  /**< pad Q3 */
#define E1M_X_GPIO_LCD_B14   87u  /**< pad R3 */
#define E1M_X_GPIO_LCD_B15   88u  /**< pad S3 */
#define E1M_X_GPIO_LCD_B16   89u  /**< pad T3 */
#define E1M_X_GPIO_LCD_B17   90u  /**< pad U3 */
#define E1M_X_GPIO_LCD_B18   91u  /**< pad V3 */
#define E1M_X_GPIO_LCD_B19   92u  /**< pad W3 */
#define E1M_X_GPIO_LCD_B20   93u  /**< pad X3 */
#define E1M_X_GPIO_LCD_B21   94u  /**< pad Y3 */
#define E1M_X_GPIO_LCD_B22   95u  /**< pad Z3 */
#define E1M_X_GPIO_LCD_B23   96u  /**< pad AA3 */
#define E1M_X_GPIO_LCD_HSYNC 97u  /**< pad AB3 */
#define E1M_X_GPIO_LCD_VSYNC 98u  /**< pad AC3 */

/** Total number of GPIO-capable indices in this header.
 *  E1M-X boards' `alp,pin-array` arrays must list this many entries. */
#define E1M_X_GPIO_COUNT    99u

/* clang-format on */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_E1M_X_PINOUT_H */
