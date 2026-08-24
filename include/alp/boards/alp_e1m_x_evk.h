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
 * bus roles, PWM channels) and the on-board INA236 addresses +
 * per-rail calibration constants are GENERATED from
 * `metadata/boards/e1m-x-evk.yaml`'s `e1m_routes:` and
 * `i2c_devices:` blocks into the companion header
 * `<alp/boards/alp_e1m_x_evk_routes.h>`, included below.  THIS
 * header carries the remaining hand-authored on-board chip I2C
 * addresses, mirroring `<alp/boards/alp_e1m_evk.h>` for the 35x35
 * EVK.
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
 * on-board I2C device facts (XEVK_I2C_ADDR_INA236_*,
 * XEVK_INA236_SHUNT_*_OHMS, XEVK_INA236_MAX_*_A).
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

#define XEVK_I2C_ADDR_BMI323   0x68u /**< U-IMU BMI323 6-axis IMU (alternate IMU). */
#define XEVK_I2C_ADDR_ICM42670 0x69u /**< Canonical primary IMU (ICM-42670). */
#define XEVK_I2C_ADDR_BMP581   0x47u /**< BMP581 barometer (SDO->VIO; 0x46 if SDO->GND). */
#define XEVK_I2C_ADDR_TCAL9538 0x72u /**< TCAL9538 I/O expander. */
#define XEVK_I2C_ADDR_EEPROM   0x50u /**< Board ID EEPROM (24-series). */

/* ================================================================== */
/* INA236 high-side current-shunt monitors (one per power rail)       */
/* ================================================================== */

/* Five INA236 monitors on XEVK_I2C_BUS_SENSORS.  Ref-des + rail +
 * A0 strap per the board schematic (Current Measurement.SchDoc):
 *
 *   U21  INA236A  3V3   rail   A0 = GND  -> 0x40  (20 mOhm)
 *   U31  INA236A  1V8   rail   A0 = V+   -> 0x41  (20 mOhm)
 *   U32  INA236B  VCAM2 rail   A0 = GND  -> 0x48  (50 mOhm)
 *   U34  INA236B  VCAM3 rail   A0 = V+   -> 0x49  (50 mOhm)
 *   U30  INA236B  5V    rail   A0 = SDA  -> 0x4A  (20 mOhm)
 *
 * INA236A occupies 0x40..0x43, INA236B occupies 0x48..0x4B (same
 * A0 strap encoding), so all five share the one bus.
 *
 * NEXT-REVISION board notes (observed on current silicon, 2026-06;
 * NOT exposed as macros -- do not rely on them):
 *   - 0x42 / 0x43 also ACK as INA236 (mfg-ID "TI") although the
 *     schematic BOM lists only the five monitors above; treated as
 *     a board anomaly, to be resolved on the next respin.
 *   - The 3V3 (U21) and 1V8 (U31) monitors read ~0 V on the bus-
 *     voltage register on current silicon (VBUS-sense wiring under
 *     investigation); their shunt/current path is unaffected.  5V
 *     (U30) reads correctly (~4.88 V / whole-board input current).
 *
 * XEVK_I2C_ADDR_INA236_3V3, _1V8, _VCAM2, _VCAM3 and _5V are
 * defined in the generated routes header (#1636).
 */

/* Per-rail shunt + max-current values for ina236_init().  Each
 * rail's shunt was picked to put its nominal max current near the
 * INA236's 81.92 mV full-scale shunt voltage:
 *   shunt_ohms * max_current_a ~= 0.080 V.
 * Apps can pass these directly:
 *   ina236_init(&ctx, bus,
 *               XEVK_I2C_ADDR_INA236_5V,
 *               XEVK_INA236_SHUNT_5V_OHMS,
 *               XEVK_INA236_MAX_5V_A,
 *               INA236_ADCRANGE_81MV);
 *
 * XEVK_INA236_SHUNT_*_OHMS and XEVK_INA236_MAX_*_A (3V3, 1V8, VCAM2,
 * VCAM3, 5V) are defined in the generated routes header (#1636). */

#ifdef __cplusplus
}
#endif

#endif /* ALP_BOARDS_E1M_X_EVK_H */
