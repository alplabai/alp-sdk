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

#define EVK_AEN_PIN_LED_RED        ALP_E1M_GPIO_IO0       /**< RGB LED red  channel control. */
#define EVK_AEN_PIN_LED_GREEN      ALP_E1M_GPIO_IO1       /**< RGB LED green channel.        */
#define EVK_AEN_PIN_LED_BLUE       ALP_E1M_GPIO_IO2       /**< RGB LED blue  channel.        */
#define EVK_AEN_PIN_ENCODER_SW     ALP_E1M_GPIO_IO3       /**< Rotary encoder push switch (active-low, internal pull-up). */
#define EVK_AEN_PIN_IO_EXP_INT     ALP_E1M_GPIO_IO4       /**< TCAL9538 I/O expander INT line. */
#define EVK_AEN_PIN_IO_EXP_RST     ALP_E1M_GPIO_IO5       /**< TCAL9538 I/O expander RST line. */

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
