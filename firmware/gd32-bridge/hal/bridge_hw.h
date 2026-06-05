/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware-abstraction shim consumed by firmware/gd32-bridge/src/protocol.c.
 * Each function maps an opcode-level operation onto the GigaDevice
 * firmware library (timer / GPIO / ADC / DAC / etc.).
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
/* Transient resource starvation: the request is valid and the unit
 * healthy, but servicing it NOW would overrun the handler's reply
 * window (e.g. a max-length TRNG pull while the conditioning round
 * is mid-flight).  Maps to STATUS_BUSY -- hosts retry. */
#define BRIDGE_HW_ERR_BUSY       -5

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

/* Report what the channel's pad is ACTUALLY generating by reading the
 * timer registers back (auto-reload + compare, converted to ns) --
 * never a software echo of the last request.  Before the first
 * bridge_hw_pwm_set a channel reports the boot default (65.536 ms
 * period, 0 duty); the period is shared per underlying timer. */
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
 * at the requested sample_rate_hz -- realised by a dedicated pacing
 * timer (stream 0: TIMER5, stream 1: TIMER6) whose update-event TRGO
 * triggers one conversion per period via TRIGSEL, so the rate is a
 * REAL hardware contract: 1 Hz..100 kHz, quantised to the timer tick
 * (1 us at >=16 Hz, 100 us below), BRIDGE_HW_ERR_RANGE above the
 * cap.  One stream per ADC converter -- a second BEGIN on a channel
 * sharing the first stream's ADC answers BRIDGE_HW_ERR_INVAL.
 * STREAM_READ drains up to `max_samples` samples (firmware caps at
 * GD32_BRIDGE_ADC_STREAM_READ_MAX); STREAM_END stops the pacing
 * timer + DMA and restores the converter's single-shot state.
 * Buffer overruns are returned as STATUS_BUSY on subsequent
 * STREAM_READ. */
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
/* TMU -- CORDIC math accelerator                                    */
/* --------------------------------------------------------------- */

/* Issue one TMU compute (`function` selects sin/cos/.../hypot per
 * `gd32_bridge_tmu_function_t`; `format` is Q31 or IEEE-754-single
 * per `gd32_bridge_tmu_format_t`).  `in_a` is the single operand for
 * one-input functions; `in_b` is the second operand for atan2 /
 * hypot and ignored otherwise.  The raw 32-bit `*result_out` is in
 * the same format as the inputs -- the firmware does NOT cast or
 * scale on the caller's behalf.  Returns BRIDGE_HW_ERR_RANGE for
 * inputs outside the function's domain (e.g. sqrt(negative) in Q31)
 * and BRIDGE_HW_ERR_IO if the TMU flags a hardware fault. */
int bridge_hw_tmu_compute(uint8_t function, uint8_t format,
                          uint32_t in_a, uint32_t in_b,
                          uint32_t *result_out);

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

/* Returns the DA9292 fault-pin state byte:
 *   bit0 = DA9292_INT asserted (active-low net)
 *   bit1 = DA9292_TW  asserted (active-low net)
 *   bits 2-6 reserved (0)
 *   0xFF = "no sample available" sentinel
 * The CURRENT SoM revision wires the DA9292 fault nets only to the
 * Renesas (P37/P36) -- the GD32 has no connection to them (and no I2C
 * path to the PMIC), so this returns the 0xFF sentinel unconditionally
 * (schematic-verified 2026-06-04).  The packing is reserved for a
 * future HW rev that mirrors the nets onto GD32 inputs.  Today the
 * host samples the pins directly (chips/da9292 da9292_get_fault_pins(),
 * same packing) and reads PMC_STATUS_00 etc. over BRD_I2C via
 * chips/da9292. */
uint8_t bridge_hw_da9292_status_cached(void);

/* --------------------------------------------------------------- */
/* v0.5 (§2B.2) -- advanced timer extras                             */
/* --------------------------------------------------------------- */

/* PWM input-capture: reconfigure @p channel's pin as an input-capture
 * source so the firmware can latch the timer counter on each edge.
 * @p edge selects polarity: 0 = rising, 1 = falling, 2 = both.  The
 * GD32G5's TIMERx CHxIE bit + DMAEN enable the capture interrupt /
 * DMA; the firmware writes the latched (period, pulse_width) tuples
 * into a per-channel ring buffer that the host drains via
 * bridge_hw_pwm_capture_read.  Calling BEGIN on a channel that's
 * already configured for output (via bridge_hw_pwm_set) implicitly
 * stops the output. */
int bridge_hw_pwm_capture_begin(uint8_t channel, uint8_t edge);

/* Read one (period_ns, pulse_width_ns) tuple from the @p channel's
 * capture ring.  Both outputs are in nanoseconds against the GD32
 * core clock (~4.63 ns LSB at 216 MHz).  BRIDGE_HW_ERR_NOTIMPL if the
 * ring is empty (host should poll); BRIDGE_HW_ERR_INVAL if the
 * channel is not currently in capture mode. */
int bridge_hw_pwm_capture_read(uint8_t channel, uint32_t *period_ns, uint32_t *pulse_width_ns);

/* Stop the @p channel's input-capture session and RESTORE the
 * channel's PWM output stage (pad back to AF-output push-pull, the
 * channel unit re-programmed to PWM output at 0 % duty) so a
 * subsequent bridge_hw_pwm_set drives again.  The original "return
 * the pin to high-impedance" contract relied on pwm_set re-initing
 * the channel direction, which it never did -- one capture session
 * left the channel output-dead until reboot (silicon 2026-06-04).
 * Idempotent. */
int bridge_hw_pwm_capture_end(uint8_t channel);

/* One-shot pulse: drive @p channel high for @p pulse_ns then return
 * to low.  Implemented on the GD32 by setting OPM (one-pulse mode) on
 * the timer + programming period = pulse_ns.  The PWM stays in
 * one-pulse mode until the next bridge_hw_pwm_set call switches it
 * back to continuous output. */
int bridge_hw_pwm_single_pulse(uint8_t channel, uint32_t pulse_ns);

/* Configure master-slave timer sync.  @p master and @p slave name two
 * of TIMER0 / TIMER7 / TIMER19 by integer id; @p mode selects the
 * slave-mode select field (per GD32G553 §17.4.3 SMC bits):
 * 0 = disabled, 1 = reset, 2 = gated, 3 = trigger, 4 = external-clock,
 * 5 = encoder-mode-1 etc.  Used to synchronise multi-channel PWM
 * outputs across the three advanced-timer groups. */
int bridge_hw_timer_sync(uint8_t master, uint8_t slave, uint8_t mode);

/* --------------------------------------------------------------- */
/* v0.5 (§2B.3) -- system power-mode set                             */
/* --------------------------------------------------------------- */

/* Request a system-wide sleep transition.  @p mode is 0 = run (no-op),
 * 1 = sleep, 2 = deep-sleep, 3 = standby.  @p wake_bitmap selects which
 * wake sources the firmware should arm (ALP_POWER_WAKE_* bits;
 * platform-specific).  @p wake_after_ms is a max wall-clock wait, or 0
 * for "no timer wake".  The GD32 prepares the V2N supervisor handshake
 * + signals the Renesas SoC to enter the matching mode, then re-runs
 * the bridge handshake on wakeup so the host can resume bridge calls. */
int bridge_hw_power_mode_set(uint8_t mode, uint32_t wake_bitmap, uint32_t wake_after_ms);

/* --------------------------------------------------------------- */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                     */
/* --------------------------------------------------------------- */

/* Allocate a fresh DSP-chain handle from the firmware's pool.  The
 * pool is sized firmware-side; exhaustion returns
 * BRIDGE_HW_ERR_NOTIMPL on the stub path (real builds return a
 * NOMEM-equivalent that the protocol layer maps to STATUS_NOMEM).
 * Returned @p chain_id is opaque to the protocol layer and host. */
int bridge_hw_adc_dsp_chain_open(uint8_t *chain_id);

/* Upload one chunk of one stage's per-kind parameter blob into the
 * named chain at @p stage_index.  @p kind is one of FIR=0, IIR=1,
 * WINDOW=2, FFT=3 (mirrors @c alp_dsp_stage_kind_t).  The firmware
 * accumulates chunks at @p chunk_offset byte positions within an
 * internal `[chain_id][stage_index]` buffer of size
 * `GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES`; the per-stage assembly
 * is complete when the host has covered `[0, chunk_total_size)`.
 * The eventual per-kind layouts the firmware decodes are documented
 * in `docs/gd32-bridge-protocol.md` §3.x. */
int bridge_hw_adc_dsp_stage_push(uint8_t chain_id, uint8_t stage_index, uint8_t kind,
                                 uint16_t chunk_offset, uint16_t chunk_total_size,
                                 const uint8_t *chunk_data, size_t chunk_data_len);

/* Attach a fully-populated chain to a streaming ADC source previously
 * opened with bridge_hw_adc_stream_begin.  After bind, the stream's
 * samples flow through the chain instead of being delivered raw to
 * subsequent bridge_hw_adc_stream_read calls.  Binding fails
 * (BRIDGE_HW_ERR_INVAL) if the chain has unfinished stages or
 * violates the chain-ordering rules (FFT must be terminal; WINDOW
 * must immediately precede FFT). */
int bridge_hw_adc_dsp_chain_bind(uint8_t chain_id, uint8_t stream_id);

#endif /* GD32_BRIDGE_HAL_BRIDGE_HW_H */
