/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 backend for the bridge HAL.  Selected by setting
 * BRIDGE_HAL_BACKEND=gd32 in gd32-bridge/CMakeLists.txt.  Links
 * against the GigaDevice firmware-library wrapper at
 * vendors/gd32_firmware_library/ (git submodule pointing at
 * https://github.com/alplabai/gd32g5x3-firmware-library, a verbatim
 * mirror of GD's v1.5.0 release).
 *
 * Status (this commit):
 *   The file exists and compiles against the GigaDevice library so
 *   the `BRIDGE_HAL_BACKEND=gd32` build path is exercisable end-to-
 *   end without any peripheral I/O.  Every hook below is a STUB
 *   returning BRIDGE_HW_ERR_NOTIMPL -- identical wire behaviour to
 *   the bridge_hw_stub.c default backend, but routed through this
 *   file when `gd32` is selected so subsequent commits can replace
 *   stubs with real bodies one peripheral at a time.
 *
 * Implementation order (planned, in increasing risk):
 *
 *   1. RESET_REASON          -- read RCU_RSTSCK status flags.
 *   2. GPIO_READ / WRITE     -- pad map mirrors metadata/e1m_modules/
 *                               v2n/gd32-io-mcu-map.tsv.
 *   3. TRNG_READ             -- vendor trng_*(); init in bridge_hw_init().
 *   4. TMU_COMPUTE           -- vendor tmu_*(); CORDIC F32 / Q31 paths.
 *   5. DAC_SET / GET         -- vendor dac_*(); two channels (PA4, PA6).
 *   6. PWM_SET / GET         -- TIMER0 / TIMER7 advanced PWM.
 *   7. PWM_CONFIGURE         -- align mode, dead-time, break input.
 *   8. ADC_READ              -- single-channel polling.
 *   9. ADC_CONFIGURE         -- oversample / sample-cycles / resolution.
 *   10. ADC_STREAM_*         -- DMA0/1 backed continuous acquisition.
 *   11. QENC_READ / RESET    -- TIMER encoder mode.
 *   12. COUNTER_READ         -- SysTick or hi-res timer.
 *   13. PWM_CAPTURE_*        -- TIMERx input-capture + ring buffer.
 *   14. PWM_SINGLE_PULSE     -- TIMERx one-pulse mode (OPM).
 *   15. TIMER_SYNC           -- master-slave SMC config.
 *   16. POWER_MODE_SET       -- PMU sleep / deep-sleep / standby.
 *   17. DA9292 status poll   -- I2C-master periodic poll cached value.
 *   18. ADC_DSP_*            -- ADC stream chained with FFT/FAC blocks
 *                               for the wave-2 DSP pipeline.
 *
 * Each follow-up commit replaces ONE hook's stub body with a real
 * implementation and updates this header comment + the CHANGELOG.
 * The HIL turn-on cadence is determined by maintainer access to the
 * V2N EVK; the structural skeleton landing today lets the rest of the
 * tree (host-side ZTESTs, the alp_*_* portable surfaces, the
 * docs/test-plan rows) gate against the real backend as soon as the
 * first hook flips from stub to real.
 *
 * Build assumptions:
 *   - arm-none-eabi-gcc on PATH (toolchain file
 *     gd32-bridge/toolchain/arm-none-eabi.cmake handles the rest).
 *   - vendors/gd32_firmware_library/upstream/ submodule initialised
 *     (`git submodule update --init --recursive` from the repo root).
 *   - Cortex-M33 + Thumb + soft-float ABI (matches the GigaDevice
 *     library's compile flags).
 */

#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"

/* The GigaDevice library headers are available via the wrapper's
 * PUBLIC include directories.  Including the device header here --
 * even when nothing below references its symbols yet -- gives us a
 * compile-time check that the submodule is in place and the include
 * path resolves.  Subsequent commits adding real hook bodies will
 * additionally include the matching peripheral header (e.g.
 * gd32g5x3_trng.h, gd32g5x3_tmu.h, gd32g5x3_gpio.h, ...) and
 * gd32-bridge will gain its own per-project libopt.h to pin which
 * standard-peripheral driver units actually link. */
#include "gd32g5x3.h"

/* ----------------------------------------------------------------- */
/* Boot hooks (overrides of the weak defaults in src/main.c)         */
/* ----------------------------------------------------------------- */

/* Called once on entry to main() before the transport ISRs come
 * online.  Future real implementation will:
 *   - rcu_periph_clock_enable() for every peripheral the bridge uses
 *     (GPIOA..GPIOE, TRNG, TMU, ADC0..ADC3, DAC, TIMER0/7/19, etc.).
 *   - gpio_af_set() / gpio_mode_set() per the pad map mirrored from
 *     metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv.
 *   - Configure SysTick at the firmware tick rate.
 *   - Seed the DA9292 status cache by issuing the first I2C poll. */
void bridge_hw_init(void)
{
    /* No-op today; subsequent commits replace per peripheral. */
}

/* Called from the SysTick handler (or the main loop's idle path)
 * on a fixed cadence.  Future real implementation will re-poll the
 * DA9292's PMC_STATUS_00 over I2C-master and update the cached
 * byte returned by bridge_hw_da9292_status_cached(). */
void bridge_hw_tick(void)
{
    /* No-op today. */
}

/* ----------------------------------------------------------------- */
/* Stub bodies -- shape mirrors bridge_hw_stub.c so the gd32 backend  */
/* is binary-compatible with the stub backend pending real impls.    */
/* ----------------------------------------------------------------- */

uint8_t bridge_hw_reset_reason(void)
{
    /* TODO #1: read RCU_RSTSCK; map PORRSTF / NRSTRSTF / SWRSTF /
     * FWDGTRSTF / WWDGTRSTF / LPRSTF + clear via RSTFC. */
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
    (void)mask;
    (void)levels;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_set(uint8_t channel, uint32_t period_ns, uint32_t duty_ns)
{
    (void)channel;
    (void)period_ns;
    (void)duty_ns;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_get(uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns)
{
    (void)channel;
    if (period_ns != 0) *period_ns = 0u;
    if (duty_ns != 0) *duty_ns = 0u;
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

int bridge_hw_pwm_configure(uint8_t channel, uint8_t align_mode, uint32_t dead_time_ns,
                            uint8_t break_cfg)
{
    (void)channel;
    (void)align_mode;
    (void)dead_time_ns;
    (void)break_cfg;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_configure(uint8_t channel, uint16_t oversample_ratio, uint16_t sample_cycles,
                            uint8_t resolution_bits)
{
    (void)channel;
    (void)oversample_ratio;
    (void)sample_cycles;
    (void)resolution_bits;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_stream_begin(uint8_t stream_id, uint8_t channel, uint32_t sample_rate_hz)
{
    (void)stream_id;
    (void)channel;
    (void)sample_rate_hz;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_stream_read(uint8_t stream_id, uint8_t max_samples, uint8_t *got_samples,
                              uint16_t *mv)
{
    (void)stream_id;
    (void)max_samples;
    (void)mv;
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
        for (size_t i = 0; i < len; ++i)
            dest[i] = 0u;
    }
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_tmu_compute(uint8_t function, uint8_t format, uint32_t in_a, uint32_t in_b,
                          uint32_t *result_out)
{
    (void)function;
    (void)format;
    (void)in_a;
    (void)in_b;
    if (result_out != 0) *result_out = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_dac_set(uint8_t channel, uint16_t value_mv)
{
    (void)channel;
    (void)value_mv;
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

/* ----------------------------------------------------------------- */
/* v0.5 (§2B.2) -- advanced timer extras                             */
/* ----------------------------------------------------------------- */

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

/* ----------------------------------------------------------------- */
/* v0.5 (§2B.3) -- system power-mode set                             */
/* ----------------------------------------------------------------- */

int bridge_hw_power_mode_set(uint8_t mode, uint32_t wake_bitmap, uint32_t wake_after_ms)
{
    (void)mode;
    (void)wake_bitmap;
    (void)wake_after_ms;
    return BRIDGE_HW_ERR_NOTIMPL;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                     */
/* ----------------------------------------------------------------- */

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
