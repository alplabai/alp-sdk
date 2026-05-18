/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Auto-generated from metadata/carriers/E1M-EVK/board.yaml
 * by scripts/gen_carrier_header.py.  DO NOT EDIT BY HAND --
 * regenerate after changing the YAML.
 *
 * Mirrors the carrier preset's `e1m_routes:` block into plain
 * `#define EVK_<NAME> E1M_<...>` lines so hand-written firmware
 * can keep using the carrier-named macros while the YAML stays
 * the single editable source of truth.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.6 generated; macro names + values track the carrier YAML.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_BOARDS_E1M_EVK_ROUTES_H
#define ALP_BOARDS_E1M_EVK_ROUTES_H

#include "alp/e1m_pinout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* GPIO routes (E1M_GPIO_IO<N> -> carrier-side feature) */
/* ------------------------------------------------------------------ */

#define EVK_PIN_CAM_MUX_SEL    E1M_GPIO_IO2  /**< PI3WVR626 SEL pin; see `evk_cam_select_*` enum + chips/cam_mux_pi3wvr626. */
#define EVK_PIN_ENCODER_SW     E1M_GPIO_IO4  /**< Rotary encoder push switch (PEC12R-4222F-S0024), 10k pull-up + 0.1uF debounce. Active-low. */
#define EVK_PIN_CAM_RST        E1M_GPIO_IO5  /**< Camera reset (active-low). Active-low. */
#define EVK_PIN_PCIE_IOEXP_INT E1M_GPIO_IO7  /**< INT input from the PCIe IO expander. */
#define EVK_PIN_I2S_MUX_EN     E1M_GPIO_IO8  /**< I2S0 74LVC157 /E (Alif side P7.1); drive low to enable mux. Active-low. */
#define EVK_PIN_PCIE_IOEXP_RST E1M_GPIO_IO9  /**< Reset output to the PCIe IO expander. */
#define EVK_PIN_PCIE0_I2C_EN   E1M_GPIO_IO10  /**< Drive high to enable I2C mux to the PCIe slot. */
#define EVK_PIN_USB2_MUX_SEL   E1M_GPIO_IO11  /**< USB2 TMUXHS221 select: 0 = USB connector, 1 = M.2 E-key USB. */
#define EVK_PIN_I2S_MUX_SEL    E1M_GPIO_IO13  /**< I2S0 74LVC157 S; 0 = TAS2563 amps, 1 = M.2 E-key I2S. */
#define EVK_PIN_BMI323_INT1    E1M_GPIO_IO15  /**< BMI323 INT1 (data-ready / motion / FIFO); CC3501E GPIO14. */
#define EVK_PIN_W_DISABLE2     E1M_GPIO_IO16  /**< Bluetooth disable (open-drain, active-low) on M.2 E-key. Active-low. */
#define EVK_PIN_W_DISABLE1     E1M_GPIO_IO17  /**< Wi-Fi disable (open-drain, active-low) on M.2 E-key. Active-low. */
#define EVK_PIN_M2E_SDIO_WAKE  E1M_GPIO_IO18  /**< M.2 E-key SDIO-path wake (active-low). Active-low. */
#define EVK_PIN_M2E_UART_WAKE  E1M_GPIO_IO19  /**< M.2 E-key UART-path wake (active-low). Active-low. */
#define EVK_PIN_SDIO_MUX_EN    E1M_GPIO_IO20  /**< SDIO 74LVC157 /E; drive low to enable mux. Active-low. */
#define EVK_PIN_SDIO_MUX_SEL   E1M_GPIO_IO21  /**< SDIO 74LVC157 S; 0 = M.2 E-key SDIO, 1 = microSD slot. */
#define EVK_PIN_PCIE_MUX_PD    E1M_GPIO_IO22  /**< Drive HIGH to power down all four PCIe lane muxes. */
#define EVK_PIN_PCIE_MUX_SEL   E1M_GPIO_IO23  /**< Selects M-key vs E-key routing on the PCIe lane muxes. */

/* ------------------------------------------------------------------ */
/* Bus assignments (E1M peripheral instance -> carrier role) */
/* ------------------------------------------------------------------ */

#define EVK_I2C_BUS_SENSORS   E1M_I2C0  /**< Shared sensor + IO-expander + INA236 bus. */
#define EVK_I2C_BUS_DSI_CSI   E1M_I2C1  /**< Display + camera control I2C (touch panel, camera-side I2C config). */
#define EVK_I2C_BUS_ARDUINO   E1M_I3C0  /**< Arduino UNO header I2C ride on I3C0 (I3C is backwards-compatible with classic I2C). */
#define EVK_SPI_BUS_ARDUINO   E1M_SPI1  /**< Arduino UNO header SPI; terminates on the on-module CC3501E, not the main SoC. */
#define EVK_UART_PORT_DEBUG   E1M_UART0  /**< Console UART exposed on the JTAG/SWD-side debug header. */
#define EVK_UART_PORT_ARDUINO E1M_UART1  /**< Arduino UNO header UART (D0/D1); CK_RXD = UART1_TX, CK_TXD = UART1_RX. */

/* ------------------------------------------------------------------ */
/* PWM channels (E1M_PWM<N> -> carrier-side feature) */
/* ------------------------------------------------------------------ */

#define EVK_PWM_LED_GREEN E1M_PWM0  /**< RGB LED green; schematic-wired via PWM0 (non-contiguous with R/B). */
#define EVK_PWM_LED_BLUE  E1M_PWM1  /**< RGB LED blue channel. */
#define EVK_ARD_PWM1      E1M_PWM1  /**< Arduino header CK_PWM1; shares E1M_PWM1 with LED_BLUE. */
#define EVK_ARD_PWM4      E1M_PWM2  /**< Arduino header CK_PWM4 = E1M_PWM2. */
#define EVK_PWM_LED_RED   E1M_PWM3  /**< RGB LED red channel. */
#define EVK_ARD_PWM2      E1M_PWM4  /**< Arduino header CK_PWM2 = E1M_PWM4. */
#define EVK_ARD_PWM3      E1M_PWM5  /**< Arduino header CK_PWM3 = E1M_PWM5. */
#define EVK_MB_PWM        E1M_PWM6  /**< mikroBUS PWM pin. */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_BOARDS_E1M_EVK_ROUTES_H */
