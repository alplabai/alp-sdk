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
 *   5. DAC_SET / GET         -- DONE: channel 0 -> DAC0_OUT0 / PA4,
 *                               channel 1 -> DAC1_OUT0 / PA6, mV<->12-bit
 *                               code at VREF=1800mV (V2N analog supply).
 *   6. PWM_SET / GET         -- DONE: 8 channels across TIMER0 + TIMER7
 *                               at 1 us LSB; pin AFs per datasheet Rev2.0.
 *   7. PWM_CONFIGURE         -- align mode, dead-time, break input.
 *   8. ADC_READ              -- DONE: 8-pad map across ADC0..3, single-
 *                               shot polling, mV<->code at VREF=1800mV.
 *   9. ADC_CONFIGURE         -- PARTIAL: per-channel sample_cycles
 *                               cached + applied; oversample_ratio +
 *                               resolution_bits gated to defaults (1,
 *                               12) until a follow-up commit.
 *   10. ADC_STREAM_*         -- DMA0/1 backed continuous acquisition.
 *   11. QENC_READ / RESET    -- TIMER encoder mode.
 *   12. COUNTER_READ         -- DONE: Cortex-M33 DWT cycle counter
 *                               (32-bit free-running at core clock).
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
/* ADC channels.                                                      */
/* ----------------------------------------------------------------- */

/* E1M ADC0..7 -> (ADC peripheral, channel index, pad).  Sourced from
 * maintainer-confirmed `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`
 * with channel + peripheral assignments cross-checked against the
 * GD32G553xx datasheet pin alt-function summary:
 *
 *   E1M ADC0  PD9   ADC3  CH12
 *   E1M ADC1  PB12  ADC3  CH2
 *   E1M ADC2  PE13  ADC2  CH2
 *   E1M ADC3  PE11  ADC2  CH13
 *   E1M ADC4  PC4   ADC1  CH4
 *   E1M ADC5  PA5   ADC1  CH12
 *   E1M ADC6  PA2   ADC0  CH2
 *   E1M ADC7  PA3   ADC0  CH3
 *
 * All four ADC peripherals carry two of the eight channels; the
 * init loop brings up each ADC once and the read body reconfigures
 * the routine channel + triggers a single conversion on demand. */

typedef struct {
    uint32_t periph;    /* ADC0..ADC3 base                            */
    uint8_t  channel;   /* ADC_CHANNEL_n                              */
    uint32_t gpio_port; /* GPIOA..GPIOF                               */
    uint32_t gpio_pin;  /* GPIO_PIN_n                                 */
} gd32_adc_ch_t;

static const gd32_adc_ch_t adc_channels_map[] = {
    [0] = { ADC3, ADC_CHANNEL_12, GPIOD, GPIO_PIN_9 },
    [1] = { ADC3, ADC_CHANNEL_2, GPIOB, GPIO_PIN_12 },
    [2] = { ADC2, ADC_CHANNEL_2, GPIOE, GPIO_PIN_13 },
    [3] = { ADC2, ADC_CHANNEL_13, GPIOE, GPIO_PIN_11 },
    [4] = { ADC1, ADC_CHANNEL_4, GPIOC, GPIO_PIN_4 },
    [5] = { ADC1, ADC_CHANNEL_12, GPIOA, GPIO_PIN_5 },
    [6] = { ADC0, ADC_CHANNEL_2, GPIOA, GPIO_PIN_2 },
    [7] = { ADC0, ADC_CHANNEL_3, GPIOA, GPIO_PIN_3 },
};
#define ADC_CHANNEL_MAP_COUNT (sizeof(adc_channels_map) / sizeof(adc_channels_map[0]))

/* VREF for the ADC's 12-bit right-aligned code -> millivolt
 * conversion.  V2N's analog supply is 1.8 V (maintainer-confirmed
 * the same rail used by DAC_VREF_MV).  Full-scale is 4095 codes. */
#define ADC_VREF_MV 1800u
#define ADC_FULL_SCALE 4095u

/* Default sample time used for single-shot reads.  240 cycles is
 * the most conservative setting in the vendor's range -- gives the
 * external source plenty of settling time for a high-impedance
 * input divider, at the cost of slower conversion (~1 us per
 * sample at ADC_CLK_SYNC_HCLK_DIV6 with HCLK=240 MHz: 240 ADCCK
 * sample + 12.5 ADCCK conversion ~= 6.3 us). */
#define ADC_DEFAULT_SAMPLE_CYCLES 240u

/* Per-channel sticky sample-cycle override applied by
 * bridge_hw_adc_configure.  Defaults to ADC_DEFAULT_SAMPLE_CYCLES;
 * caller's tighter values (e.g. 24 cycles for low-impedance sources)
 * override on the next bridge_hw_adc_read.  Resolution + oversample
 * are still gated to defaults at v0.3 and live in a follow-up. */
static uint16_t adc_sample_cycles_cache[8];

static void adc_periph_init(uint32_t periph)
{
    adc_deinit(periph);
    adc_clock_config(periph, ADC_CLK_SYNC_HCLK_DIV6);
    adc_data_alignment_config(periph, ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(periph, ADC_ROUTINE_CHANNEL, 1u);
    adc_external_trigger_config(periph, ADC_ROUTINE_CHANNEL, EXTERNAL_TRIGGER_DISABLE);
    adc_enable(periph);
}

/* ----------------------------------------------------------------- */
/* PWM channels (TIMER0 + TIMER7).                                    */
/* ----------------------------------------------------------------- */

/* E1M PWM channel -> GD32 (timer, channel, output kind, pad, AF).
 * Sourced from `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` for the
 * pad column; AF + timer-channel from the GD32G553xx Datasheet Rev2.0
 * Tables 2-10..2-13 (pin alternate-function summary).
 *
 *   PWM0  PA11  TIMER0_MCH0 (complement.) AF6
 *   PWM1  PB1   TIMER0_MCH2 (complement.) AF6
 *   PWM2  PB14  TIMER0_MCH1 (complement.) AF6
 *   PWM3  PC5   TIMER0_MCH3 (complement.) AF6
 *   PWM4  PC10  TIMER7_MCH0               AF4
 *   PWM5  PC11  TIMER7_MCH1               AF4
 *   PWM6  PC12  TIMER7_MCH2               AF4
 *   PWM7  PD0   TIMER7_MCH3               AF6
 *
 * Every PWM rides a distinct TIMER channel's complementary output,
 * so per-channel duty cycles are fully independent.  Each channel's
 * main output (CHx) is unused on V2N -- only the complementary
 * (CHxN) pad sits on the E1M PWM connector.
 *
 * Periods are SHARED across all PWMs of the same timer (TIMER0:
 * PWM0..3; TIMER7: PWM4..7) because each TIMER has one ARR.  The
 * per-channel `bridge_hw_pwm_set` body updates the timer's ARR every
 * call -- last write wins.  In typical V2N use the host sets the
 * same period across each group so this doesn't surface. */

typedef struct {
    uint32_t periph;     /* TIMER0 or TIMER7 base                          */
    uint16_t channel;    /* TIMER_CH_0..TIMER_CH_3                          */
    bool     complement; /* true: drive complementary output, false: main */
    uint32_t gpio_port;  /* GPIOA..GPIOF                                    */
    uint32_t gpio_pin;   /* GPIO_PIN_n                                     */
    uint32_t gpio_af;    /* GPIO_AF_X                                      */
} gd32_pwm_ch_t;

static const gd32_pwm_ch_t pwm_channels[] = {
    [0] = { TIMER0, TIMER_CH_0, true, GPIOA, GPIO_PIN_11, GPIO_AF_6 },
    [1] = { TIMER0, TIMER_CH_2, true, GPIOB, GPIO_PIN_1, GPIO_AF_6 },
    [2] = { TIMER0, TIMER_CH_1, true, GPIOB, GPIO_PIN_14, GPIO_AF_6 },
    [3] = { TIMER0, TIMER_CH_3, true, GPIOC, GPIO_PIN_5, GPIO_AF_6 },
    [4] = { TIMER7, TIMER_CH_0, true, GPIOC, GPIO_PIN_10, GPIO_AF_4 },
    [5] = { TIMER7, TIMER_CH_1, true, GPIOC, GPIO_PIN_11, GPIO_AF_4 },
    [6] = { TIMER7, TIMER_CH_2, true, GPIOC, GPIO_PIN_12, GPIO_AF_4 },
    [7] = { TIMER7, TIMER_CH_3, true, GPIOD, GPIO_PIN_0, GPIO_AF_6 },
};
#define PWM_CHANNEL_COUNT (sizeof(pwm_channels) / sizeof(pwm_channels[0]))

/* TIMER core clock.  GD32G553's stock SystemInit() clocks the
 * advanced timers from the APB-derived TIMER clock; on the chip's
 * default 240 MHz config that's 240 MHz at the timer counter input.
 * 1 ns LSB resolution would need a 240x faster counter; we instead
 * round period_ns + duty_ns to the nearest 1 us cycle by fixing the
 * prescaler at (240 - 1) so the counter ticks at 1 MHz.  ARR is
 * then `period_us - 1`, fitting in 16 bits for periods up to ~65 ms
 * which covers every realistic control PWM frequency (>=15 Hz). */
#define PWM_TIMER_CLK_HZ 240000000u
#define PWM_TIMER_PRESCALER (240u - 1u) /* 240 MHz -> 1 MHz tick    */
#define PWM_TIMER_TICK_NS 1000u         /* 1 us per timer tick      */
#define PWM_TIMER_ARR_MAX 0xFFFFu       /* 16-bit auto-reload limit */

/* Last-set cache for bridge_hw_pwm_get.  Reading back the timer's
 * compare register would also work but the caller is interested in
 * "what did I ask for", not "what does the rounded-to-1us value
 * round-trip to". */
static uint32_t pwm_period_ns_cache[PWM_CHANNEL_COUNT];
static uint32_t pwm_duty_ns_cache[PWM_CHANNEL_COUNT];

/* Per-timer init.  Called once per peripheral from bridge_hw_init();
 * Advanced timers need timer_primary_output_config(ENABLE) before any
 * output pin actually drives (vs basic timers, where the channel
 * enable is sufficient). */
static void pwm_timer_init(uint32_t periph)
{
    timer_parameter_struct ip;
    timer_struct_para_init(&ip);
    ip.prescaler         = (uint16_t)PWM_TIMER_PRESCALER;
    ip.alignedmode       = TIMER_COUNTER_EDGE;
    ip.counterdirection  = TIMER_COUNTER_UP;
    ip.period            = PWM_TIMER_ARR_MAX; /* 65.5 ms default; per-set */
    ip.clockdivision     = TIMER_CKDIV_DIV1;
    ip.repetitioncounter = 0u;
    timer_deinit(periph);
    timer_init(periph, &ip);
    timer_primary_output_config(periph, ENABLE);
    timer_enable(periph);
}

/* Per-channel init.  Sets PWM mode 0 (output high while counter <
 * compare) and 0 duty -- HW pad sits low until the host issues a
 * bridge_hw_pwm_set with a non-zero duty. */
static void pwm_channel_init(const gd32_pwm_ch_t *ch)
{
    timer_oc_parameter_struct oc;
    timer_channel_output_struct_para_init(&oc);
    if (ch->complement) {
        oc.outputstate  = TIMER_CCX_DISABLE;
        oc.outputnstate = TIMER_CCXN_ENABLE;
    } else {
        oc.outputstate  = TIMER_CCX_ENABLE;
        oc.outputnstate = TIMER_CCXN_DISABLE;
    }
    oc.ocpolarity   = TIMER_OC_POLARITY_HIGH;
    oc.ocnpolarity  = TIMER_OCN_POLARITY_HIGH;
    oc.ocidlestate  = TIMER_OC_IDLE_STATE_LOW;
    oc.ocnidlestate = TIMER_OCN_IDLE_STATE_LOW;
    timer_channel_output_config(ch->periph, ch->channel, &oc);
    timer_channel_output_pulse_value_config(ch->periph, ch->channel, 0u);
    timer_channel_output_mode_config(ch->periph, ch->channel, TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(ch->periph, ch->channel, TIMER_OC_SHADOW_DISABLE);
}

/* ----------------------------------------------------------------- */
/* DAC channels.                                                      */
/* ----------------------------------------------------------------- */

/* E1M DAC0/DAC1 -> physical DAC peripheral + output + pad.  Sourced
 * from `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`; PA4 is the
 * GD32G5x3's stock DAC0_OUT0 alt-function and PA6 is DAC1_OUT0 per
 * the datasheet's pin alt-function table. */
typedef struct {
    uint32_t periph;    /* DAC0..DAC3 base address */
    uint8_t  out;       /* DAC_OUT0 or DAC_OUT1    */
    uint32_t gpio_port; /* GPIOA..GPIOG base addr  */
    uint32_t gpio_pin;  /* GPIO_PIN_n bit mask     */
} gd32_dac_ch_t;

static const gd32_dac_ch_t dac_channels[] = {
    [0] = { DAC0, DAC_OUT0, GPIOA, GPIO_PIN_4 }, /* E1M DAC0 -> PA4 */
    [1] = { DAC1, DAC_OUT0, GPIOA, GPIO_PIN_6 }, /* E1M DAC1 -> PA6 */
};
#define DAC_CHANNEL_COUNT (sizeof(dac_channels) / sizeof(dac_channels[0]))

/* DAC VREF.  The V2N's analog supply is 1.8 V (maintainer-confirmed
 * 2026-05-13 against the schematic).  Revisit if a future hw-revision
 * moves to a buffered VREFINT source or a different rail.  Full-scale
 * code is 4095 for 12-bit alignment; code = (value_mv * 4095) /
 * VREF_mV with overflow clamped. */
#define DAC_VREF_MV    1800u
#define DAC_FULL_SCALE 4095u

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

    /* DAC bring-up.  Two channels per `dac_channels[]`:
     *   ch0 -> DAC0_OUT0 -> PA4
     *   ch1 -> DAC1_OUT0 -> PA6
     * Configure both pads as analog (GPIOA clock was enabled
     * above for the IO pad map), enable each DAC peripheral in
     * NORMAL_PIN_BUFFON mode so the output drives the pad
     * through the chip's built-in buffer. */
    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO_PIN_4 | GPIO_PIN_6);
    rcu_periph_clock_enable(RCU_DAC0);
    rcu_periph_clock_enable(RCU_DAC1);
    for (size_t i = 0; i < DAC_CHANNEL_COUNT; ++i) {
        dac_deinit(dac_channels[i].periph);
        dac_trigger_disable(dac_channels[i].periph, dac_channels[i].out);
        dac_wave_mode_config(dac_channels[i].periph, dac_channels[i].out, DAC_WAVE_DISABLE);
        dac_mode_config(dac_channels[i].periph, dac_channels[i].out, NORMAL_PIN_BUFFON);
        dac_enable(dac_channels[i].periph, dac_channels[i].out);
    }

    /* Free-running counter: enable the Cortex-M33 DWT cycle counter.
     * TRCENA in CoreDebug->DEMCR gates the entire DWT/ITM trace block;
     * setting CYCCNTENA in DWT->CTRL starts the 32-bit free-running
     * counter at the core clock rate (240 MHz on the GD32G553 in the
     * stock clock config -> ~17.9 s wrap, ~4.16 ns LSB).  The counter
     * is the source for bridge_hw_counter_read(). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* PWM bring-up: configure the 8 PWM pads as alt-function outputs,
     * enable TIMER0 + TIMER7 clocks, run the per-timer + per-channel
     * init.  After this the timers are running at 1 us tick with 0%
     * duty on every channel; bridge_hw_pwm_set programs both the
     * channel compare register and the timer ARR per call. */
    for (size_t i = 0; i < PWM_CHANNEL_COUNT; ++i) {
        const gd32_pwm_ch_t *ch = &pwm_channels[i];
        gpio_mode_set(ch->gpio_port, GPIO_MODE_AF, GPIO_PUPD_NONE, ch->gpio_pin);
        gpio_output_options_set(ch->gpio_port, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, ch->gpio_pin);
        gpio_af_set(ch->gpio_port, ch->gpio_af, ch->gpio_pin);
    }
    rcu_periph_clock_enable(RCU_TIMER0);
    rcu_periph_clock_enable(RCU_TIMER7);
    pwm_timer_init(TIMER0);
    pwm_timer_init(TIMER7);
    for (size_t i = 0; i < PWM_CHANNEL_COUNT; ++i) {
        pwm_channel_init(&pwm_channels[i]);
    }

    /* ADC bring-up: configure 8 pads as analog, enable all four ADC
     * peripheral clocks, run the per-peripheral init.  No calibration
     * pass here -- it needs a millisecond settling delay after
     * adc_enable() and the bridge boot has no SysTick yet.  A
     * follow-up commit can add calibration once a delay primitive
     * lands. */
    for (size_t i = 0; i < ADC_CHANNEL_MAP_COUNT; ++i) {
        gpio_mode_set(adc_channels_map[i].gpio_port, GPIO_MODE_ANALOG, GPIO_PUPD_NONE,
                      adc_channels_map[i].gpio_pin);
    }
    rcu_periph_clock_enable(RCU_ADC0);
    rcu_periph_clock_enable(RCU_ADC1);
    rcu_periph_clock_enable(RCU_ADC2);
    rcu_periph_clock_enable(RCU_ADC3);
    adc_periph_init(ADC0);
    adc_periph_init(ADC1);
    adc_periph_init(ADC2);
    adc_periph_init(ADC3);
    for (size_t i = 0; i < ADC_CHANNEL_MAP_COUNT; ++i) {
        adc_sample_cycles_cache[i] = ADC_DEFAULT_SAMPLE_CYCLES;
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
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    if (duty_ns > period_ns) return BRIDGE_HW_ERR_INVAL;

    /* Round period + duty to whole microseconds (the timer tick).
     * `period_us` must fit in 16 bits (ARR) -- caller is responsible
     * for staying under ~65 ms; we clamp on over-range so the timer
     * doesn't get an invalid value. */
    uint32_t period_us = period_ns / PWM_TIMER_TICK_NS;
    uint32_t duty_us   = duty_ns / PWM_TIMER_TICK_NS;
    if (period_us == 0u) return BRIDGE_HW_ERR_RANGE;
    if (period_us > PWM_TIMER_ARR_MAX + 1u) period_us = PWM_TIMER_ARR_MAX + 1u;
    if (duty_us > period_us) duty_us = period_us;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];

    /* ARR is "period_us - 1" because the up-counter counts 0..ARR
     * inclusive (period_us ticks total).  Updates ALL channels of
     * the same timer -- the contract documents this constraint. */
    timer_autoreload_value_config(ch->periph, (uint32_t)(period_us - 1u));
    timer_channel_output_pulse_value_config(ch->periph, ch->channel, duty_us);

    /* Cache the host's request for read-back.  The HW reality is
     * shared-period; the cache keeps per-channel intent. */
    pwm_period_ns_cache[channel] = period_ns;
    pwm_duty_ns_cache[channel]   = duty_ns;
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_get(uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns)
{
    if (period_ns == 0 || duty_ns == 0) return BRIDGE_HW_ERR_INVAL;
    *period_ns = 0u;
    *duty_ns   = 0u;
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    *period_ns = pwm_period_ns_cache[channel];
    *duty_ns   = pwm_duty_ns_cache[channel];
    return BRIDGE_HW_OK;
}

int bridge_hw_adc_read(uint8_t channel, uint8_t samples, uint16_t *mv)
{
    if (mv == 0) return BRIDGE_HW_ERR_INVAL;
    if (samples == 0u) return BRIDGE_HW_ERR_INVAL;
    if (channel >= ADC_CHANNEL_MAP_COUNT) return BRIDGE_HW_ERR_RANGE;

    const gd32_adc_ch_t *ch = &adc_channels_map[channel];

    /* Configure the routine channel for this op (each call re-applies
     * because multiple bridge channels can share an ADC peripheral -- a
     * prior bridge_hw_adc_read on a different bridge channel may have
     * pointed the routine slot elsewhere).  Sample-cycle count comes
     * from the per-channel cache so bridge_hw_adc_configure's choice
     * survives across reads. */
    adc_routine_channel_config(ch->periph, 0u, ch->channel, adc_sample_cycles_cache[channel]);

    /* Take `samples` consecutive conversions, software-triggered per
     * sample.  Polled EOC.  No timeout -- if the ADC is wedged the
     * SysTick watchdog (when it ships) catches it; in the steady-state
     * a 12-bit conversion at HCLK/6 finishes in ~6 us so the spin is
     * brief. */
    for (uint8_t i = 0; i < samples; ++i) {
        adc_software_trigger_enable(ch->periph, ADC_ROUTINE_CHANNEL);
        while (!adc_flag_get(ch->periph, ADC_FLAG_EOC)) {
            /* spin */
        }
        adc_flag_clear(ch->periph, ADC_FLAG_EOC);
        uint32_t code = adc_routine_data_read(ch->periph);
        if (code > ADC_FULL_SCALE) code = ADC_FULL_SCALE;
        mv[i] = (uint16_t)((code * ADC_VREF_MV) / ADC_FULL_SCALE);
    }
    return BRIDGE_HW_OK;
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
    if (channel >= ADC_CHANNEL_MAP_COUNT) return BRIDGE_HW_ERR_RANGE;

    /* v0.3 partial: only sample_cycles is sticky.  The oversample +
     * resolution paths are still gated to their defaults (1, 12).
     * Non-default values return NOSUPPORT so the host gets a clear
     * "this commit doesn't support that yet" rather than silently
     * accepting and ignoring.  A follow-up commit will land the
     * resolution + oversample apply path. */
    if (oversample_ratio != 1u) return BRIDGE_HW_ERR_NOTIMPL;
    if (resolution_bits != 12u) return BRIDGE_HW_ERR_NOTIMPL;

    /* Sample cycles: clamp to the vendor's accepted 2..638 cycle
     * range (per `adc_routine_channel_config`'s sample_time
     * parameter).  Round caller-supplied values into this range
     * rather than rejecting -- matches the contract's "firmware
     * rounds down" wording. */
    uint16_t sc = sample_cycles;
    if (sc < 2u) sc = 2u;
    if (sc > 638u) sc = 638u;

    adc_sample_cycles_cache[channel] = sc;
    return BRIDGE_HW_OK;
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
            float fr;
            float faux;
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
    if (channel >= DAC_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    /* mV -> 12-bit code, clamp on over-range (the host doesn't see
     * BRIDGE_HW_ERR_RANGE for this case -- saturating is friendlier
     * than rejecting a request the user can recover from by reading
     * back the actual programmed value). */
    uint32_t code = ((uint32_t)value_mv * DAC_FULL_SCALE) / DAC_VREF_MV;
    if (code > DAC_FULL_SCALE) code = DAC_FULL_SCALE;
    dac_data_set(dac_channels[channel].periph, dac_channels[channel].out, DAC_ALIGN_12B_R,
                 (uint16_t)code);
    return BRIDGE_HW_OK;
}

int bridge_hw_dac_get(uint8_t channel, uint16_t *value_mv)
{
    if (value_mv == 0) return BRIDGE_HW_ERR_INVAL;
    *value_mv = 0u;
    if (channel >= DAC_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    /* dac_output_value_get reads the DAC's hold register (the value
     * currently driving the pad), not the input setpoint -- this is
     * what we want for read-back: callers see the actual code in
     * play, which may differ from the last `set` if the DAC was
     * concurrent-paired or DMA-driven elsewhere. */
    uint16_t code = dac_output_value_get(dac_channels[channel].periph, dac_channels[channel].out);
    if (code > DAC_FULL_SCALE) code = DAC_FULL_SCALE;
    *value_mv = (uint16_t)(((uint32_t)code * DAC_VREF_MV) / DAC_FULL_SCALE);
    return BRIDGE_HW_OK;
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
    if (ticks == 0) return BRIDGE_HW_ERR_INVAL;
    *ticks = 0u;
    /* Single free-running counter exposed today; future revisions can
     * carve out additional ids for derived (slower) tick bases.  The
     * DWT counter ticks at the core clock (240 MHz on GD32G553),
     * wraps every ~17.9 s, and is monotonically non-decreasing across
     * reads -- the host can compute deltas without watching for
     * mid-read consistency since the register is atomic. */
    if (counter != 0u) return BRIDGE_HW_ERR_RANGE;
    *ticks = DWT->CYCCNT;
    return BRIDGE_HW_OK;
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
