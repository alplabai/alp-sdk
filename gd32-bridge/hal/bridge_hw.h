/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware-abstraction shim consumed by gd32-bridge/src/protocol.c.
 * Each function maps an opcode-level operation onto the GigaDevice
 * firmware library (timer / GPIO / ADC / I2C-master / etc.).
 *
 * The default implementations in hal/bridge_hw_stub.c return
 * BRIDGE_HW_ERR_NOTIMPL so the protocol round-trip can be smoke-tested
 * without the GigaDevice library being on the workspace yet.  The
 * real implementations land in hal/bridge_hw_gd32.c alongside the
 * GigaDevice firmware library pull (TBD: maintainer to drop the
 * vendor library at vendors/gd32_firmware_library/ and flip a
 * CMake flag).
 */

#ifndef GD32_BRIDGE_HAL_BRIDGE_HW_H
#define GD32_BRIDGE_HAL_BRIDGE_HW_H

#include <stdint.h>

/* Negative return values from any bridge_hw_* call.  Positive return
 * values are treated as "operation-specific success state" (currently
 * unused). */
#define BRIDGE_HW_OK              0
#define BRIDGE_HW_ERR_NOTIMPL    -1
#define BRIDGE_HW_ERR_INVAL      -2
#define BRIDGE_HW_ERR_RANGE      -3
#define BRIDGE_HW_ERR_IO         -4

/* --------------------------------------------------------------- */
/* Reset-cause                                                       */
/* --------------------------------------------------------------- */

/* Returns the cached cause of the most recent reset.  Reading
 * latches the cause to UNKNOWN for the next reader so chains of
 * callers don't see the same event twice. */
uint8_t bridge_hw_reset_reason(void);

/* --------------------------------------------------------------- */
/* GPIO                                                              */
/* --------------------------------------------------------------- */

/* Read the GD32's pad levels under @p mask.  Output @p levels has
 * bit i set iff (mask bit i set) and (pad reads high). */
int bridge_hw_gpio_read(uint32_t mask, uint32_t *levels);

/* Atomically set/clear the pad outputs selected by @p mask to the
 * corresponding bit in @p levels. */
int bridge_hw_gpio_write(uint32_t mask, uint32_t levels);

/* --------------------------------------------------------------- */
/* PWM                                                              */
/* --------------------------------------------------------------- */

int bridge_hw_pwm_set(uint8_t channel, uint32_t period_ns, uint32_t duty_ns);
int bridge_hw_pwm_get(uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns);

/* --------------------------------------------------------------- */
/* ADC                                                              */
/* --------------------------------------------------------------- */

int bridge_hw_adc_read(uint8_t channel, uint8_t samples, uint16_t *mv);

/* --------------------------------------------------------------- */
/* DAC                                                              */
/* --------------------------------------------------------------- */

/* Set the @p channel DAC output to @p value_mv (millivolts).  The
 * firmware rounds to its hardware-achievable resolution. */
int bridge_hw_dac_set(uint8_t channel, uint16_t value_mv);

/* Read back the currently-programmed @p channel DAC output in mV. */
int bridge_hw_dac_get(uint8_t channel, uint16_t *value_mv);

/* --------------------------------------------------------------- */
/* Quadrature encoder                                                */
/* --------------------------------------------------------------- */

/* Read the @p encoder's accumulated signed count since the last
 * reset (or since boot, whichever is more recent). */
int bridge_hw_qenc_read(uint8_t encoder, int32_t *position);

/* Reset the @p encoder's accumulated count to zero. */
int bridge_hw_qenc_reset(uint8_t encoder);

/* --------------------------------------------------------------- */
/* Free-running counter                                              */
/* --------------------------------------------------------------- */

/* Read the @p counter's current tick value.  Tick frequency is
 * firmware-defined and currently fixed; a future GET_FREQ opcode
 * will let the host convert ticks to microseconds without
 * out-of-band knowledge. */
int bridge_hw_counter_read(uint8_t counter, uint32_t *ticks);

/* --------------------------------------------------------------- */
/* DA9292 forward                                                    */
/* --------------------------------------------------------------- */

/* Returns the most recent cached snapshot of the DA9292's
 * PMC_STATUS_00 byte.  Refreshed by a periodic I2C-master poll on
 * the GD32 side (see hal/bridge_hw_gd32.c). */
uint8_t bridge_hw_da9292_status_cached(void);

#endif /* GD32_BRIDGE_HAL_BRIDGE_HW_H */
