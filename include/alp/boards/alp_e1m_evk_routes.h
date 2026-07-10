/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Auto-generated from metadata/boards/e1m-evk.yaml
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
/* GPIO routes (ALP_E1M_GPIO_IO<N> -> board-side feature) */
/* ------------------------------------------------------------------ */

#define EVK_PIN_CAM_MUX_SEL    ALP_E1M_GPIO_IO2  /**< PI3WVR626 SEL pin; see `evk_cam_select_*` enum + chips/cam_mux_pi3wvr626. */
#define EVK_PIN_ENCODER_SW     ALP_E1M_GPIO_IO4  /**< Rotary encoder push switch (PEC12R-4222F-S0024), 10k pull-up + 0.1uF debounce. Active-low. */
#define EVK_PIN_CAM_RST        ALP_E1M_GPIO_IO5  /**< Camera reset (active-low). Active-low. */
#define EVK_PIN_PCIE_IOEXP_INT ALP_E1M_GPIO_IO7  /**< INT input from the PCIe IO expander. */
#define EVK_PIN_I2S_MUX_EN     ALP_E1M_GPIO_IO8  /**< I2S0 74LVC157 /E (Alif side P7.1); drive low to enable mux. Active-low. */
#define EVK_PIN_PCIE_IOEXP_RST ALP_E1M_GPIO_IO9  /**< Reset output to the PCIe IO expander. */
#define EVK_PIN_PCIE0_I2C_EN   ALP_E1M_GPIO_IO10  /**< Drive high to enable I2C mux to the PCIe slot. */
#define EVK_PIN_USB2_MUX_SEL   ALP_E1M_GPIO_IO11  /**< USB2 TMUXHS221 select: 0 = USB connector, 1 = M.2 E-key USB. */
#define EVK_PIN_I2S_MUX_SEL    ALP_E1M_GPIO_IO13  /**< I2S0 74LVC157 S; 0 = TAS2563 amps, 1 = M.2 E-key I2S. */
#define EVK_PIN_BMI323_INT1    ALP_E1M_GPIO_IO15  /**< BMI323 INT1 (data-ready / motion / FIFO); CC3501E GPIO14. */
#define EVK_PIN_W_DISABLE2     ALP_E1M_GPIO_IO16  /**< Bluetooth disable (open-drain, active-low) on M.2 E-key. Active-low. */
#define EVK_PIN_W_DISABLE1     ALP_E1M_GPIO_IO17  /**< Wi-Fi disable (open-drain, active-low) on M.2 E-key. Active-low. */
#define EVK_PIN_M2E_SDIO_WAKE  ALP_E1M_GPIO_IO18  /**< M.2 E-key SDIO-path wake (active-low). Active-low. */
#define EVK_PIN_M2E_UART_WAKE  ALP_E1M_GPIO_IO19  /**< M.2 E-key UART-path wake (active-low). Active-low. */
#define EVK_PIN_SDIO_MUX_EN    ALP_E1M_GPIO_IO20  /**< SDIO 74LVC157 /E; drive low to enable mux. Active-low. */
#define EVK_PIN_SDIO_MUX_SEL   ALP_E1M_GPIO_IO21  /**< SDIO 74LVC157 S; 0 = M.2 E-key SDIO, 1 = microSD slot. */
#define EVK_PIN_PCIE_MUX_PD    ALP_E1M_GPIO_IO22  /**< Drive HIGH to power down all four PCIe lane muxes. */
#define EVK_PIN_PCIE_MUX_SEL   ALP_E1M_GPIO_IO23  /**< Selects M-key vs E-key routing on the PCIe lane muxes. */
#define EVK_PIN_LED_RED        ALP_E1M_GPIO_PWM3  /**< RGB LED red -- the PWM3 pad driven as a digital GPIO. */
#define EVK_PIN_LED_GREEN      ALP_E1M_GPIO_PWM0  /**< RGB LED green -- the PWM0 pad driven as a digital GPIO. */
#define EVK_PIN_LED_BLUE       ALP_E1M_GPIO_PWM1  /**< RGB LED blue -- the PWM1 pad driven as a digital GPIO. */

/* ------------------------------------------------------------------ */
/* Bus assignments (ALP_E1M_I2C / I3C / SPI / UART -> board role) */
/* ------------------------------------------------------------------ */

#define EVK_I2C_BUS_SENSORS   ALP_E1M_I2C0  /**< Shared sensor + IO-expander + INA236 bus. */
#define EVK_I2C_BUS_DSI_CSI   ALP_E1M_I2C1  /**< Display + camera control I2C (touch panel, camera-side I2C config). */
#define EVK_I2C_BUS_ARDUINO   ALP_E1M_I3C0  /**< Arduino UNO header I2C ride on I3C0 (I3C is backwards-compatible with classic I2C). */
#define EVK_SPI_BUS_ARDUINO   ALP_E1M_SPI1  /**< Arduino UNO header SPI; terminates on the on-module CC3501E, not the main SoC. */
#define EVK_UART_PORT_DEBUG   ALP_E1M_UART0  /**< Console UART exposed on the JTAG/SWD-side debug header. */
#define EVK_UART_PORT_ARDUINO ALP_E1M_UART1  /**< Arduino UNO header UART (D0/D1); CK_RXD = UART1_TX, CK_TXD = UART1_RX. */

/* ------------------------------------------------------------------ */
/* PWM channels (ALP_E1M_PWM<N> -> board-side feature) */
/* ------------------------------------------------------------------ */

#define EVK_PWM_LED_GREEN ALP_E1M_PWM0  /**< RGB LED green; schematic-wired via PWM0 (non-contiguous with R/B). */
#define EVK_PWM_LED_BLUE  ALP_E1M_PWM1  /**< RGB LED blue channel. */
#define EVK_ARD_PWM1      ALP_E1M_PWM1  /**< Arduino header CK_PWM1; shares E1M_PWM1 with LED_BLUE. */
#define EVK_ARD_PWM4      ALP_E1M_PWM2  /**< Arduino header CK_PWM4 = E1M_PWM2. */
#define EVK_PWM_LED_RED   ALP_E1M_PWM3  /**< RGB LED red channel. */
#define EVK_ARD_PWM2      ALP_E1M_PWM4  /**< Arduino header CK_PWM2 = E1M_PWM4. */
#define EVK_ARD_PWM3      ALP_E1M_PWM5  /**< Arduino header CK_PWM3 = E1M_PWM5. */
#define EVK_MB_PWM        ALP_E1M_PWM6  /**< mikroBUS PWM pin. */

/* ------------------------------------------------------------------ */
/* ADC channels (ALP_E1M_ADC<N> -> board-side signal) */
/* ------------------------------------------------------------------ */

#define EVK_ADC_BOARD_ID   ALP_E1M_ADC0  /**< Carrier-side BOARD_ID resistor divider (see docs/board-id.md). */
#define EVK_ADC_ARDUINO_A1 ALP_E1M_ADC1  /**< Arduino UNO header A1 analog input. */
#define EVK_ADC_ARDUINO_A2 ALP_E1M_ADC2  /**< Arduino UNO header A2 analog input. */
#define EVK_ADC_ARDUINO_A3 ALP_E1M_ADC3  /**< Arduino UNO header A3 analog input. */
#define EVK_ADC_ARDUINO_A4 ALP_E1M_ADC4  /**< Arduino UNO header A4 analog input (shared with I2C SDA on classic UNO boards). */
#define EVK_ADC_ARDUINO_A5 ALP_E1M_ADC5  /**< Arduino UNO header A5 analog input (shared with I2C SCL on classic UNO boards). */
#define EVK_ADC_MB_AN      ALP_E1M_ADC6  /**< mikroBUS click AN pin. */
#define EVK_ADC_VBAT_SENSE ALP_E1M_ADC7  /**< Battery voltage divider (4:1 resistor scale) for power-monitor demos. */

/* ------------------------------------------------------------------ */
/* DAC channels (ALP_E1M_DAC<N> -> board-side signal) */
/* ------------------------------------------------------------------ */

#define EVK_DAC_ARDUINO_DAC0   ALP_E1M_DAC0  /**< Arduino-shield-style DAC0 output exposed on header J3. */
#define EVK_DAC_AUDIO_LINE_OUT ALP_E1M_DAC1  /**< Auxiliary line-level audio output (analog, sums with TAS2563 mix). */

/* ------------------------------------------------------------------ */
/* I2S instances (ALP_E1M_I2S<N> -> board-side codec / mic role) */
/* ------------------------------------------------------------------ */

#define EVK_I2S_AUDIO_CODEC ALP_E1M_I2S0  /**< Routed through the 74LVC157 mux to either the TAS2563 amps (default) or the M.2 E-key I2S; see EVK_PIN_I2S_MUX_SEL. */
#define EVK_I2S_PDM_MIC     ALP_E1M_I2S1  /**< PDM mic capture (4x MP34DT05 mics). */

/* ------------------------------------------------------------------ */
/* CAN buses (ALP_E1M_CAN<N> -> board-side bus role) */
/* ------------------------------------------------------------------ */

#define EVK_CAN_VEHICLE_BUS ALP_E1M_CAN0  /**< TCAN1044A transceiver on header J9; termination via jumpers JP1-JP4. */

/* ------------------------------------------------------------------ */
/* Quadrature encoder channels (ALP_E1M_ENC<N> -> board-side encoder) */
/* ------------------------------------------------------------------ */

#define EVK_ENC_ROTARY ALP_E1M_ENC0  /**< PEC12R-4222F-S0024 rotary encoder: ENC0_X = A phase, ENC0_Y = B phase, 24 PPR; push switch on EVK_PIN_ENCODER_SW (E1M_GPIO_IO4). */

/* ------------------------------------------------------------------ */
/* On-board I2C device addresses (from `i2c_devices:`) */
/* ------------------------------------------------------------------ */

#define EVK_I2C_ADDR_ICM42670      0x69u  /**< U12 IMU (AD0->VIO). Collides with U13 @0x69 until the respin. BENCH-CONFIRMED 2026-06-16 (E1M-AEN801): U12 + U13 both answer at 0x69 and collide -- see EVK_I2C_ADDR_BMI323. */
#define EVK_I2C_ADDR_BMI323        0x68u  /**< U13 IMU; respin target (SDO->GND = datasheet default). Pre-respin batch mis-straps it to 0x69 (collides w/ U12, see EVK_I2C_ADDR_ICM42670). */
#define EVK_I2C_ADDR_BMP581        0x47u  /**< U14 barometer (SDO->VIO; 0x46 if SDO->GND). */
#define EVK_I2C_ADDR_TCAL9538_MAIN 0x72u  /**< U35 main I/O expander (A1=1, A0=0). Handles LCD/camera/capacitive-touch control + four sensor interrupt inputs. */
#define EVK_I2C_ADDR_TCAL9538      EVK_I2C_ADDR_TCAL9538_MAIN  /**< Alias for EVK_I2C_ADDR_TCAL9538_MAIN. */
#define EVK_I2C_ADDR_TCAL9538_PCIE 0x71u  /**< U37 PCIe I/O expander (A0=1, A1=0). Handles the I2C-mux SEL + PCIe slot RST/WAKE/CLKREQ signals + M2E_ALERT. */
#define EVK_I2C_ADDR_TCA6408A_MAIN 0x20u  /**< U35 main I/O expander, TCA6408ARSVR alternative (R112 fitted, R145 DNP). PCA9538-register-compatible, so chips/tcal9538 drives it unchanged. BENCH-CONFIRMED 2026-06-16: read back config=0xFF + a live input port. */
#define EVK_I2C_ADDR_TAS2563_LOW   0x4Du  /**< U27 smart amp (AD0 = 10k to GND). */
#define EVK_I2C_ADDR_TAS2563_HIGH  0x4Eu  /**< U28 smart amp (AD0 = 10k to VDD). The TAS2563 broadcast address (0x48) is occupied on this EVK by U32 INA236B (+V_CAM0 rail, pre-respin) -- firmware that wants to write both amps must issue two targeted unit-address writes. */
#define EVK_I2C_ADDR_INA236_3V3    0x40u  /**< U21 INA236A, +3V3 rail (20 mOhm shunt, 4.0 A max). A0 = GND. */
#define EVK_I2C_ADDR_INA236_1V8    0x41u  /**< U31 INA236A, +1V8 rail (20 mOhm shunt, 4.0 A max). A0 = V+. */
#define EVK_I2C_ADDR_INA236_VIO    0x42u  /**< U33 INA236A, +VIO rail (50 mOhm shunt, 1.6 A max). A0 = SDA. */
#define EVK_I2C_ADDR_INA236_VCAM0  0x4Bu  /**< U32 INA236B, +V_CAM0 rail (50 mOhm shunt, 1.6 A max). Re-strapped A0=SCL -> 0x4B from the next batch; PRE-RESPIN boards had it at 0x48, which collides with the TAS2563 broadcast address (unreadable there). */
#define EVK_I2C_ADDR_INA236_VCAM1  0x49u  /**< U34 INA236B, +V_CAM1 rail (50 mOhm shunt, 1.6 A max). A0 = V+. */
#define EVK_I2C_ADDR_INA236_5V     0x4Au  /**< U30 INA236B, +5V rail (20 mOhm shunt, 4.0 A max). A0 = SDA. */

/* ------------------------------------------------------------------ */
/* INA236 calibration constants (from `i2c_devices[].calibration`) */
/* ------------------------------------------------------------------ */

#define EVK_INA236_SHUNT_3V3_OHMS   0.020f  /**< Shunt for EVK_I2C_ADDR_INA236_3V3. */
#define EVK_INA236_MAX_3V3_A        4.0f  /**< Max current for EVK_I2C_ADDR_INA236_3V3. */
#define EVK_INA236_SHUNT_1V8_OHMS   0.020f  /**< Shunt for EVK_I2C_ADDR_INA236_1V8. */
#define EVK_INA236_MAX_1V8_A        4.0f  /**< Max current for EVK_I2C_ADDR_INA236_1V8. */
#define EVK_INA236_SHUNT_VIO_OHMS   0.050f  /**< Shunt for EVK_I2C_ADDR_INA236_VIO. */
#define EVK_INA236_MAX_VIO_A        1.6f  /**< Max current for EVK_I2C_ADDR_INA236_VIO. */
#define EVK_INA236_SHUNT_VCAM0_OHMS 0.050f  /**< Shunt for EVK_I2C_ADDR_INA236_VCAM0. */
#define EVK_INA236_MAX_VCAM0_A      1.6f  /**< Max current for EVK_I2C_ADDR_INA236_VCAM0. */
#define EVK_INA236_SHUNT_VCAM1_OHMS 0.050f  /**< Shunt for EVK_I2C_ADDR_INA236_VCAM1. */
#define EVK_INA236_MAX_VCAM1_A      1.6f  /**< Max current for EVK_I2C_ADDR_INA236_VCAM1. */
#define EVK_INA236_SHUNT_5V_OHMS    0.020f  /**< Shunt for EVK_I2C_ADDR_INA236_5V. */
#define EVK_INA236_MAX_5V_A         4.0f  /**< Max current for EVK_I2C_ADDR_INA236_5V. */

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
#define BOARD_PWM_LED_BLUE    EVK_PWM_LED_BLUE
#define BOARD_PWM_LED_GREEN   EVK_PWM_LED_GREEN
#define BOARD_PWM_LED_RED     EVK_PWM_LED_RED
#define BOARD_SPI_ARDUINO     EVK_SPI_BUS_ARDUINO
#define BOARD_UART_ARDUINO    EVK_UART_PORT_ARDUINO
#define BOARD_UART_DEBUG      EVK_UART_PORT_DEBUG

#ifdef __cplusplus
} /* extern "C" */
#endif

/* clang-format on */

#endif /* ALP_BOARDS_E1M_EVK_ROUTES_H */
