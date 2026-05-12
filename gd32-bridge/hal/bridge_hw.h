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

#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------- */
/* Performance notes for the future bridge_hw_gd32.c implementer:    */
/*                                                                  */
/* * The GD32G5 carries a hardware CRC unit (datasheet §95) that    */
/*   processes one byte per AHB clock vs the ~16 cycles the         */
/*   software byte-XOR loop in protocol.c needs.  The bridge wire   */
/*   protocol uses CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF,    */
/*   non-reflected, xor-out 0x0000); the CRC unit's user-           */
/*   configurable polynomial supports that pattern.  Plumb both     */
/*   transports' framing layers through the HW unit and the         */
/*   software fallback becomes a self-test reference only.          */
/*                                                                  */
/* * Both DMA controllers (DMA0 + DMA1, 7 channels each) are        */
/*   available -- bind one ADC stream's DMA to DMA0 channel 1 and   */
/*   the other to DMA1 channel 1 so they can run truly concurrently.*/
/*   The bridge transports themselves (SPI / I2C) can also DMA-back */
/*   their RX/TX FIFOs to free the CPU during bridge handler        */
/*   bodies.                                                         */
/* --------------------------------------------------------------- */

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

/* v0.3: sticky PWM tuning.  align_mode is one of
 *   0 = edge-aligned, 1 = center-aligned-up,
 *   2 = center-aligned-down, 3 = center-aligned-both (datasheet
 *   §17 CAM field).  dead_time_ns programs the GD32's break-and-
 *   dead-time register for complementary outputs (0 = no dead
 *   time).  break_cfg bit 0 enables the break input; remaining
 *   bits reserved. */
int bridge_hw_pwm_configure(uint8_t channel, uint8_t align_mode,
                            uint32_t dead_time_ns, uint8_t break_cfg);

/* --------------------------------------------------------------- */
/* ADC                                                              */
/* --------------------------------------------------------------- */

int bridge_hw_adc_read(uint8_t channel, uint8_t samples, uint16_t *mv);

/* v0.3: sticky ADC tuning.  oversample_ratio is one of
 * 1/2/4/8/16/32/64/128/256 (rounded down to nearest power-of-two
 * by the firmware).  sample_cycles is one of the eight datasheet
 * values (2/6/12/24/47/92/247/640 cycles, GD32G553 §16.4.6) -- the
 * firmware rounds down.  resolution is 6/8/10/12/14/16 bits (the
 * latter two require oversampling >= 4 / 16 respectively per the
 * datasheet's effective-resolution table). */
int bridge_hw_adc_configure(uint8_t channel, uint16_t oversample_ratio,
                            uint16_t sample_cycles, uint8_t resolution_bits);

/* v0.3: DMA-backed continuous acquisition.  Two streams supported
 * concurrently (`stream_id` selects which one) -- the firmware
 * binds stream 0 to DMA0 and stream 1 to DMA1 per the chip's two-DMA
 * controller setup.  STREAM_BEGIN starts the firmware's ring buffer
 * at the requested sample_rate_hz; STREAM_READ drains up to
 * `max_samples` samples (firmware caps at
 * GD32_BRIDGE_ADC_STREAM_READ_MAX); STREAM_END stops the DMA and
 * flushes the ring.  Buffer overruns are returned as STATUS_BUSY on
 * subsequent STREAM_READ. */
int bridge_hw_adc_stream_begin(uint8_t stream_id, uint8_t channel,
                               uint32_t sample_rate_hz);
int bridge_hw_adc_stream_read(uint8_t stream_id, uint8_t max_samples,
                              uint8_t *got_samples, uint16_t *mv);
int bridge_hw_adc_stream_end(uint8_t stream_id);

/* --------------------------------------------------------------- */
/* TRNG -- true random number generator (NIST SP800-90B)             */
/* --------------------------------------------------------------- */

/* Fill @p dest with @p len bytes of true randomness from the
 * GD32G5's TRNG unit.  Caller chooses len in [1..32]; the firmware
 * accumulates 32-bit pulls into the output until len is satisfied
 * (a single pull = ~40 TRNG_CLK cycles per the datasheet).  Returns
 * BRIDGE_HW_ERR_IO if the TRNG's startup or in-service self-check
 * flags an error. */
int bridge_hw_trng_read(uint8_t *dest, size_t len);

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
