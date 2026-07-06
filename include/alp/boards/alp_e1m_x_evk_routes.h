/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Auto-generated from metadata/boards/e1m-x-evk.yaml
 * by scripts/gen_board_header.py.  DO NOT EDIT BY HAND --
 * regenerate after changing the YAML.
 *
 * Mirrors the board YAML's `e1m_routes:` block into plain
 * `#define EVK_<NAME> ALP_E1M_<...>` lines so hand-written firmware
 * can keep using the board-named macros while the YAML stays
 * the single editable source of truth.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.6 generated; macro names + values track the board YAML.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_BOARDS_E1M_X_EVK_ROUTES_H
#define ALP_BOARDS_E1M_X_EVK_ROUTES_H

#include "alp/e1m_x_pinout.h"

/* This header is auto-generated; clang-format ignores it so the
 * generator's column-aligned `#define` blocks survive PR static
 * analysis without forcing 100-col wraps on long doc strings. */
/* clang-format off */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* GPIO routes (ALP_E1M_GPIO_IO<N> -> board-side feature) */
/* ------------------------------------------------------------------ */

#define XEVK_PIN_PCIE_MUX_PD   ALP_E1M_X_GPIO_IO0  /**< Drive HIGH to power down the PCIe lane mux. */
#define XEVK_PIN_PCIE_MUX_SEL  ALP_E1M_X_GPIO_IO1  /**< PCIe lane-mux routing select. */
#define XEVK_PIN_PCIE0_I2C_EN  ALP_E1M_X_GPIO_IO2  /**< Drive high to enable the I2C mux to the PCIe slot. */
#define XEVK_PIN_I2S_MUX_EN    ALP_E1M_X_GPIO_IO4  /**< I2S path-mux (TMUX1574 U46) enable -- active-low; drive LOW to enable. Active-low. */
#define XEVK_PIN_I2S_MUX_SEL   ALP_E1M_X_GPIO_IO5  /**< I2S path-mux (TMUX1574 U46) select: LOW = TAS2563 amps (A side), HIGH = M.2 E-key I2S (B side). */
#define XEVK_PIN_M2E_UART_WAKE ALP_E1M_X_GPIO_IO6  /**< M.2 E-key UART-path wake (active-low). Active-low. */
#define XEVK_PIN_CTP1_INT      ALP_E1M_X_GPIO_IO9  /**< Capacitive touch panel 1 interrupt (display 1). */
#define XEVK_PIN_CTP1_RST      ALP_E1M_X_GPIO_IO11  /**< Capacitive touch panel 1 reset (active-low). Active-low. */
#define XEVK_PIN_CTP2_INT      ALP_E1M_X_GPIO_IO17  /**< Capacitive touch panel 2 interrupt (display 2). */
#define XEVK_PIN_CTP2_RST      ALP_E1M_X_GPIO_IO19  /**< Capacitive touch panel 2 reset (active-low). Active-low. */
#define XEVK_PIN_LCD1_RST      ALP_E1M_X_GPIO_IO13  /**< DSI display panel 1 reset (active-low). Active-low. */
#define XEVK_PIN_LCD1_PWR_EN   ALP_E1M_X_GPIO_IO15  /**< DSI display panel 1 power enable. */
#define XEVK_PIN_LCD2_RST      ALP_E1M_X_GPIO_IO21  /**< DSI display panel 2 reset (active-low). Active-low. */
#define XEVK_PIN_LCD2_PWR_EN   ALP_E1M_X_GPIO_IO22  /**< DSI display panel 2 power enable. */
#define XEVK_PIN_CAM0_MUX_SEL  ALP_E1M_X_GPIO_IO16  /**< Camera-0 source/lane mux select. */
#define XEVK_PIN_CAM0_EN       ALP_E1M_X_GPIO_IO18  /**< Camera-0 power enable. */
#define XEVK_PIN_CAM0_RST      ALP_E1M_X_GPIO_IO20  /**< Camera-0 reset (active-low). Active-low. */
#define XEVK_PIN_USB_MUX_SEL   ALP_E1M_X_GPIO_IO24  /**< USB path-mux select. */
#define XEVK_PIN_SDIO_MUX_SEL  ALP_E1M_X_GPIO_IO27  /**< SDIO path-mux select. */
#define XEVK_PIN_SDIO_MUX_EN   ALP_E1M_X_GPIO_IO29  /**< SDIO path-mux enable. */
#define XEVK_PIN_ENCODER_SW    ALP_E1M_X_GPIO_IO28  /**< Rotary encoder (PEC12R-4222F) push switch; pull-up + RC debounce. Active-low. */
#define XEVK_PIN_BMI323_INT1   ALP_E1M_X_GPIO_IO32  /**< BMI323 INT1 (data-ready / motion / FIFO). */
#define XEVK_PIN_LED_RED       ALP_E1M_X_GPIO_PWM5  /**< RGB LED red -- the PWM5 pad driven as a digital GPIO. */
#define XEVK_PIN_LED_GREEN     ALP_E1M_X_GPIO_PWM7  /**< RGB LED green -- the PWM7 pad driven as a digital GPIO. */
#define XEVK_PIN_LED_BLUE      ALP_E1M_X_GPIO_PWM6  /**< RGB LED blue -- the PWM6 pad driven as a digital GPIO. */

/* ------------------------------------------------------------------ */
/* Bus assignments (ALP_E1M_I2C / I3C / SPI / UART -> board role) */
/* ------------------------------------------------------------------ */

#define XEVK_I2C_BUS_SENSORS   ALP_E1M_X_I2C0  /**< On-board sensor + IO-expander + INA236 bus (ICM-42670, BMI323, BMP581, TCAL9538, INA236). */
#define XEVK_I2C_BUS_DSI_CSI0  ALP_E1M_X_I2C2  /**< Display/camera control I2C bank 0 (DSI panel + CSI camera-side config). */
#define XEVK_I2C_BUS_DSI_CSI1  ALP_E1M_X_I2C3  /**< Display/camera control I2C bank 1. */
#define XEVK_SPI_BUS_ARDUINO   ALP_E1M_X_SPI1  /**< Arduino UNO header SPI (level-shifted). */
#define XEVK_UART_PORT_DEBUG   ALP_E1M_X_UART0  /**< Console / debug UART. */
#define XEVK_UART_PORT_ARDUINO ALP_E1M_X_UART1  /**< Arduino UNO header UART (D0/D1, level-shifted). */

/* ------------------------------------------------------------------ */
/* PWM channels (ALP_E1M_PWM<N> -> board-side feature) */
/* ------------------------------------------------------------------ */

#define XEVK_ARD_PWM0      ALP_E1M_X_PWM0  /**< Arduino UNO header PWM0 (level-shifted). */
#define XEVK_ARD_PWM1      ALP_E1M_X_PWM1  /**< Arduino UNO header PWM1. */
#define XEVK_ARD_PWM2      ALP_E1M_X_PWM2  /**< Arduino UNO header PWM2. */
#define XEVK_ARD_PWM3      ALP_E1M_X_PWM3  /**< Arduino UNO header PWM3. */
#define XEVK_PWM_DISP2_BL  ALP_E1M_X_PWM4  /**< Display-2 backlight PWM (DISP2_BL_PWM). */
#define XEVK_PWM_LED_RED   ALP_E1M_X_PWM5  /**< RGB LED red channel (GPIO-secondary = XEVK_PIN_LED_RED). */
#define XEVK_PWM_LED_BLUE  ALP_E1M_X_PWM6  /**< RGB LED blue channel (GPIO-secondary = XEVK_PIN_LED_BLUE). */
#define XEVK_PWM_LED_GREEN ALP_E1M_X_PWM7  /**< RGB LED green channel (GPIO-secondary = XEVK_PIN_LED_GREEN). */

/* ------------------------------------------------------------------ */
/* ADC channels (ALP_E1M_ADC<N> -> board-side signal) */
/* ------------------------------------------------------------------ */

#define XEVK_ADC_MIKROBUS_AN ALP_E1M_X_ADC0  /**< mikroBUS Click socket AN analog input (net CK_ANA, also on breakout header P7.1; raw passthrough, no divider).  The V2 carrier netlist routes ONLY the mikroBUS AN to ANA_S0; the earlier 'Arduino A0' doc here did not match the netlist. */

/* ------------------------------------------------------------------ */
/* DAC channels (ALP_E1M_DAC<N> -> board-side signal) */
/* ------------------------------------------------------------------ */

#define XEVK_DAC0 ALP_E1M_X_DAC0  /**< DAC0 analog output.  Header J15.2 (DAC0_OUT) is the x2-buffered copy, but the buffered path is INOPERABLE on this carrier revision (carrier erratum, fixed next rev; rework details in the internal carrier errata).  Bench use taps the raw 1.8 V-full-scale DAC0 net instead. */
#define XEVK_DAC1 ALP_E1M_X_DAC1  /**< DAC1 analog output. */

/* ------------------------------------------------------------------ */
/* I2S instances (ALP_E1M_I2S<N> -> board-side codec / mic role) */
/* ------------------------------------------------------------------ */

#define XEVK_I2S_AUDIO ALP_E1M_X_I2S0  /**< TAS2563 smart-amp I2S (SCLK / WS / SDI / SDO). */

/* ------------------------------------------------------------------ */
/* CAN buses (ALP_E1M_CAN<N> -> board-side bus role) */
/* ------------------------------------------------------------------ */

#define XEVK_CAN_BUS0 ALP_E1M_X_CAN0  /**< CAN0 via TCAN1044 transceiver (U51). */
#define XEVK_CAN_BUS1 ALP_E1M_X_CAN1  /**< CAN1 via TCAN1044 transceiver (U52). */

/* ------------------------------------------------------------------ */
/* Quadrature encoder channels (ALP_E1M_ENC<N> -> board-side encoder) */
/* ------------------------------------------------------------------ */

#define XEVK_ENC_ROTARY ALP_E1M_X_ENC0  /**< PEC12R-4222F rotary encoder: ENC0_X = A phase, ENC0_Y = B phase; push switch on XEVK_PIN_ENCODER_SW (E1M_X_GPIO_IO28).  ENC1-3 pads are broken out but unpopulated. */

/* ------------------------------------------------------------------ */
/* Portable cross-EVK aliases (e1m-spec STANDARD.md §7.2 common set). */
/* Same BOARD_* names on every board; include via <alp/board.h>.       */
/* ------------------------------------------------------------------ */

#define BOARD_CAN0            XEVK_CAN_BUS0
#define BOARD_DAC0            XEVK_DAC0
#define BOARD_DAC1            XEVK_DAC1
#define BOARD_ENC_ROTARY      XEVK_ENC_ROTARY
#define BOARD_I2C_SENSORS     XEVK_I2C_BUS_SENSORS
#define BOARD_I2S_AUDIO       XEVK_I2S_AUDIO
#define BOARD_PIN_BMI323_INT1 XEVK_PIN_BMI323_INT1
#define BOARD_PIN_ENCODER_SW  XEVK_PIN_ENCODER_SW
#define BOARD_PIN_LED_BLUE    XEVK_PIN_LED_BLUE
#define BOARD_PIN_LED_GREEN   XEVK_PIN_LED_GREEN
#define BOARD_PIN_LED_RED     XEVK_PIN_LED_RED
#define BOARD_PWM_ARD1        XEVK_ARD_PWM1
#define BOARD_PWM_ARD2        XEVK_ARD_PWM2
#define BOARD_PWM_ARD3        XEVK_ARD_PWM3
#define BOARD_PWM_LED_BLUE    XEVK_PWM_LED_BLUE
#define BOARD_PWM_LED_GREEN   XEVK_PWM_LED_GREEN
#define BOARD_PWM_LED_RED     XEVK_PWM_LED_RED
#define BOARD_SPI_ARDUINO     XEVK_SPI_BUS_ARDUINO
#define BOARD_UART_ARDUINO    XEVK_UART_PORT_ARDUINO
#define BOARD_UART_DEBUG      XEVK_UART_PORT_DEBUG

#ifdef __cplusplus
} /* extern "C" */
#endif

/* clang-format on */

#endif /* ALP_BOARDS_E1M_X_EVK_ROUTES_H */
