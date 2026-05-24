/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Auto-generated from metadata/boards/e1m-evk.yaml
 * by scripts/gen_board_header.py.  DO NOT EDIT BY HAND --
 * regenerate after changing the YAML.
 *
 * Mirrors the board YAML's `e1m_routes:` block into plain
 * `#define EVK_<NAME> E1M_<...>` lines so hand-written firmware
 * can keep using the board-named macros while the YAML stays
 * the single editable source of truth.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.6 generated; macro names + values track the board YAML.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_BOARDS_E1M_EVK_ROUTES_H
#define ALP_BOARDS_E1M_EVK_ROUTES_H

#include "alp/e1m_pinout.h"

/* This header is auto-generated; clang-format ignores it so the
 * generator's column-aligned `#define` blocks survive PR static
 * analysis without forcing 100-col wraps on long doc strings. */
/* clang-format off */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* GPIO routes (E1M_GPIO_IO<N> -> board-side feature) */
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
#define EVK_PIN_LED_RED        E1M_GPIO_PWM3  /**< RGB LED red -- the PWM3 pad driven as a digital GPIO. */
#define EVK_PIN_LED_GREEN      E1M_GPIO_PWM0  /**< RGB LED green -- the PWM0 pad driven as a digital GPIO. */
#define EVK_PIN_LED_BLUE       E1M_GPIO_PWM1  /**< RGB LED blue -- the PWM1 pad driven as a digital GPIO. */

/* ------------------------------------------------------------------ */
/* Bus assignments (E1M_I2C / I3C / SPI / UART -> board role) */
/* ------------------------------------------------------------------ */

#define EVK_I2C_BUS_SENSORS   E1M_I2C0  /**< Shared sensor + IO-expander + INA236 bus. */
#define EVK_I2C_BUS_DSI_CSI   E1M_I2C1  /**< Display + camera control I2C (touch panel, camera-side I2C config). */
#define EVK_I2C_BUS_ARDUINO   E1M_I3C0  /**< Arduino UNO header I2C ride on I3C0 (I3C is backwards-compatible with classic I2C). */
#define EVK_SPI_BUS_ARDUINO   E1M_SPI1  /**< Arduino UNO header SPI; terminates on the on-module CC3501E, not the main SoC. */
#define EVK_UART_PORT_DEBUG   E1M_UART0  /**< Console UART exposed on the JTAG/SWD-side debug header. */
#define EVK_UART_PORT_ARDUINO E1M_UART1  /**< Arduino UNO header UART (D0/D1); CK_RXD = UART1_TX, CK_TXD = UART1_RX. */

/* ------------------------------------------------------------------ */
/* PWM channels (E1M_PWM<N> -> board-side feature) */
/* ------------------------------------------------------------------ */

#define EVK_PWM_LED_GREEN E1M_PWM0  /**< RGB LED green; schematic-wired via PWM0 (non-contiguous with R/B). */
#define EVK_PWM_LED_BLUE  E1M_PWM1  /**< RGB LED blue channel. */
#define EVK_ARD_PWM1      E1M_PWM1  /**< Arduino header CK_PWM1; shares E1M_PWM1 with LED_BLUE. */
#define EVK_ARD_PWM4      E1M_PWM2  /**< Arduino header CK_PWM4 = E1M_PWM2. */
#define EVK_PWM_LED_RED   E1M_PWM3  /**< RGB LED red channel. */
#define EVK_ARD_PWM2      E1M_PWM4  /**< Arduino header CK_PWM2 = E1M_PWM4. */
#define EVK_ARD_PWM3      E1M_PWM5  /**< Arduino header CK_PWM3 = E1M_PWM5. */
#define EVK_MB_PWM        E1M_PWM6  /**< mikroBUS PWM pin. */

/* ------------------------------------------------------------------ */
/* ADC channels (E1M_ADC<N> -> board-side signal) */
/* ------------------------------------------------------------------ */

#define EVK_ADC_BOARD_ID   E1M_ADC0  /**< Carrier-side BOARD_ID resistor divider (see docs/board-id.md). */
#define EVK_ADC_ARDUINO_A1 E1M_ADC1  /**< Arduino UNO header A1 analog input. */
#define EVK_ADC_ARDUINO_A2 E1M_ADC2  /**< Arduino UNO header A2 analog input. */
#define EVK_ADC_ARDUINO_A3 E1M_ADC3  /**< Arduino UNO header A3 analog input. */
#define EVK_ADC_ARDUINO_A4 E1M_ADC4  /**< Arduino UNO header A4 analog input (shared with I2C SDA on classic UNO boards). */
#define EVK_ADC_ARDUINO_A5 E1M_ADC5  /**< Arduino UNO header A5 analog input (shared with I2C SCL on classic UNO boards). */
#define EVK_ADC_MB_AN      E1M_ADC6  /**< mikroBUS click AN pin. */
#define EVK_ADC_VBAT_SENSE E1M_ADC7  /**< Battery voltage divider (4:1 resistor scale) for power-monitor demos. */

/* ------------------------------------------------------------------ */
/* DAC channels (E1M_DAC<N> -> board-side signal) */
/* ------------------------------------------------------------------ */

#define EVK_DAC_ARDUINO_DAC0   E1M_DAC0  /**< Arduino-shield-style DAC0 output exposed on header J3. */
#define EVK_DAC_AUDIO_LINE_OUT E1M_DAC1  /**< Auxiliary line-level audio output (analog, sums with TAS2563 mix). */

/* ------------------------------------------------------------------ */
/* I2S instances (E1M_I2S<N> -> board-side codec / mic role) */
/* ------------------------------------------------------------------ */

#define EVK_I2S_AUDIO_CODEC E1M_I2S0  /**< Routed through the 74LVC157 mux to either the TAS2563 amps (default) or the M.2 E-key I2S; see EVK_PIN_I2S_MUX_SEL. */
#define EVK_I2S_PDM_MIC     E1M_I2S1  /**< PDM mic capture (4x MP34DT05 mics). */

/* ------------------------------------------------------------------ */
/* CAN buses (E1M_CAN<N> -> board-side bus role) */
/* ------------------------------------------------------------------ */

#define EVK_CAN_VEHICLE_BUS E1M_CAN0  /**< TCAN1044A transceiver on header J9; termination via jumpers JP1-JP4. */

/* ------------------------------------------------------------------ */
/* Quadrature encoder channels (E1M_ENC<N> -> board-side encoder) */
/* ------------------------------------------------------------------ */

#define EVK_ENC_ROTARY E1M_ENC0  /**< PEC12R-4222F-S0024 rotary encoder: ENC0_X = A phase, ENC0_Y = B phase, 24 PPR; push switch on EVK_PIN_ENCODER_SW (E1M_GPIO_IO4). */

/* ------------------------------------------------------------------ */
/* Portable cross-EVK aliases (e1m-spec STANDARD.md §7.2 common set). */
/* Same BOARD_* names on every board; include via <alp/board.h>.       */
/* ------------------------------------------------------------------ */

#define BOARD_CAN0            EVK_CAN_VEHICLE_BUS
#define BOARD_DAC0            EVK_DAC_ARDUINO_DAC0
#define BOARD_DAC1            EVK_DAC_AUDIO_LINE_OUT
#define BOARD_ENC_ROTARY      EVK_ENC_ROTARY
#define BOARD_I2C_SENSORS     EVK_I2C_BUS_SENSORS
#define BOARD_I2S_AUDIO       EVK_I2S_AUDIO_CODEC
#define BOARD_PIN_BMI323_INT1 EVK_PIN_BMI323_INT1
#define BOARD_PIN_ENCODER_SW  EVK_PIN_ENCODER_SW
#define BOARD_PIN_LED_BLUE    EVK_PIN_LED_BLUE
#define BOARD_PIN_LED_GREEN   EVK_PIN_LED_GREEN
#define BOARD_PIN_LED_RED     EVK_PIN_LED_RED
#define BOARD_PWM_ARD1        EVK_ARD_PWM1
#define BOARD_PWM_ARD2        EVK_ARD_PWM2
#define BOARD_PWM_ARD3        EVK_ARD_PWM3
#define BOARD_SPI_ARDUINO     EVK_SPI_BUS_ARDUINO
#define BOARD_UART_ARDUINO    EVK_UART_PORT_ARDUINO
#define BOARD_UART_DEBUG      EVK_UART_PORT_DEBUG

#ifdef __cplusplus
} /* extern "C" */
#endif

/* clang-format on */

#endif /* ALP_BOARDS_E1M_EVK_ROUTES_H */
