/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default HAL implementation: every operation returns
 * BRIDGE_HW_ERR_NOTIMPL.  The protocol layer translates these to
 * STATUS_IO / STATUS_NOSUPPORT on the wire so host code can
 * distinguish "firmware doesn't speak this opcode" (NOSUPPORT, from
 * the dispatcher) from "firmware speaks the opcode but the
 * underlying HW operation failed" (IO).
 *
 * The real implementation against the GigaDevice firmware library
 * lives in bridge_hw_gd32.c; the build picks one or the other via
 * BRIDGE_HAL_BACKEND in CMakeLists.txt.
 */

#include <stdint.h>

#include "bridge_hw.h"

uint8_t bridge_hw_reset_reason(void)
{
    return 0u; /* UNKNOWN */
}

int bridge_hw_gpio_read(uint32_t mask, uint32_t *levels)
{
    (void)mask;
    if (levels != 0) *levels = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_gpio_write(uint32_t mask, uint32_t levels)
{
    (void)mask; (void)levels;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_set(uint8_t channel, uint32_t period_ns, uint32_t duty_ns)
{
    (void)channel; (void)period_ns; (void)duty_ns;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_get(uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns)
{
    (void)channel;
    if (period_ns != 0) *period_ns = 0u;
    if (duty_ns != 0)   *duty_ns   = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_read(uint8_t channel, uint8_t samples, uint16_t *mv)
{
    (void)channel;
    for (uint8_t i = 0; i < samples && mv != 0; ++i) {
        mv[i] = 0u;
    }
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_configure(uint8_t channel, uint8_t align_mode,
                            uint32_t dead_time_ns, uint8_t break_cfg)
{
    (void)channel; (void)align_mode; (void)dead_time_ns; (void)break_cfg;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_configure(uint8_t channel, uint16_t oversample_ratio,
                            uint16_t sample_cycles, uint8_t resolution_bits)
{
    (void)channel; (void)oversample_ratio; (void)sample_cycles;
    (void)resolution_bits;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_stream_begin(uint8_t stream_id, uint8_t channel,
                               uint32_t sample_rate_hz)
{
    (void)stream_id; (void)channel; (void)sample_rate_hz;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_stream_read(uint8_t stream_id, uint8_t max_samples,
                              uint8_t *got_samples, uint16_t *mv)
{
    (void)stream_id; (void)max_samples; (void)mv;
    if (got_samples != 0) *got_samples = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_stream_end(uint8_t stream_id)
{
    (void)stream_id;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_trng_read(uint8_t *dest, size_t len)
{
    if (dest != 0) {
        for (size_t i = 0; i < len; ++i) dest[i] = 0u;
    }
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_tmu_compute(uint8_t function, uint8_t format,
                          uint32_t in_a, uint32_t in_b,
                          uint32_t *result_out)
{
    (void)function; (void)format; (void)in_a; (void)in_b;
    if (result_out != 0) *result_out = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_dac_set(uint8_t channel, uint16_t value_mv)
{
    (void)channel; (void)value_mv;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_dac_get(uint8_t channel, uint16_t *value_mv)
{
    (void)channel;
    if (value_mv != 0) *value_mv = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_qenc_read(uint8_t encoder, int32_t *position)
{
    (void)encoder;
    if (position != 0) *position = 0;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_qenc_reset(uint8_t encoder)
{
    (void)encoder;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_counter_read(uint8_t counter, uint32_t *ticks)
{
    (void)counter;
    if (ticks != 0) *ticks = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

uint8_t bridge_hw_da9292_status_cached(void)
{
    return 0xFFu; /* "no PMIC poll has populated the cache yet" sentinel */
}

/* --------------------------------------------------------------- */
/* v0.5 (§2B.2) -- advanced timer extras                             */
/* --------------------------------------------------------------- */

int bridge_hw_pwm_capture_begin(uint8_t channel, uint8_t edge)
{
    (void)channel;
    (void)edge;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_capture_read(uint8_t channel, uint32_t *period_ns, uint32_t *pulse_width_ns)
{
    (void)channel;
    if (period_ns != 0) *period_ns = 0u;
    if (pulse_width_ns != 0) *pulse_width_ns = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_capture_end(uint8_t channel)
{
    (void)channel;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_single_pulse(uint8_t channel, uint32_t pulse_ns)
{
    (void)channel;
    (void)pulse_ns;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_timer_sync(uint8_t master, uint8_t slave, uint8_t mode)
{
    (void)master;
    (void)slave;
    (void)mode;
    return BRIDGE_HW_ERR_NOTIMPL;
}

/* --------------------------------------------------------------- */
/* v0.5 (§2B.3) -- system power-mode set                             */
/* --------------------------------------------------------------- */

int bridge_hw_power_mode_set(uint8_t mode, uint32_t wake_bitmap, uint32_t wake_after_ms)
{
    (void)mode;
    (void)wake_bitmap;
    (void)wake_after_ms;
    return BRIDGE_HW_ERR_NOTIMPL;
}

/* --------------------------------------------------------------- */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                     */
/* --------------------------------------------------------------- */

int bridge_hw_adc_dsp_chain_open(uint8_t *chain_id)
{
    if (chain_id != 0) *chain_id = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_dsp_stage_push(uint8_t chain_id, uint8_t stage_index, uint8_t kind,
                                 uint16_t chunk_offset, uint16_t chunk_total_size,
                                 const uint8_t *chunk_data, size_t chunk_data_len)
{
    (void)chain_id;
    (void)stage_index;
    (void)kind;
    (void)chunk_offset;
    (void)chunk_total_size;
    (void)chunk_data;
    (void)chunk_data_len;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_dsp_chain_bind(uint8_t chain_id, uint8_t stream_id)
{
    (void)chain_id;
    (void)stream_id;
    return BRIDGE_HW_ERR_NOTIMPL;
}
