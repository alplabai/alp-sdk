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
 *   3. TRNG_READ             -- DONE: NIST SP800-90B mode init in
 *                               bridge_hw_init, DRDY-polled byte read
 *                               with bounded timeout.
 *   4. TMU_COMPUTE           -- DONE: 9 of 12 host functions via TMU
 *                               (tan / exp / tanh NOSUPPORT -- host
 *                               wrapper can libm-fallback in a future
 *                               commit).  F32 + Q31 paths.
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
    { GPIOB, GPIO_PIN_10 }, /* bit  0 = E1M IO8  */
    { GPIOA, GPIO_PIN_7 },  /* bit  1 = E1M IO9  */
    { GPIOA, GPIO_PIN_12 }, /* bit  2 = E1M IO10 */
    { GPIOB, GPIO_PIN_0 },  /* bit  3 = E1M IO11 */
    { GPIOC, GPIO_PIN_1 },  /* bit  4 = E1M IO12 */
    { GPIOF, GPIO_PIN_1 },  /* bit  5 = E1M IO13 */
    { GPIOB, GPIO_PIN_7 },  /* bit  6 = E1M IO14 */
    { GPIOC, GPIO_PIN_0 },  /* bit  7 = E1M IO16 */
    { GPIOC, GPIO_PIN_14 }, /* bit  8 = E1M IO24 */
    { GPIOC, GPIO_PIN_15 }, /* bit  9 = E1M IO25 */
    { GPIOB, GPIO_PIN_11 }, /* bit 10 = E1M IO27 */
    { GPIOC, GPIO_PIN_2 },  /* bit 11 = E1M IO28 */
    { GPIOD, GPIO_PIN_11 }, /* bit 12 = E1M IO29 */
    { GPIOD, GPIO_PIN_10 }, /* bit 13 = E1M IO30 */
    { GPIOE, GPIO_PIN_12 }, /* bit 14 = E1M IO31 */
    { GPIOD, GPIO_PIN_2 },  /* bit 15 = E1M IO32 */
    { GPIOD, GPIO_PIN_8 },  /* bit 16 = E1M IO34 */
    { GPIOD, GPIO_PIN_1 },  /* bit 17 = E1M IO35 */
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
/* TRNG (NIST SP800-90B) state + bring-up.                            */
/* ----------------------------------------------------------------- */

/* Set true once bridge_hw_init() has confirmed the TRNG is producing
 * data and its hardware self-checks are clean.  bridge_hw_trng_read()
 * short-circuits to BRIDGE_HW_ERR_IO when this is false -- e.g. PLL
 * never stabilised, or the TRNG's analog noise source self-check
 * tripped CECS / SECS during bring-up. */
static bool trng_ready = false;

/* Coarse timeout for the PLL stable + TRNG DRDY polls.  Roughly
 * 100k iterations of `trng_flag_get` -- about half a millisecond
 * at the GD32G553's 240 MHz core clock when the TRNG is healthy
 * (DRDY trips in dozens of cycles, so the timeout is the abort
 * latch, not the typical-case bound). */
#define TRNG_INIT_TIMEOUT 100000u
#define TRNG_READY_TIMEOUT 65535u

/* One-time TRNG bring-up.  Configuration mirrors the vendor's
 * `TRNG_NIST_mode` example: PLL Q / 2 clock source, SHA-256
 * conditioning over a 440-bit input window, 256-bit output stage.
 * Returns true iff the TRNG produced its first DRDY without
 * tripping the clock-error (CECS) / seed-error (SECS) flags --
 * either of those fault states leaves the unit unable to deliver
 * randomness, and we surface that via `trng_ready = false`. */
static bool trng_bringup(void)
{
    uint32_t to;

    /* Vendor's `SystemInit()` (called from Reset_Handler before
     * main()) boots the PLL.  In normal operation PLLSTB is set by
     * the time we get here; bound the wait so a misconfigured
     * clock tree doesn't hang `bridge_hw_init`. */
    for (to = TRNG_INIT_TIMEOUT; to != 0u; --to) {
        if (SET == rcu_flag_get(RCU_FLAG_PLLSTB)) break;
    }
    if (to == 0u) return false;

    rcu_trng_clock_config(RCU_TRNG_CKPLLQ_DIV2);
    rcu_periph_clock_enable(RCU_TRNG);

    /* Self-tests on the analog noise source.  trng_clockerror_detection
     * arms the CECS flag so we'd see a clock outage during runtime. */
    trng_clockerror_detection_enable();

    trng_deinit();
    trng_conditioning_reset_enable();
    trng_mode_config(TRNG_MODSEL_NIST);
    trng_nist_seed_config(TRNG_NIST_SEED_ANALOG);
    trng_conditioning_input_bitwidth(TRNG_INMOD_440BIT);
    trng_conditioning_output_bitwidth(TRNG_OUTMOD_256BIT);
    trng_conditioning_algo_config(TRNG_ALGO_SHA256);
    trng_conditioning_enable();
    trng_postprocessing_enable();
    trng_enable();

    /* Wait for the first DRDY -- proves the noise source + post-
     * processing pipeline are alive.  Bounded -- if the analog
     * source is dead the unit will never trip DRDY. */
    for (to = TRNG_READY_TIMEOUT; to != 0u; --to) {
        if (SET == trng_flag_get(TRNG_FLAG_DRDY)) break;
    }
    if (to == 0u) return false;
    if (SET == trng_flag_get(TRNG_FLAG_CECS)) return false;
    if (SET == trng_flag_get(TRNG_FLAG_SECS)) return false;
    return true;
}

/* ----------------------------------------------------------------- */
/* TMU (CORDIC math accelerator) dispatch.                            */
/* ----------------------------------------------------------------- */

/* Per-host-function -> vendor TMU mode mapping.  Indexed by the wire
 * `function` byte (gd32g553_tmu_function_t in
 * `include/alp/chips/gd32g553.h`).  `mode = 0` is the NOSUPPORT
 * sentinel for the three host functions the GD32G5 TMU doesn't
 * natively support (tan / exp / tanh) -- the host wrapper can later
 * compose them via libm or via two TMU calls (tan = sin/cos,
 * tanh = sinh/cosh, exp = cosh + sinh) without protocol changes. */
typedef struct {
    uint32_t mode;   /* TMU_MODE_* (zero = NOSUPPORT marker)             */
    uint8_t  inputs; /* 1 or 2 inputs (zero matches NOSUPPORT entries)   */
} tmu_dispatch_t;

static const tmu_dispatch_t tmu_dispatch[] = {
    [0]  = { TMU_MODE_SIN, 2 },     /* sin(theta) -- HW computes m*sin(theta) */
    [1]  = { TMU_MODE_COS, 2 },     /* cos(theta) -- ditto                    */
    [2]  = { 0, 0 },                /* tan       -- NOSUPPORT                 */
    [3]  = { TMU_MODE_ATAN, 1 },    /* atan(x)                                */
    [4]  = { TMU_MODE_ATAN2, 2 },   /* atan2(y, x)                            */
    [5]  = { TMU_MODE_SQRT, 1 },    /* sqrt(x)                                */
    [6]  = { TMU_MODE_LN, 1 },      /* log = ln(x)                            */
    [7]  = { 0, 0 },                /* exp       -- NOSUPPORT                 */
    [8]  = { TMU_MODE_SINH, 1 },    /* sinh(x)                                */
    [9]  = { TMU_MODE_COSH, 1 },    /* cosh(x)                                */
    [10] = { 0, 0 },                /* tanh      -- NOSUPPORT                 */
    [11] = { TMU_MODE_MODULUS, 2 }, /* hypot = sqrt(x^2 + y^2)                */
};
#define TMU_DISPATCH_COUNT (sizeof(tmu_dispatch) / sizeof(tmu_dispatch[0]))

/* Timeout for the TMU END flag poll.  TMU operations complete in
 * tens of cycles at 240 MHz; the bound is the abort latch for a
 * misconfigured op rather than the typical-case wait. */
#define TMU_COMPUTE_TIMEOUT 65535u

/* Strict-aliasing-safe float<->u32 bit reinterpret helpers.  GCC
 * inlines these to bitcast instructions with -O1+. */
static float bits_to_f32(uint32_t b)
{
    float f;
    __builtin_memcpy(&f, &b, sizeof f);
    return f;
}
static uint32_t f32_to_bits(float f)
{
    uint32_t b;
    __builtin_memcpy(&b, &f, sizeof b);
    return b;
}

/* ----------------------------------------------------------------- */
/* Boot hooks (overrides of the weak defaults in src/main.c)         */
/* ----------------------------------------------------------------- */

/* Called once on entry to main() before the transport ISRs come
 * online.  Future commits also wire up the remaining peripherals
 * the bridge uses (TMU, ADC0..ADC3, DAC, TIMER0/7/19, the DA9292
 * I2C-master poll, SysTick, etc.). */
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
        gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP,
                      gpio_pad_map[i].pin);
        gpio_is_output[i] = false;
    }

    /* TRNG bring-up.  Failure (PLL never stable, analog noise
     * source bad, self-check tripped) leaves trng_ready = false;
     * bridge_hw_trng_read returns BRIDGE_HW_ERR_IO in that case
     * rather than serving zero-filled "randomness". */
    trng_ready = trng_bringup();

    /* TMU clock enable.  Per-op configuration happens in
     * bridge_hw_tmu_compute because the mode + I/O width vary per
     * call. */
    rcu_periph_clock_enable(RCU_TMU);
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
        const FlagStatus s = gpio_is_output[i]
                                 ? gpio_output_bit_get(gpio_pad_map[i].periph, gpio_pad_map[i].pin)
                                 : gpio_input_bit_get(gpio_pad_map[i].periph, gpio_pad_map[i].pin);
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
            gpio_output_options_set(gpio_pad_map[i].periph, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ,
                                    gpio_pad_map[i].pin);
            gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                          gpio_pad_map[i].pin);
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
    if (dest == 0) return BRIDGE_HW_ERR_INVAL;
    if (len == 0u || len > 32u) return BRIDGE_HW_ERR_RANGE;
    if (!trng_ready) return BRIDGE_HW_ERR_IO;

    /* Pull 32-bit randoms and pack their LSB bytes into `dest`.  A
     * single trng_get_true_random_data() call drains one entry from
     * the TRNG's output FIFO and the unit refills autonomously; we
     * poll DRDY between pulls so a starved-noise condition doesn't
     * silently emit a stale word. */
    size_t off = 0u;
    while (off < len) {
        uint32_t to;
        for (to = TRNG_READY_TIMEOUT; to != 0u; --to) {
            if (SET == trng_flag_get(TRNG_FLAG_DRDY)) break;
        }
        if (to == 0u) return BRIDGE_HW_ERR_IO;
        if (SET == trng_flag_get(TRNG_FLAG_CECS)) return BRIDGE_HW_ERR_IO;
        if (SET == trng_flag_get(TRNG_FLAG_SECS)) return BRIDGE_HW_ERR_IO;

        uint32_t     word  = trng_get_true_random_data();
        const size_t chunk = (len - off >= 4u) ? 4u : (len - off);
        for (size_t i = 0; i < chunk; ++i) {
            dest[off++] = (uint8_t)(word & 0xFFu);
            word >>= 8;
        }
    }
    return BRIDGE_HW_OK;
}

int bridge_hw_tmu_compute(uint8_t function, uint8_t format, uint32_t in_a, uint32_t in_b,
                          uint32_t *result_out)
{
    if (result_out == 0) return BRIDGE_HW_ERR_INVAL;
    *result_out = 0u;
    if (function >= TMU_DISPATCH_COUNT) return BRIDGE_HW_ERR_INVAL;
    if (format > 1u) return BRIDGE_HW_ERR_INVAL;

    const tmu_dispatch_t *d = &tmu_dispatch[function];
    if (d->inputs == 0u) {
        /* tan / exp / tanh: GD32G5 TMU has no native mode.  Host
         * wrapper sees STATUS_NOSUPPORT and (eventually) falls back
         * to libm or composes via two TMU calls. */
        return BRIDGE_HW_ERR_NOTIMPL;
    }

    /* Configure TMU for this op.  Per-call config keeps the dispatch
     * stateless -- a previous SIN call doesn't leave the TMU in a
     * state that surprises a subsequent SQRT call. */
    tmu_parameter_struct cfg;
    tmu_struct_para_init(&cfg);
    tmu_deinit();
    cfg.mode            = d->mode;
    cfg.scale           = TMU_SCALING_FACTOR_1;
    cfg.dma_read        = TMU_READ_DMA_DISABLE;
    cfg.dma_write       = TMU_WRITE_DMA_DISABLE;
    cfg.input_width     = TMU_INPUT_WIDTH_32;
    cfg.output_width    = TMU_OUTPUT_WIDTH_32;
    cfg.input_floating  = (format == 1u) ? TMU_INPUT_FLOAT_ENABLE : TMU_INPUT_FLOAT_DISABLE;
    cfg.output_floating = (format == 1u) ? TMU_OUTPUT_FLOAT_ENABLE : TMU_OUTPUT_FLOAT_DISABLE;
    cfg.write_times     = (d->inputs == 2u) ? TMU_WRITE_TIMES_2 : TMU_WRITE_TIMES_1;
    /* SIN/COS emit two output words (the scaled result + an auxiliary
     * scaling factor); every other mode emits one.  We always return
     * the first word; reads are sized accordingly so the TMU END flag
     * clears cleanly. */
    const bool sin_or_cos = (d->mode == TMU_MODE_SIN) || (d->mode == TMU_MODE_COS);
    cfg.read_times        = sin_or_cos ? TMU_READ_TIMES_2 : TMU_READ_TIMES_1;
    tmu_init(&cfg);

    /* Issue the op.  For SIN/COS the host's `in_b` is unused (host
     * wrapper passes 0); the TMU's `m*sin(theta)` form needs a
     * non-zero modulus, so substitute unit-scale (1.0 in the active
     * format).  For native 2-input modes (ATAN2, MODULUS) the host's
     * in_b is the actual second operand. */
    uint32_t b = in_b;
    if (sin_or_cos) {
        b = (format == 1u) ? f32_to_bits(1.0f) /* IEEE-754 */
                           : 0x7FFFFFFFu;      /* Q31 ~1.0  */
    }
    if (d->inputs == 2u) {
        if (format == 1u) {
            tmu_two_f32_write(bits_to_f32(in_a), bits_to_f32(b));
        } else {
            tmu_two_q31_write(in_a, b);
        }
    } else {
        if (format == 1u) {
            tmu_one_f32_write(bits_to_f32(in_a));
        } else {
            tmu_one_q31_write(in_a);
        }
    }

    /* Wait for END.  Bounded so a misconfigured op doesn't hang. */
    uint32_t to = TMU_COMPUTE_TIMEOUT;
    while ((RESET == tmu_flag_get(TMU_FLAG_END)) && --to) {
        /* spin */
    }
    if (to == 0u) return BRIDGE_HW_ERR_IO;
    if (SET == tmu_flag_get(TMU_FLAG_OVRF)) {
        tmu_flag_clear(TMU_FLAG_OVRF);
        return BRIDGE_HW_ERR_RANGE;
    }

    /* Drain output(s).  For SIN/COS the second word is the aux
     * scaling factor; we discard it.  Single-output modes need
     * only one read. */
    if (sin_or_cos) {
        if (format == 1u) {
            float    fr;
            float    faux;
            tmu_two_f32_read(&fr, &faux);
            *result_out = f32_to_bits(fr);
        } else {
            uint32_t aux;
            tmu_two_q31_read(result_out, &aux);
        }
    } else {
        if (format == 1u) {
            float fr;
            tmu_one_f32_read(&fr);
            *result_out = f32_to_bits(fr);
        } else {
            tmu_one_q31_read(result_out);
        }
    }
    return BRIDGE_HW_OK;
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
