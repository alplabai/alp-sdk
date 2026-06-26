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
 * Pin/bus ROUTES (E1M_X_GPIO_IO<N> -> board feature, I2C/SPI/UART
 * bus roles, PWM channels) are generated from
 * `metadata/boards/e1m-x-evk.yaml` into the companion header
 * `<alp/boards/alp_e1m_x_evk_routes.h>` -- include that for pin
 * macros.  THIS header carries the hand-authored on-board chip
 * I2C addresses and the per-rail INA236 calibration constants,
 * mirroring `<alp/boards/alp_e1m_evk.h>` for the 35x35 EVK.
 *
 * On-board chips live on the sensor I2C bus
 * @ref XEVK_I2C_BUS_SENSORS (E1M_X_I2C0; Linux `i2c-0`).
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
 */
#define XEVK_I2C_ADDR_INA236_3V3 0x40u /**< U21 INA236A, +3V3 rail   (20 mOhm shunt, 4.0 A max). */
#define XEVK_I2C_ADDR_INA236_1V8 0x41u /**< U31 INA236A, +1V8 rail   (20 mOhm shunt, 4.0 A max). */
#define XEVK_I2C_ADDR_INA236_VCAM2                                                                 \
	0x48u /**< U32 INA236B, +VCAM2 rail (50 mOhm shunt, 1.6 A max). */
#define XEVK_I2C_ADDR_INA236_VCAM3                                                                 \
	0x49u                             /**< U34 INA236B, +VCAM3 rail (50 mOhm shunt, 1.6 A max). */
#define XEVK_I2C_ADDR_INA236_5V 0x4Au /**< U30 INA236B, +5V rail    (20 mOhm shunt, 4.0 A max). */

/* Per-rail shunt + max-current values for ina236_init().  Each
 * rail's shunt was picked to put its nominal max current near the
 * INA236's 81.92 mV full-scale shunt voltage:
 *   shunt_ohms * max_current_a ~= 0.080 V.
 * Apps can pass these directly:
 *   ina236_init(&ctx, bus,
 *               XEVK_I2C_ADDR_INA236_5V,
 *               XEVK_INA236_SHUNT_5V_OHMS,
 *               XEVK_INA236_MAX_5V_A,
 *               INA236_ADCRANGE_81MV); */
#define XEVK_INA236_SHUNT_3V3_OHMS   0.020f
#define XEVK_INA236_MAX_3V3_A        4.0f
#define XEVK_INA236_SHUNT_1V8_OHMS   0.020f
#define XEVK_INA236_MAX_1V8_A        4.0f
#define XEVK_INA236_SHUNT_VCAM2_OHMS 0.050f
#define XEVK_INA236_MAX_VCAM2_A      1.6f
#define XEVK_INA236_SHUNT_VCAM3_OHMS 0.050f
#define XEVK_INA236_MAX_VCAM3_A      1.6f
#define XEVK_INA236_SHUNT_5V_OHMS    0.020f
#define XEVK_INA236_MAX_5V_A         4.0f

#ifdef __cplusplus
}
#endif

#endif /* ALP_BOARDS_E1M_X_EVK_H */
