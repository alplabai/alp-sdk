/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file alp_e1m_x_evk.h
 * @brief Board-feature names for the E1M-X EVK (the 45x65 mm carrier).
 *
 * The E1M-X EVK accepts 45x65 mm E1M-X-standard SoMs.  Currently
 * shipping families that fit:
 *   - E1M-X-V2N  (Renesas RZ/V2N, with optional DEEPX DX-M1)
 *
 * Pin/bus ROUTES (ALP_E1M_X_GPIO_IO<N> -> board feature, I2C/SPI/UART
 * bus roles, PWM channels), the on-board sensor I2C addresses, and
 * the INA236 addresses + per-rail calibration constants are all
 * GENERATED from `metadata/boards/e1m-x-evk.yaml`'s `e1m_routes:`
 * and `i2c_devices:` blocks into the companion header
 * `<alp/boards/alp_e1m_x_evk_routes.h>`, included below.  THIS
 * header carries only hand-authored prose context (mirroring
 * `<alp/boards/alp_e1m_evk.h>` for the 35x35 EVK) -- the macros
 * themselves live in the generated header.
 *
 * On-board chips live on the sensor I2C bus
 * @ref XEVK_I2C_BUS_SENSORS (ALP_E1M_X_I2C0; Linux `i2c-0`).
 *
 * @par Verification status: [BENCH-CONFIRMED ADDRESSES] -- the I2C
 *      addresses + INA236 register semantics below were confirmed
 *      on E1M-X-V2N silicon (chip-ID reads on i2c-0, 2026-06).
 *      The INA236 shunt/max-current pairs come from the board
 *      schematic (Current Measurement.SchDoc); current/power
 *      end-to-end accuracy is pending the HiL sweep.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.7 new -- first E1M-X EVK board header (sensor addresses +
 *      INA236 rail calibration).  See docs/abi-markers.md.
 */

#ifndef ALP_BOARDS_E1M_X_EVK_H
#define ALP_BOARDS_E1M_X_EVK_H

/* GENERATED board route bindings (XEVK_PIN_*, XEVK_*_BUS_*,
 * XEVK_UART_PORT_*, XEVK_PWM_*, XEVK_ARD_PWM*) and GENERATED
 * on-board I2C device facts (XEVK_I2C_ADDR_*, XEVK_INA236_SHUNT_*_OHMS,
 * XEVK_INA236_MAX_*_A).
 * Source of truth: metadata/boards/e1m-x-evk.yaml
 * `e1m_routes:` and `i2c_devices:` blocks. Regenerate via:
 *     python scripts/gen_board_header.py
 * The prose blocks below describe the hardware those macros bind
 * to; the macro values themselves come from the included header. */
#include "alp/boards/alp_e1m_x_evk_routes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* On-board sensor I2C addresses (bus = XEVK_I2C_BUS_SENSORS)          */
/* ================================================================== */

/* BMI323 (alternate IMU), ICM-42670 (canonical primary IMU), BMP581
 * barometer, TCAL9538 I/O expander, and the board-ID EEPROM.
 * XEVK_I2C_ADDR_BMI323, _ICM42670, _BMP581, _TCAL9538 and _EEPROM
 * are defined in the generated routes header (#1636). */

/* ================================================================== */
/* INA236 high-side current-shunt monitors (one per power rail)       */
/* ================================================================== */

/* Four INA236 monitors (U21/U31/U32/U34) on XEVK_I2C_BUS_SENSORS.
 * INA236A occupies 0x40..0x43, INA236B occupies 0x48..0x4B (same A0
 * strap encoding).  The +5V input monitor U30 is an INA228 at 0x42
 * (XEVK_I2C_ADDR_INA228_5V), a different 20-bit register map that
 * ina236_init() does not drive.  Ref-des, rail, A0 strap and address
 * per device are in the generated routes header (from
 * metadata/boards/e1m-x-evk.yaml's `i2c_devices:` block).
 *
 * Board note (NOT exposed as macros -- do not rely on it): the 3V3
 * (U21) and 1V8 (U31) monitors read ~0 V on the bus-voltage register
 * (VBUS-sense wiring under investigation); their shunt/current path
 * is unaffected.
 *
 * XEVK_I2C_ADDR_INA236_3V3, _1V8 and _VCAM3 are defined in the
 * generated routes header (#1636).
 */

/* Per-rail shunt + max-current values for ina236_init().  Each
 * rail's shunt was picked to put its nominal max current near the
 * INA236's 81.92 mV full-scale shunt voltage:
 *   shunt_ohms * max_current_a ~= 0.080 V.
 * Apps can pass these directly:
 *   ina236_init(&ctx, bus,
 *               XEVK_I2C_ADDR_INA236_3V3,
 *               XEVK_INA236_SHUNT_3V3_OHMS,
 *               XEVK_INA236_MAX_3V3_A,
 *               INA236_ADCRANGE_81MV);
 *
 * XEVK_INA236_SHUNT_*_OHMS and XEVK_INA236_MAX_*_A (3V3, 1V8,
 * VCAM3) are defined in the generated routes header (#1636). */

#ifdef __cplusplus
}
#endif

#endif /* ALP_BOARDS_E1M_X_EVK_H */
