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
 * that the standard does NOT name — the EVK's RGB LED, rotary
 * encoder switch, IO-expander control lines, on-board sensor I2C
 * addresses, etc.
 *
 * For the underlying integers — bus/port instance IDs and
 * pad-level GPIO indices — `#include <alp/e1m_pinout.h>` directly.
 */

#ifndef ALP_BOARDS_ALP_E1M_EVK_AEN_H
#define ALP_BOARDS_ALP_E1M_EVK_AEN_H

#include "alp/e1m_pinout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* EVK feature → E1M-standard pin index                                */
/*                                                                     */
/* The mapping below reflects how the EVK schematic (UG-E1M-001)       */
/* wires each named feature to an E1M pad.  Each macro just renames    */
/* an `ALP_E1M_GPIO_*` constant for readability.                       */
/* ------------------------------------------------------------------ */

/* RGB LED -- E1M IO0 / IO1 / TBD pin assignments are still
 * placeholders awaiting confirmation from the EVK schematic.
 * Treat these as guesses until UG-E1M-001 §<LED-section> is
 * cross-checked.  See `project_pending_hw_configs` memory note. */
#define EVK_AEN_PIN_LED_RED        ALP_E1M_GPIO_IO0       /**< RGB LED red  channel (TBD-confirm). */
#define EVK_AEN_PIN_LED_GREEN      ALP_E1M_GPIO_IO1       /**< RGB LED green channel (TBD-confirm). */
/* IO2 is the MIPI CSI camera-mux SEL line (PI3WVR626 SEL pin),
 * not an LED.  The blue LED's actual pin is TBD until the user
 * confirms; do not assume IO2 here. */
#define EVK_AEN_PIN_CAM_MUX_SEL    ALP_E1M_GPIO_IO2       /**< PI3WVR626 SEL pin -- selects between camera A and camera B. See `evk_aen_cam_select_*` helpers below. */
#define EVK_AEN_PIN_ENCODER_SW     ALP_E1M_GPIO_IO3       /**< Rotary encoder push switch (active-low, internal pull-up). */
#define EVK_AEN_PIN_IO_EXP_INT     ALP_E1M_GPIO_IO4       /**< TCAL9538 I/O expander INT line. */
#define EVK_AEN_PIN_IO_EXP_RST     ALP_E1M_GPIO_IO5       /**< TCAL9538 I/O expander RST line. */

/* ------------------------------------------------------------------ */
/* MIPI CSI camera multiplexer (PI3WVR626XEBEX)                        */
/*                                                                     */
/* The EVK routes a single MIPI CSI lane pair from the SoM through a   */
/* 2:1 mux that picks between two camera inputs (A and B).  The mux's  */
/* SEL pin is driven by `EVK_AEN_PIN_CAM_MUX_SEL` (E1M IO2 / Alif      */
/* P12.5).  Per the PI3WVR626 datasheet convention SEL=0 -> A path,    */
/* SEL=1 -> B path.  Confirm against the device's TG (Truth Table)     */
/* if a respin changes the polarity.                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    EVK_AEN_CAM_A = 0,    /**< MIPI_CSI_<lane>_<P/N> -> A_MIPI_CSI_<lane>_<P/N> */
    EVK_AEN_CAM_B = 1,    /**< MIPI_CSI_<lane>_<P/N> -> B_MIPI_CSI_<lane>_<P/N> */
} evk_aen_cam_select_t;

/* The rotary encoder's quadrature signals run through the SoC's
 * hardware quadrature counter on E1M's `ENC0_X` / `ENC0_Y` pads.
 * Use the E1M-standard `ALP_E1M_GPIO_ENC0_X` / `_Y` indices when
 * driving them as raw GPIOs. */

/* ------------------------------------------------------------------ */
/* EVK bus assignments                                                 */
/* ------------------------------------------------------------------ */

/** Shared sensor + IO-expander + INA236 bus.  Maps to E1M's `I2C0`. */
#define EVK_AEN_I2C_BUS_SENSORS    ALP_E1M_I2C0

/** SPI for the M.2 Key M slot.  Maps to E1M's `SPI0`. */
#define EVK_AEN_SPI_BUS_M2_KEYM    ALP_E1M_SPI0

/** Console UART exposed on the JTAG/SWD-side debug header.  Maps to `UART0`. */
#define EVK_AEN_UART_PORT_DEBUG    ALP_E1M_UART0

/* ------------------------------------------------------------------ */
/* On-board sensor 7-bit I2C addresses (per UG-E1M-001 §14)            */
/* ------------------------------------------------------------------ */

#define EVK_AEN_I2C_ADDR_ICM42670   0x2Cu  /**< U12 — primary 6-axis IMU. */
#define EVK_AEN_I2C_ADDR_BMI323     0x68u  /**< U13 — secondary 6-axis IMU. */
#define EVK_AEN_I2C_ADDR_BMP581     0x47u  /**< U14 — barometric pressure. */
#define EVK_AEN_I2C_ADDR_TCAL9538   0x70u  /**< U35 — I/O expander.        */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_BOARDS_ALP_E1M_EVK_AEN_H */
