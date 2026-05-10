/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file alp_e1m_evk_aen.h
 * @brief Carrier-feature names for the E1M EVK populated with an
 *        E1M-AEN family SoM (UG-E1M-001).
 *
 * Layered atop the E1M-standard fixed pinout in
 * `<alp/e1m_pinout.h>`.  This header only defines names for things
 * the standard does NOT name -- the EVK's RGB LED, rotary encoder,
 * IO-expander pins, on-board sensor I2C addresses, and the
 * carrier's repurposing of certain SoM pads (e.g. SPI0 pins as
 * GPIOs for amp / IO-expander control).
 *
 * For the underlying integers -- bus / port instance IDs and
 * pad-level GPIO indices -- `#include <alp/e1m_pinout.h>` directly.
 */

#ifndef ALP_BOARDS_ALP_E1M_EVK_AEN_H
#define ALP_BOARDS_ALP_E1M_EVK_AEN_H

#include "alp/e1m_pinout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* RGB LED (PWM channels)                                             */
/*                                                                    */
/* The EVK drives the three RGB LED channels through PWM, not as      */
/* simple GPIOs -- this gives the studio's UI block proper            */
/* brightness control out of the box.  Use alp_pwm_open() with        */
/* these channel IDs.                                                 */
/* ================================================================== */

#define EVK_AEN_PWM_LED_RED ALP_E1M_PWM3 /**< RGB LED red  channel. */
#define EVK_AEN_PWM_LED_GREEN                                                                      \
    ALP_E1M_PWM2 /**< RGB LED green channel (TBD-confirm; user-supplied wiring named PWM3 = R, PWM1 = B but the green-channel PWM index was not stated explicitly -- PWM2 is inferred from the natural R/G/B order). */
#define EVK_AEN_PWM_LED_BLUE ALP_E1M_PWM1 /**< RGB LED blue channel. */

/* ================================================================== */
/* Direct GPIO mappings (E1M-standard pads)                           */
/* ================================================================== */

#define EVK_AEN_PIN_CAM_MUX_SEL                                                                    \
    ALP_E1M_GPIO_IO2 /**< PI3WVR626 SEL pin. See `evk_aen_cam_select_*` enum below + chips/cam_mux_pi3wvr626. */
#define EVK_AEN_PIN_ENCODER_SW                                                                     \
    ALP_E1M_GPIO_IO3 /**< Rotary encoder push switch (active-low, internal pull-up). */
#define EVK_AEN_PIN_CAM_RST                                                                        \
    ALP_E1M_GPIO_IO5 /**< Camera reset (was previously misdocumented as IO_EXP_RST -- the schematic confirms IO5 -> CAM_RST and the I/O expander reset is on a different pad; see below). */

/* SDIO 74LVC157 multiplexer (M.2 E-key SDIO vs microSD card slot).
 *
 * Two 74LVC157 quad 2:1 muxes (U38 for data lines, U39 for CLK/CMD/RST)
 * pick which device drives the SoM's single SDIO bus.  Control pins:
 *
 *   /E (active-low enable) = E1M IO20 -- both U38 and U39 share /E
 *   S  (select)            = E1M IO21
 *
 * Per the 74LVC157 truth table:
 *   /E = 0, S = 0  ->  M.2 E-key SDIO routed to SoM
 *   /E = 0, S = 1  ->  microSD card slot routed to SoM
 *   /E = 1         ->  outputs Hi-Z (both buses isolated; safe-default)
 *
 * IMPORTANT: per the user-supplied wiring + this repo's
 * metadata/e1m_modules/aen/from-cc3501e.tsv, BOTH IO20 and IO21 are
 * proxied through the on-module CC3501E (GPIO26 and GPIO30 on the
 * CC3501E side).  Firmware drives the mux by dispatching
 * GPIO_WRITE commands to the CC3501E over the inter-chip SPI1
 * (see <alp/protocol/cc3501e.h>'s ALP_CC3501E_CMD_GPIO_WRITE),
 * NOT via Alif's GPIO peripheral. */
#define EVK_AEN_PIN_SDIO_MUX_EN                                                                    \
    ALP_E1M_GPIO_IO20 /**< /E -- drive low to enable the mux.  Routed through CC3501E. */
#define EVK_AEN_PIN_SDIO_MUX_SEL                                                                   \
    ALP_E1M_GPIO_IO21 /**< S  -- 0 = M.2 E SDIO, 1 = microSD.  Routed through CC3501E. */

typedef enum {
    EVK_AEN_SDIO_M2E_KEY = 0, /**< MUX_SEL.SDIO low. */
    EVK_AEN_SDIO_SDCARD  = 1, /**< MUX_SEL.SDIO high. */
} evk_aen_sdio_select_t;

/* The rotary encoder's quadrature signals run through the SoC's
 * hardware quadrature counter on E1M's `ENC0_X` / `ENC0_Y` pads.
 * Use the E1M-standard `ALP_E1M_GPIO_ENC0_X` / `_Y` indices when
 * driving them as raw GPIOs. */

/* ================================================================== */
/* Pads repurposed by the EVK (off the E1M GPIO_IO numbering)         */
/*                                                                    */
/* The EVK's schematic ties a few non-IO pads to GPIO functions.      */
/* These can't ride the global `ALP_E1M_GPIO_IO*` namespace -- their  */
/* pad indices live past the standard 42-entry GPIO array.  The       */
/* carrier's `alp,pin-array` overlay extends the array with the       */
/* extra entries (indices 42..N) and the macros below map a name to  */
/* that overlay-defined index.                                        */
/*                                                                    */
/* The actual integer values are populated by the EVK's overlay (in  */
/* `alplabai/alp-zephyr-modules`); for studio-codegen consumers the  */
/* studio's pin allocator handles it.  Until that overlay lands, the */
/* macros below resolve to `EVK_AEN_PIN_OVERLAY_BASE + offset` and a */
/* hand-written-firmware author needs to verify the overlay declares */
/* matching extra entries.                                            */
/* ================================================================== */

/** Base index for EVK overlay-extended `alp,pin-array` entries.  Sits
 *  just past the 42 standard entries so it never collides. */
#define EVK_AEN_PIN_OVERLAY_BASE ALP_E1M_GPIO_COUNT

/** AUDIO_CLK pad (E1M Z2 / Alif P9_6) repurposed as the I/O
 *  expander INT line on this EVK.  When the audio path is in
 *  use the IO expander interrupt is unavailable; firmware should
 *  poll the expander instead. */
#define EVK_AEN_PIN_IO_EXP_INT (EVK_AEN_PIN_OVERLAY_BASE + 0u)

/** SPI0_CS1 pad (E1M N1 / Alif P3_6) repurposed as the I/O
 *  expander reset line.  When SPI0 is used with two chip-selects
 *  this pin can't double as IO_EXP_RST -- the EVK assumes SPI0
 *  is in single-CS mode at most. */
#define EVK_AEN_PIN_IO_EXP_RST (EVK_AEN_PIN_OVERLAY_BASE + 1u)

/** SPI0_MISO pad (E1M L1 / Alif P5_0) repurposed as the audio
 *  amplifier fault output (open-drain input from the amp). */
#define EVK_AEN_PIN_AMP_FAULT (EVK_AEN_PIN_OVERLAY_BASE + 2u)

/** SPI0_CS0 pad (E1M M1 / Alif P5_2) repurposed as the audio
 *  amplifier enable input (active-high). */
#define EVK_AEN_PIN_AMP_ENABLE (EVK_AEN_PIN_OVERLAY_BASE + 3u)

/** I2S1_SDI pad (E1M AH6 / Alif P13_4) repurposed as the
 *  capacitive touch panel interrupt input.  When the audio I2S1
 *  RX path is in use the touch INT is unavailable; firmware
 *  should poll the touch IC instead. */
#define EVK_AEN_PIN_CTP_INT (EVK_AEN_PIN_OVERLAY_BASE + 4u)

/** SPI1_CS0 pad (E1M AH9 -- CC3501E side, GPIO_31) repurposed as
 *  the capacitive touch panel reset.  Routed through the CC3501E
 *  -- firmware drives this via ALP_CC3501E_CMD_GPIO_WRITE on the
 *  inter-chip SPI1, NOT via Alif's GPIO peripheral.
 *
 *  WARNING: the IO expander's P3 was earlier documented as
 *  CTP_RST too.  Two possible interpretations -- the user must
 *  confirm:
 *    (a) Only this pad is the real CTP_RST; expander P3 was a
 *        mis-label and is actually a different signal (e.g.
 *        CTP power-enable).
 *    (b) Both routes exist (e.g. one resets the touch IC, the
 *        other resets a level shifter).
 *  Until clarified, firmware should drive both to be safe. */
#define EVK_AEN_PIN_CTP_RST (EVK_AEN_PIN_OVERLAY_BASE + 5u)

/* SPI0_MOSI / SPI0_SCLK remain available as either SPI or GPIO
 * depending on the carrier-overlay's declaration -- the EVK
 * doesn't pin them down to a specific repurpose, so apps that
 * want to use SPI0 for actual SPI traffic CANNOT also drive the
 * AMP / IO_EXP signals above (they alias on the same SPI0 bus). */

/* ================================================================== */
/* MIPI CSI camera multiplexer (PI3WVR626XEBEX)                       */
/* ================================================================== */

typedef enum {
    EVK_AEN_CAM_A = 0, /**< MIPI_CSI_<lane>_<P/N> -> A_MIPI_CSI_<lane>_<P/N> */
    EVK_AEN_CAM_B = 1, /**< MIPI_CSI_<lane>_<P/N> -> B_MIPI_CSI_<lane>_<P/N> */
} evk_aen_cam_select_t;

/* ================================================================== */
/* TCAL9538 I/O expander pin layout                                   */
/*                                                                    */
/* The TCAL9538 sits on E1M_I2C0 at 7-bit address 0x72 (A1=1, A0=0    */
/* per the EVK schematic).  Its 8 GPIO pins fan out to the LCD /      */
/* camera / capacitive-touch control lines and four sensor interrupt  */
/* inputs.  Apps drive them via the chips/tcal9538 driver:            */
/*                                                                    */
/*    tcal9538_t io_exp;                                              */
/*    tcal9538_init(&io_exp, i2c_bus, 0x72);                          */
/*    tcal9538_set_direction(&io_exp,                                 */
/*        BIT(EVK_AEN_IOEXP_LCD_PWR_EN) |                             */
/*        BIT(EVK_AEN_IOEXP_LCD_RST) |                                */
/*        BIT(EVK_AEN_IOEXP_CAM_EN)   |                               */
/*        BIT(EVK_AEN_IOEXP_CTP_RST),                                 */
/*        TCAL9538_DIR_OUTPUT);                                       */
/*    tcal9538_set(&io_exp, EVK_AEN_IOEXP_CAM_EN, true);              */
/* ================================================================== */

typedef enum {
    EVK_AEN_IOEXP_LCD_PWR_EN     = 0, /**< P0: LCD power enable.            */
    EVK_AEN_IOEXP_LCD_RST        = 1, /**< P1: LCD reset.                    */
    EVK_AEN_IOEXP_CAM_EN         = 2, /**< P2: Camera enable LDO.            */
    EVK_AEN_IOEXP_CTP_RST        = 3, /**< P3: Capacitive touch panel reset. */
    EVK_AEN_IOEXP_ICM42670_INT1  = 4, /**< P4: ICM-42670 INT1 input.         */
    EVK_AEN_IOEXP_ICM42670_INT2  = 5, /**< P5: ICM-42670 INT2 input.         */
    EVK_AEN_IOEXP_ICM42670_FSYNC = 6, /**< P6: ICM-42670 frame-sync input.   */
    EVK_AEN_IOEXP_BMP581_INT1    = 7, /**< P7: BMP581 INT1 input.            */
} evk_aen_ioexp_pin_t;

/* ================================================================== */
/* EVK bus assignments                                                */
/* ================================================================== */

/** Shared sensor + IO-expander + INA236 bus.  Maps to E1M's `I2C0`. */
#define EVK_AEN_I2C_BUS_SENSORS ALP_E1M_I2C0

/** Display + camera control I2C bus (touch panel, camera-side I2C
 *  configuration).  Maps to E1M's `I2C1` per the EVK schematic's
 *  DSI_CSI_I2C net. */
#define EVK_AEN_I2C_BUS_DSI_CSI ALP_E1M_I2C1

/** SPI for the M.2 Key M slot.  Maps to E1M's `SPI0` -- but note
 *  that some SPI0 pads (CS0/CS1/MISO) are repurposed as GPIOs on
 *  this carrier (see "Pads repurposed by the EVK" above).  Apps
 *  that want to drive M.2 over real SPI must coordinate with the
 *  amp + I/O-expander control logic. */
#define EVK_AEN_SPI_BUS_M2_KEYM ALP_E1M_SPI0

/** Console UART exposed on the JTAG/SWD-side debug header.  Maps to `UART0`. */
#define EVK_AEN_UART_PORT_DEBUG ALP_E1M_UART0

/* ================================================================== */
/* On-board sensor 7-bit I2C addresses                                */
/*                                                                    */
/* All on E1M_I2C0 (the sensor bus).  Strap values per the EVK        */
/* schematic UG-E1M-001 + user-supplied confirmation:                 */
/*   - ICM-42670-P  AD0/SD0 = high  -> 0x69                           */
/*   - BMI323       SDO_MISO_ADR = high -> 0x69                       */
/*   - BMP581       SDO = high      -> 0x47                           */
/*   - TCAL9538     A1=1, A0=0       -> 0x72                          */
/*                                                                    */
/* WARNING: ICM-42670-P and BMI323 both compute to 0x69 with the      */
/* straps documented in the EVK schematic.  This is an apparent       */
/* address collision on E1M_I2C0.  Two possible explanations -- the   */
/* user should confirm:                                               */
/*                                                                    */
/*   (a) Only ONE of {U12 ICM-42670-P, U13 BMI323} is populated at a  */
/*       time (the two parts share a footprint family), and the      */
/*       collision never materialises in practice.                   */
/*   (b) One strap is actually different than documented and we have */
/*       two distinct addresses (e.g. BMI323 at 0x68).                */
/*                                                                    */
/* Until the user confirms, firmware that opens both at 0x69 will not */
/* work; pick the populated IMU at runtime via lsm6dso_init's         */
/* WHO_AM_I check (different magic byte per chip).                    */
/* ================================================================== */

#define EVK_AEN_I2C_ADDR_ICM42670 0x69u /**< U12 IMU (AD0=1).  See collision warning above. */
#define EVK_AEN_I2C_ADDR_BMI323 0x69u   /**< U13 IMU (SDO=1). See collision warning above. */
#define EVK_AEN_I2C_ADDR_BMP581 0x47u   /**< U14 barometer (SDO=1). */
#define EVK_AEN_I2C_ADDR_TCAL9538 0x72u /**< U35 I/O expander (A1=1, A0=0). */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_BOARDS_ALP_E1M_EVK_AEN_H */
