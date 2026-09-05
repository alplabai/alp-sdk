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
#define EVK_PIN_LED_GREEN      ALP_E1M_GPIO_PWM3  /**< RGB LED green -- the PWM3 pad driven as a digital GPIO. */
#define EVK_PIN_LED_RED        ALP_E1M_GPIO_PWM0  /**< RGB LED red -- the PWM0 pad driven as a digital GPIO. */
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

#define EVK_PWM_LED_RED   ALP_E1M_PWM0  /**< RGB LED red; schematic-wired via PWM0 (non-contiguous with G/B). */
#define EVK_PWM_LED_BLUE  ALP_E1M_PWM1  /**< RGB LED blue channel. */
#define EVK_ARD_PWM1      ALP_E1M_PWM1  /**< Arduino header CK_PWM1; shares E1M_PWM1 with LED_BLUE. */
#define EVK_ARD_PWM4      ALP_E1M_PWM2  /**< Arduino header CK_PWM4 = E1M_PWM2. */
#define EVK_PWM_LED_GREEN ALP_E1M_PWM3  /**< RGB LED green channel. */
#define EVK_ARD_PWM2      ALP_E1M_PWM4  /**< Arduino header CK_PWM2 = E1M_PWM4. */
#define EVK_ARD_PWM3      ALP_E1M_PWM5  /**< Arduino header CK_PWM3 = E1M_PWM5. */
#define EVK_MB_PWM        ALP_E1M_PWM6  /**< mikroBUS PWM pin. */

/* ------------------------------------------------------------------ */
/* ADC channels (ALP_E1M_ADC<N> -> board-side signal) */
/* ------------------------------------------------------------------ */

#define EVK_ADC_ARDUINO_A0    ALP_E1M_ADC0  /**< Arduino UNO header A0 analog input. SHARED with the mikroBUS click ANA pin (net CK_ANA reaches ARD.A0 through R52, R63 pulldown, C60 filter) -- see the Arduino-A0 / mikroBUS-ANA convenience macros in alp_e1m_evk.h. There is no BOARD_ID divider on this board; no BOARD_ID net exists in the EVK netlist. */
#define EVK_ADC_ARDUINO_A1    ALP_E1M_ADC1  /**< Arduino UNO header A1 analog input. */
#define EVK_ADC_ARDUINO_A2    ALP_E1M_ADC2  /**< Arduino UNO header A2 analog input. */
#define EVK_ADC_ARDUINO_A3    ALP_E1M_ADC3  /**< Arduino UNO header A3 analog input. */
#define EVK_ADC_ARDUINO_A4    ALP_E1M_ADC4  /**< Arduino UNO header A4 analog input (shared with I2C SDA on classic UNO boards). */
#define EVK_ADC_ARDUINO_A5    ALP_E1M_ADC5  /**< Arduino UNO header A5 analog input (shared with I2C SCL on classic UNO boards). */
#define EVK_ADC_DAC0_LOOPBACK ALP_E1M_ADC6  /**< DAC0 output loopback sense (net A6: R88 series from DAC0_OUT, R89 pulldown, C107 filter). NOT the mikroBUS AN pin -- mikroBUS ANA is shared with Arduino A0 on E1M_ADC0, see EVK_ADC_ARDUINO_A0. */
#define EVK_ADC_DAC1_LOOPBACK ALP_E1M_ADC7  /**< DAC1 output loopback sense (net A7: R90 series from DAC1_OUT, R91 pulldown, C108 filter). There is no VBAT net anywhere in the EVK netlist -- this channel cannot be used for battery-voltage sensing. */

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
#define EVK_I2C_ADDR_BMP581        0x47u  /**< U14 barometer. SDO is connected to VIO on this EVK (maintainer-confirmed 2026-09-05), which is the 0x47 strap; 0x46 would be SDO->GND. Matches 2 of 2 boards, which ACK at 0x47 with CHIP_ID(0x01)=0x50. */
#define EVK_I2C_ADDR_TCAL9538_MAIN 0x73u  /**< U35 main I/O expander (A1=1, A0=1). Handles LCD/camera/capacitive-touch control + four sensor interrupt inputs. CORRECTED 2026-09-05 from 0x72 / A1=1,A0=0 (alp-sdk#1974): the maintainer's EVK I2C schedule gives 1110011 = 0x73, and 2 of 2 boards answer there and are silent at 0x72. */
#define EVK_I2C_ADDR_TCAL9538      EVK_I2C_ADDR_TCAL9538_MAIN  /**< Alias for EVK_I2C_ADDR_TCAL9538_MAIN. */
#define EVK_I2C_ADDR_TCAL9538_PCIE 0x71u  /**< U37 PCIe I/O expander (A0=1, A1=0). NOT ASSEMBLED on this EVK revision -- confirmed by the maintainer 2026-09-05 (alp-sdk#1974): this revision, built for Alif, does not need the second expander. Consistent with the evidence: never observed on either board, and absent from the maintainer EVK I2C schedule, which lists only ONE TCAL9538 (U35, at 0x73). The entry is kept, not deleted, because it describes a real footprint that earlier/other revisions populate. Anything that would have used it -- the I2C-mux SEL, PCIe slot RST/WAKE/CLKREQ, M2E_ALERT -- has no expander behind it here. */
#define EVK_I2C_ADDR_TCA6408A_MAIN 0x20u  /**< U35 main I/O expander, TCA6408ARSVR alternative (R112 fitted, R145 DNP). PCA9538-register-compatible, so chips/tcal9538 drives it unchanged. BENCH-CONFIRMED 2026-06-16 on an EARLIER EVK revision: read back config=0xFF + a live input port. NOT OBSERVED on either 2026-09-05 board (0x20 a clean NACK on both) and absent from the maintainer EVK I2C schedule for this revision, which places U35 at 0x73. Treat 0x20 as an earlier-revision population; do not expect it on an Alif-revision EVK. */
#define EVK_I2C_ADDR_TAS2563_LOW   0x4Du  /**< U27 smart amp (AD0 = 10k to GND). CONFIRMED 2026-09-05: 0x4D, ACKing on 2 of 2 boards, and confirmed correct by the maintainer. */
#define EVK_I2C_ADDR_TAS2563_HIGH  0x4Eu  /**< U28 smart amp (AD0 = 10k to VDD). CONFIRMED 2026-09-05: 0x4E, ACKing on 2 of 2 boards, and confirmed correct by the maintainer. The TAS2563 broadcast address (0x48) was occupied on PRE-RESPIN boards by U32 INA236B (+V_CAM0 rail); the U32 re-strap to 0x4B from the next batch freed 0x48 at the hardware level. That does not make it usable from the SDK: 0x48 doesn't pin down one physical chip the way a strap address does, and tas2563_init() rejects 0x48 on every board revision regardless of direction, so firmware must unconditionally issue two targeted unit-address writes rather than a 0x48 broadcast. */
#define EVK_I2C_ADDR_INA236_3V3    0x40u  /**< U21 INA236A, +3V3 rail (20 mOhm shunt, 4.0 A max). A0 = GND. */
#define EVK_I2C_ADDR_INA236_1V8    0x41u  /**< U31 INA236A, +1V8 rail (20 mOhm shunt, 4.0 A max). A0 = V+. */
#define EVK_I2C_ADDR_INA236_VIO    0x42u  /**< U33 INA236A, +VIO rail (50 mOhm shunt, 1.6 A max). A0 = SDA. */
#define EVK_I2C_ADDR_INA236_VCAM0  0x4Bu  /**< U32 INA236B, +V_CAM0 rail (50 mOhm shunt, 1.6 A max). Re-strapped A0=SCL -> 0x4B from the next batch; PRE-RESPIN boards had it at 0x48, which collides with the TAS2563 broadcast address (unreadable there). */
#define EVK_I2C_ADDR_INA236_VCAM1  0x49u  /**< U34 INA236B, +V_CAM1 rail (50 mOhm shunt, 1.6 A max). A0 = V+. */
#define EVK_I2C_ADDR_INA236_5V     0x4Au  /**< U30 INA236B, +5V rail (20 mOhm shunt, 4.0 A max). A0 = SDA. NOT OBSERVED on 2026W36-0001 (alp-sdk#1975) -- but PRESENT and TI-confirmed on 2026W36-0003, which is instead missing 0x41 (U31, +1V8). So each of the two boards tested answers on exactly five of six INA236, and WHICH one is missing differs per board. 'U30 unpopulated' cannot explain both. Five-of-six looks batch-wide; the identity of the missing one is per-unit. Power characterisation on either board silently omits one rail. */

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
/* Overlay-extended pin-array indices (from `overlay_pins:`) */
/* ------------------------------------------------------------------ */

#define EVK_PIN_OVERLAY_BASE ALP_E1M_GPIO_COUNT

#define EVK_PIN_IO_EXP_INT (EVK_PIN_OVERLAY_BASE + 0u)  /**< AUDIO_CLK pad (E1M Z2 / Alif P9_6) repurposed as the I/O expander INT line on this EVK. When the audio path is in use the IO expander interrupt is unavailable; firmware should poll the expander instead. */
#define EVK_PIN_IO_EXP_RST (EVK_PIN_OVERLAY_BASE + 1u)  /**< SPI0_CS1 pad (E1M N1 / Alif P3_6) repurposed as the I/O expander reset line. When SPI0 is used with two chip-selects this pin can't double as IO_EXP_RST -- the EVK assumes SPI0 is in single-CS mode at most. */
#define EVK_PIN_AMP_FAULT  (EVK_PIN_OVERLAY_BASE + 2u)  /**< SPI0_MISO pad (E1M L1 / Alif P5_0) repurposed as the audio amplifier fault output (open-drain input from the amp). */
#define EVK_PIN_AMP_ENABLE (EVK_PIN_OVERLAY_BASE + 3u)  /**< SPI0_CS0 pad (E1M M1 / Alif P5_2) repurposed as the audio amplifier enable input (active-high). */
#define EVK_PIN_MB_INT     (EVK_PIN_OVERLAY_BASE + 4u)  /**< I2S1_SDI pad (E1M AH6 / Alif P13_4) repurposed as the mikroBUS click INT pin. Was earlier (mis)documented as CTP_INT; the user has since clarified that CTP_INT is on SPI1_CS1 (see EVK_PIN_CTP_INT below) and I2S1_SDI is the mikroBUS INT line. */
#define EVK_PIN_CK_DIO4    (EVK_PIN_OVERLAY_BASE + 5u)  /**< SPI0_MOSI pad (E1M M2 / Alif P5_1) repurposed as Arduino CK_DIO4 (digital I/O 4 on the Arduino UNO header). */
#define EVK_PIN_CK_DIO3    (EVK_PIN_OVERLAY_BASE + 6u)  /**< SPI0_SCLK pad (E1M N2) repurposed as Arduino CK_DIO3. NB: the Alif-side pad mapping for SPI0_SCLK is left blank in metadata/e1m_modules/aen/from-alif.tsv (user-supplied) and needs filling once the EVK schematic is cross-checked. */
#define EVK_PIN_CK_DIO2    (EVK_PIN_OVERLAY_BASE + 7u)  /**< I2S1_WS pad (E1M AG7 / Alif P2_7) repurposed as Arduino CK_DIO2. */
#define EVK_PIN_CK_DIO1    (EVK_PIN_OVERLAY_BASE + 8u)  /**< I2S1_SDO pad (E1M AG6 / Alif P13_5) repurposed as Arduino CK_DIO1. */
#define EVK_PIN_CK_RST     (EVK_PIN_OVERLAY_BASE + 9u)  /**< I2S1_SCLK pad (E1M AH7 / Alif P2_6) repurposed as Arduino CK_RST (the Arduino UNO header's RESET signal -- shields can pulse it low to force a reboot). */
#define EVK_PIN_CTP_INT    (EVK_PIN_OVERLAY_BASE + 10u)  /**< SPI1_CS1 pad (E1M AH8 -- CC3501E side, GPIO_15) repurposed as the capacitive touch panel interrupt input. Routed through the on-module CC3501E -- firmware reads CTP touches by registering an interrupt callback on the CC3501E's GPIO_15 via ALP_CC3501E_CMD_GPIO_SET_INTERRUPT. */

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
