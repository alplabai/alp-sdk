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
 *   1. RESET_REASON          -- DONE: RCU_RSTSCK decode + RSTFC clear.
 *   2. GPIO_READ / WRITE     -- DONE: 18-pad map (E1M IO8..IO35),
 *                               boot configures all as INPUT + PULL_UP,
 *                               write auto-promotes to OUTPUT push-pull.
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

#include <stdbool.h>
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
/* GPIO pad map -- E1M IO logical-index to GD32 (port, pin) lookup.   */
/* Sourced from `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`        */
/* (the "E1M IO*" rows).  Wire-side `mask` bit i selects entry i in   */
/* this table; numbering is compact (0..17) rather than matching the  */
/* physical E1M IO numbering, which has gaps at 15 / 17..23 / 26 / 33 */
/* because those positions are assigned to other peripherals on the   */
/* carrier.  Host-side translation table lives in                     */
/* `chips/gd32g553/gd32g553.c`.                                       */
/* ----------------------------------------------------------------- */

typedef struct {
    uint32_t periph; /* GPIOA..GPIOF base address. */
    uint32_t pin;    /* GPIO_PIN_n bit mask.       */
} gd32_gpio_pad_t;

static const gd32_gpio_pad_t gpio_pad_map[] = {
    {GPIOB, GPIO_PIN_10}, /* bit  0 = E1M IO8  */
    {GPIOA, GPIO_PIN_7},  /* bit  1 = E1M IO9  */
    {GPIOA, GPIO_PIN_12}, /* bit  2 = E1M IO10 */
    {GPIOB, GPIO_PIN_0},  /* bit  3 = E1M IO11 */
    {GPIOC, GPIO_PIN_1},  /* bit  4 = E1M IO12 */
    {GPIOF, GPIO_PIN_1},  /* bit  5 = E1M IO13 */
    {GPIOB, GPIO_PIN_7},  /* bit  6 = E1M IO14 */
    {GPIOC, GPIO_PIN_0},  /* bit  7 = E1M IO16 */
    {GPIOC, GPIO_PIN_14}, /* bit  8 = E1M IO24 */
    {GPIOC, GPIO_PIN_15}, /* bit  9 = E1M IO25 */
    {GPIOB, GPIO_PIN_11}, /* bit 10 = E1M IO27 */
    {GPIOC, GPIO_PIN_2},  /* bit 11 = E1M IO28 */
    {GPIOD, GPIO_PIN_11}, /* bit 12 = E1M IO29 */
    {GPIOD, GPIO_PIN_10}, /* bit 13 = E1M IO30 */
    {GPIOE, GPIO_PIN_12}, /* bit 14 = E1M IO31 */
    {GPIOD, GPIO_PIN_2},  /* bit 15 = E1M IO32 */
    {GPIOD, GPIO_PIN_8},  /* bit 16 = E1M IO34 */
    {GPIOD, GPIO_PIN_1},  /* bit 17 = E1M IO35 */
};
#define GPIO_PAD_MAP_COUNT (sizeof(gpio_pad_map) / sizeof(gpio_pad_map[0]))

/* Per-pad direction tracking.  Boot configures every pad as INPUT +
 * PULL_UP; bridge_hw_gpio_write() flips an entry to OUTPUT push-pull
 * on first call (sticky until the next chip reset).  Avoids the
 * need for a separate `CMD_GPIO_CONFIGURE` opcode -- read-only
 * callers see the external level until they touch the pad with a
 * write, after which subsequent reads return the driven level. */
static bool gpio_is_output[GPIO_PAD_MAP_COUNT];

/* ----------------------------------------------------------------- */
/* Boot hooks (overrides of the weak defaults in src/main.c)         */
/* ----------------------------------------------------------------- */

/* Called once on entry to main() before the transport ISRs come
 * online.  Future commits also wire up the remaining peripherals
 * the bridge uses (TRNG, TMU, ADC0..ADC3, DAC, TIMER0/7/19, the
 * DA9292 I2C-master poll, SysTick, etc.). */
void bridge_hw_init(void)
{
    /* Enable AHB2 clocks for every GPIO port the pad map references.
     * The chip's RCU keeps unused GPIO ports clock-gated to save
     * power; we enable A..F unconditionally because the E1M IO map
     * spans those ports.  Port G isn't referenced by any pad. */
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_GPIOF);

    /* Configure every entry in `gpio_pad_map` as INPUT + PULL_UP.
     * Safe default per the GPIO direction policy: no driven
     * contention with whatever the carrier might pull / drive on
     * those pads.  bridge_hw_gpio_write() promotes individual
     * pads to OUTPUT on demand. */
    for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
        gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_INPUT,
                      GPIO_PUPD_PULLUP, gpio_pad_map[i].pin);
        gpio_is_output[i] = false;
    }
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
    /* Read RCU_RSTSCK (reset/clock control status register, GD32G5xx
     * Reference Manual §6.6.13) and decode the sticky reset-cause
     * flags in the high byte: PORRSTF (bit 27), BORRSTF (25),
     * EPRSTF (26, NRST pin), SWRSTF (28), FWDGTRSTF (29),
     * WWDGTRSTF (30), LPRSTF (31).
     *
     * The hardware can latch multiple flags across nested resets, so
     * we decode in coldest-first priority order: a power-on event
     * dominates a brownout, which dominates an external-pin reset,
     * which dominates a watchdog or software trigger.  Encoded byte
     * matches the host's `gd32g553_reset_cause_t` in
     * <alp/chips/gd32g553.h>:
     *
     *   0 = UNKNOWN, 1 = POWER_ON, 2 = NRST_PIN, 3 = SOFT,
     *   4 = WDT, 5 = BROWNOUT, 6 = LOWPOWER.
     *
     * RSTFC (bit 24) clears every cause flag in one write; the vendor
     * helper `rcu_all_reset_flag_clear()` is functionally identical
     * but we keep the access inline to avoid pulling rcu.c stages we
     * don't otherwise need.  After the write the next reader sees
     * UNKNOWN unless something resets the chip again. */
    const uint32_t rstsck = RCU_RSTSCK;
    uint8_t        cause  = 0u; /* UNKNOWN */

    if (rstsck & RCU_RSTSCK_PORRSTF) {
        cause = 1u; /* POWER_ON */
    } else if (rstsck & RCU_RSTSCK_BORRSTF) {
        cause = 5u; /* BROWNOUT */
    } else if (rstsck & RCU_RSTSCK_EPRSTF) {
        cause = 2u; /* NRST_PIN */
    } else if (rstsck & RCU_RSTSCK_LPRSTF) {
        cause = 6u; /* LOWPOWER */
    } else if (rstsck & (RCU_RSTSCK_FWDGTRSTF | RCU_RSTSCK_WWDGTRSTF)) {
        cause = 4u; /* WDT */
    } else if (rstsck & RCU_RSTSCK_SWRSTF) {
        cause = 3u; /* SOFT */
    }

    RCU_RSTSCK |= RCU_RSTSCK_RSTFC;
    return cause;
}

int bridge_hw_gpio_read(uint32_t mask, uint32_t *levels)
{
    if (levels == 0) return BRIDGE_HW_ERR_INVAL;
    *levels = 0u;
    /* Bits above `GPIO_PAD_MAP_COUNT` are silently ignored -- the
     * host header documents the mapping as opaque, so out-of-range
     * bits are treated as "no pad selected" rather than an error. */
    for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
        if ((mask & ((uint32_t)1u << i)) == 0u) continue;
        const FlagStatus s =
            gpio_is_output[i]
                ? gpio_output_bit_get(gpio_pad_map[i].periph,
                                      gpio_pad_map[i].pin)
                : gpio_input_bit_get(gpio_pad_map[i].periph,
                                     gpio_pad_map[i].pin);
        if (s == SET) {
            *levels |= ((uint32_t)1u << i);
        }
    }
    return BRIDGE_HW_OK;
}

int bridge_hw_gpio_write(uint32_t mask, uint32_t levels)
{
    /* Out-of-range bits silently ignored, same policy as
     * bridge_hw_gpio_read(). */
    for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
        if ((mask & ((uint32_t)1u << i)) == 0u) continue;
        if (!gpio_is_output[i]) {
            /* First write to this pad since boot: promote
             * INPUT+PULL_UP to OUTPUT push-pull.  12 MHz is the
             * GD32G5's slowest output speed (datasheet §7.4.1);
             * adequate for control lines, low EMI.  The bridge
             * dispatcher is single-threaded so no locking is
             * needed around the mode flip + the flag write. */
            gpio_output_options_set(gpio_pad_map[i].periph,
                                    GPIO_OTYPE_PP,
                                    GPIO_OSPEED_12MHZ,
                                    gpio_pad_map[i].pin);
            gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_OUTPUT,
                          GPIO_PUPD_NONE, gpio_pad_map[i].pin);
            gpio_is_output[i] = true;
        }
        if (levels & ((uint32_t)1u << i)) {
            gpio_bit_set(gpio_pad_map[i].periph, gpio_pad_map[i].pin);
        } else {
            gpio_bit_reset(gpio_pad_map[i].periph, gpio_pad_map[i].pin);
        }
    }
    return BRIDGE_HW_OK;
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
