/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 backend for the bridge HAL.  Selected by setting
 * BRIDGE_HAL_BACKEND=gd32 in firmware/gd32-bridge/CMakeLists.txt.  Links
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
 *   7. PWM_CONFIGURE         -- PARTIAL: defaults (edge-aligned, no
 *                               dead-time, no break input) accepted;
 *                               non-defaults NOSUPPORT pending follow-up.
 *   8. ADC_READ              -- DONE: 8-pad map across ADC0..3, single-
 *                               shot polling, mV<->code at VREF=1800mV.
 *   9. ADC_CONFIGURE         -- PARTIAL: per-channel sample_cycles
 *                               cached + applied; oversample_ratio +
 *                               resolution_bits gated to defaults (1,
 *                               12) until a follow-up commit.
 *   10. ADC_STREAM_*         -- DONE (§C.23): DMA0/1-backed
 *                               continuous acquisition.  Two parallel
 *                               streams; each owns a 1024-sample
 *                               circular ring buffer + a DMA channel
 *                               driving it peripheral-to-memory.
 *                               Host drains via polled stream_read;
 *                               write cursor recovered from the DMA
 *                               counter.
 *   11. QENC_READ / RESET    -- DONE: 4 encoders across TIMER1/2/3/4
 *                               in quadrature decoder mode 2 (X4).
 *   12. COUNTER_READ         -- DONE: Cortex-M33 DWT cycle counter
 *                               (32-bit free-running at core clock).
 *   13. PWM_CAPTURE_*        -- DONE (§C.15a): TIMERx input-capture
 *                               with polled drain.  V2N pad map binds
 *                               the COMPLEMENTARY (CHxN) outputs --
 *                               BEGIN reconfigures the channel as
 *                               input-capture but the captured edges
 *                               only land once a future hardware-
 *                               bring-up commit reworks the pad
 *                               routing onto the timer's main CHx
 *                               input mux.  Structural code path
 *                               (config + drain + ns conversion) is
 *                               complete and exercised end-to-end.
 *   14. PWM_SINGLE_PULSE     -- DONE: TIMERx OPM (one-pulse mode).
 *                               Switches the timer's whole SP-bit so
 *                               other channels on the same timer also
 *                               run as single-pulse until a PWM_SET
 *                               flips back to repetitive.
 *   15. TIMER_SYNC           -- DONE (§C.15b): master-slave SMC
 *                               config via timer_slave_mode_select
 *                               + timer_master_output0_trigger_source_select
 *                               + timer_master_slave_mode_config.
 *                               Wire `mode` byte translated to the
 *                               vendor's TIMER_SLAVE_MODE_* /
 *                               TIMER_QUAD_DECODER_MODE* encoding.
 *                               Slave listens to ITI0; SYSCFG router
 *                               left at chip default.
 *   16. POWER_MODE_SET       -- DONE (§C.15c + §C.25): mode 0/1
 *                               (run/sleep) accepted no-ops, mode 2
 *                               (deep-sleep) calls
 *                               pmu_to_deepsleepmode(LDO_LOWPOWER, WFI),
 *                               mode 3 (standby) calls
 *                               pmu_to_standbymode().  Wake-source
 *                               bitmap: GPIO enables PMU_WAKEUP_PIN0..4,
 *                               RTC + TIMER arm the RTC wakeup timer
 *                               at 0.5 ms LSB (IRC32K / DIV16); same
 *                               timer also honours wake_after_ms.
 *                               UART_RX / USB / ETH_LINK reject as
 *                               NOSUPPORT (no HW path on GD32G5).
 *   17. DA9292 fault forward -- 0xFF sentinel UNCONDITIONALLY: this
 *                               SoM rev wires the DA9292 fault nets
 *                               only to the Renesas (P37/P36); the
 *                               GD32 has no connection to them and no
 *                               I2C path to the PMIC.  Pin sampling
 *                               awaits a HW rev that mirrors the nets.
 *   18. ADC_DSP_*            -- DONE (§C.15d + §C.24): chain_open +
 *                               stage_push implemented as a 4-chain
 *                               pool with per-stage chunk reassembly
 *                               into a 260-byte buffer.  chain_bind
 *                               validates completeness + ordering
 *                               rules (FFT terminal, WINDOW preceding
 *                               FFT) and stores the binding on both
 *                               sides; runtime FFT/FAC dispatch inside
 *                               stream_read follows in a later commit.
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
 *     firmware/gd32-bridge/toolchain/arm-none-eabi.cmake handles the rest).
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
/* board.  Host-side translation table lives in                     */
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
    { GPIOB, GPIO_PIN_5 },  /* bit  6 = E1M IO14 */
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

/* TRNG bring-up is INCREMENTAL: `trng_started` records that the unit
 * is configured + enabled; `trng_ready` that the first conditioned
 * word arrived (DRDY) with clean self-checks.  The split exists
 * because the NIST-mode pipeline's first DRDY (analog source startup
 * + 440-bit window fill + SHA-256 stage) can take longer than any
 * wait this firmware is allowed to spin inside a request handler --
 * the original single-shot bring-up waited ~1 ms at boot, timed out
 * on real silicon, latched trng_ready = false FOREVER and every
 * TRNG_READ answered STATUS_IO (HiL soak 2026-06-04: 0-for-437).
 * Now bring-up configures and returns; each read re-polls DRDY with
 * a short bound until conditioning completes, and a tripped
 * CECS/SECS self-check triggers a full reconfigure on the next call
 * instead of bricking the surface for the session. */
static bool trng_started = false;
static bool trng_ready   = false;

/* Coarse timeout for the PLL stable + TRNG DRDY polls.  Roughly
 * 100k iterations of `trng_flag_get` -- about half a millisecond
 * at the GD32G553's 216 MHz core clock when the TRNG is healthy
 * (DRDY trips in dozens of cycles, so the timeout is the abort
 * latch, not the typical-case bound). */
#define TRNG_INIT_TIMEOUT 100000u
#define TRNG_READY_TIMEOUT 65535u

/* Configure + enable the TRNG (no DRDY wait here -- see above).
 * Mirrors the vendor's `TRNG_NIST_mode` example: PLL Q / 2 clock
 * source, SHA-256 conditioning over a 440-bit input window, 256-bit
 * output stage. */
static bool trng_start(void)
{
    uint32_t to;

    /* Vendor's `SystemInit()` (called from Reset_Handler before
     * main()) boots the PLL.  In normal operation PLLSTB is set by
     * the time we get here; bound the wait so a misconfigured
     * clock tree doesn't hang. */
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
    return true;
}

/* TRNG fault detector.  Mirrors the vendor's trng_ready_check: a
 * fault shows in FOUR places, and the two LATCHED interrupt flags are
 * the ones that persist -- a seed error parks the unit with SEIF +
 * STAT.ERRSTA set while the *current*-status SECS reads CLEAR again
 * (silicon 2026-06-04: TRNG_STAT == 0x48, DRDY never returning, and a
 * CECS/SECS-only check looping blind on DRDY until its budget died).
 * Recovery for any of them is the same full deinit + reconfigure. */
static bool trng_faulted(void)
{
    return (SET == trng_interrupt_flag_get(TRNG_INT_FLAG_CEIF)) ||
           (SET == trng_interrupt_flag_get(TRNG_INT_FLAG_SEIF)) ||
           (SET == trng_flag_get(TRNG_FLAG_CECS)) || (SET == trng_flag_get(TRNG_FLAG_SECS));
}

static void trng_demote(void)
{
    trng_deinit(); /* RCU reset clears the latched flags too */
    trng_started = false;
    trng_ready   = false;
}

/* Short, handler-safe DRDY poll: promotes `trng_ready` once the first
 * conditioned word lands.  Faults are checked BEFORE the DRDY wait --
 * in the latched-error state DRDY never comes, and burning the whole
 * poll budget on a dead unit kept the request handler busy long
 * enough to tar-pit unrelated commands. */
static bool trng_poll_ready(void)
{
    uint32_t to;

    if (trng_faulted()) {
        trng_demote();
        return false;
    }
    for (to = TRNG_READY_TIMEOUT; to != 0u; --to) {
        if (SET == trng_flag_get(TRNG_FLAG_DRDY)) break;
    }
    if (to == 0u || trng_faulted()) {
        if (trng_faulted()) trng_demote();
        return false;
    }
    trng_ready = true;
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
 * tens of cycles at 216 MHz; the bound is the abort latch for a
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
 * sample at ADC_CLK_SYNC_HCLK_DIV6 with HCLK=216 MHz: 240 ADCCK
 * sample + 12.5 ADCCK conversion ~= 7.0 us). */
#define ADC_DEFAULT_SAMPLE_CYCLES 240u

/* Per-channel sticky sample-cycle override applied by
 * bridge_hw_adc_configure.  Defaults to ADC_DEFAULT_SAMPLE_CYCLES;
 * caller's tighter values (e.g. 24 cycles for low-impedance sources)
 * override on the next bridge_hw_adc_read.  Resolution + oversample
 * are still gated to defaults at v0.3 and live in a follow-up. */
static uint16_t adc_sample_cycles_cache[8];

static void     adc_periph_init(uint32_t periph)
{
    adc_deinit(periph);
    adc_clock_config(periph, ADC_CLK_SYNC_HCLK_DIV6);
    adc_data_alignment_config(periph, ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(periph, ADC_ROUTINE_CHANNEL, 1u);
    adc_external_trigger_config(periph, ADC_ROUTINE_CHANNEL, EXTERNAL_TRIGGER_DISABLE);
    adc_enable(periph);

    /* Stabilisation + calibration.  The datasheet wants tSTAB after
     * ADCON before calibrating; a generous spin costs microseconds in
     * this boot-only path.  adc_calibration_enable() then self-blocks
     * on RSTCLB/CLB until the hardware finishes -- without it the
     * converter runs uncalibrated for the whole session (linearity
     * offsets land directly in every reported millivolt). */
    for (volatile uint32_t stab = 0u; stab < 4096u; ++stab) {
    }
    adc_calibration_enable(periph);
}

/* ----------------------------------------------------------------- */
/* Quadrature encoder channels.                                       */
/* ----------------------------------------------------------------- */

/* E1M ENC0..3 -> (TIMER peripheral, X pad, Y pad, AF).  Each encoder
 * binds the underlying timer's CH0 + CH1 input-capture units as the
 * X / Y quadrature pair (decoder mode 2 -> X4 counts on both edges
 * of both inputs).  Maintainer-confirmed mapping:
 *
 *   E1M ENC0  X=PA0  Y=PB3  TIMER1  CH0/CH1  AF1
 *   E1M ENC1  X=PC6  Y=PC7  TIMER2  CH0/CH1  AF2
 *   E1M ENC2  X=PB6  Y=PB7  TIMER3  CH0/CH1  AF2
 *   E1M ENC3  X=PB2  Y=PA1  TIMER4  CH0/CH1  AF2
 *
 * TIMER1 + TIMER4 are 32-bit counters on the GD32G5x3; TIMER2 +
 * TIMER3 are 16-bit.  bridge_hw_qenc_read returns the raw counter
 * cast to int32_t -- the host handles wrap detection via deltas. */

typedef struct {
    uint32_t timer_periph;
    uint32_t gpio_x_port;
    uint32_t gpio_x_pin;
    uint32_t gpio_y_port;
    uint32_t gpio_y_pin;
    uint32_t gpio_af;
} gd32_qenc_t;

static const gd32_qenc_t qenc_map[] = {
    [0] = { TIMER1, GPIOA, GPIO_PIN_0, GPIOB, GPIO_PIN_3, GPIO_AF_1 },
    [1] = { TIMER2, GPIOC, GPIO_PIN_6, GPIOC, GPIO_PIN_7, GPIO_AF_2 },
    [2] = { TIMER3, GPIOB, GPIO_PIN_6, GPIOB, GPIO_PIN_7, GPIO_AF_2 },
    [3] = { TIMER4, GPIOB, GPIO_PIN_2, GPIOA, GPIO_PIN_1, GPIO_AF_2 },
};
#define QENC_CHANNEL_COUNT (sizeof(qenc_map) / sizeof(qenc_map[0]))

/* Per-encoder init.  Configures the X / Y pads as alt-function
 * inputs (pull-up so a disconnected encoder doesn't float), enables
 * the timer's clock, sets it up in quadrature decoder mode 2 (X4
 * counts on both edges of both inputs), and starts the counter. */
static void qenc_channel_init(const gd32_qenc_t *e)
{
    gpio_mode_set(e->gpio_x_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, e->gpio_x_pin);
    gpio_mode_set(e->gpio_y_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, e->gpio_y_pin);
    gpio_af_set(e->gpio_x_port, e->gpio_af, e->gpio_x_pin);
    gpio_af_set(e->gpio_y_port, e->gpio_af, e->gpio_y_pin);

    timer_parameter_struct ip;
    timer_struct_para_init(&ip);
    ip.prescaler         = 0u; /* count every encoder edge   */
    ip.alignedmode       = TIMER_COUNTER_EDGE;
    ip.counterdirection  = TIMER_COUNTER_UP;
    ip.period            = 0xFFFFFFFFu; /* 16-bit timers truncate     */
    ip.clockdivision     = TIMER_CKDIV_DIV1;
    ip.repetitioncounter = 0u;
    timer_deinit(e->timer_periph);
    timer_init(e->timer_periph, &ip);
    timer_quadrature_decoder_mode_config(e->timer_periph, TIMER_QUAD_DECODER_MODE2,
                                         TIMER_IC_POLARITY_RISING, TIMER_IC_POLARITY_RISING);
    timer_enable(e->timer_periph);
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

/* TIMER core clock.  This SoM's SystemInit override runs SYSCLK at
 * 216 MHz (216M-PLL-IRC8M -- see vendors/gd32_firmware_library/
 * overrides/system_gd32g5x3.c) with APB1/APB2 at DIV1, so CK_TIMER =
 * 216 MHz at every timer counter input.  GigaDevice's own PWM
 * example states the same base ("TIMER0 frequency is fixed to
 * 216MHz").  NOTE 2026-06-04: this was wrongly coded as 240 MHz
 * through v0.2.3 -- every PWM period was ~11 % long (a commanded
 * 1 kHz physically ran ~900 Hz).  1 ns LSB resolution would need a
 * faster counter; we instead round period_ns + duty_ns to the
 * nearest 1 us cycle by fixing the prescaler at (216 - 1) so the
 * counter ticks at exactly 1 MHz.  ARR is then `period_us - 1`,
 * fitting in 16 bits for periods up to ~65 ms which covers every
 * realistic control PWM frequency (>=15 Hz). */
#define PWM_TIMER_CLK_HZ 216000000u
#define PWM_TIMER_PRESCALER (216u - 1u) /* 216 MHz -> 1 MHz tick    */
#define PWM_TIMER_TICK_NS 1000u         /* 1 us per timer tick      */
#define PWM_TIMER_ARR_MAX 0xFFFFu       /* 16-bit auto-reload limit */

/* No read-back cache: bridge_hw_pwm_get reads CAR/CHxCV straight from
 * the timer so the host sees what the silicon is actually generating,
 * never a software echo of its own request (silicon lesson 2026-06-04:
 * a cache echo "verified" PWM for weeks while the pads were idle). */

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
#define DAC_VREF_MV 1800u
#define DAC_FULL_SCALE 4095u

/* ----------------------------------------------------------------- */
/* Boot hooks (overrides of the weak defaults in src/main.c)         */
/* ----------------------------------------------------------------- */

/* Called once on entry to main() before the transport ISRs come
 * online.  Future commits also wire up the remaining peripherals
 * the bridge uses (TMU, ADC0..ADC3, DAC, TIMER0/7/19, SysTick, etc.).
 * NOTE: no DA9292 wiring exists on this SoM rev -- the fault nets
 * reach only the Renesas (P37/P36); see bridge_hw_da9292_status_cached. */
void bridge_hw_init(void)
{
#if defined(BRIDGE_OTA_PARTITIONED) && defined(BRIDGE_APP_SLOT_BASE)
    /* OTA Path-A: the app runs from a flash slot, not 0x08000000, so move
     * the vector table off the vendor SystemInit default before any NVIC
     * use.  Runs first (main calls bridge_hw_init before the transports
     * enable interrupts). */
    SCB->VTOR = (uint32_t)(BRIDGE_APP_SLOT_BASE);
    __DSB();
    __ISB();
    /* The Path-A bootloader hands off with PRIMASK SET (it runs
     * __disable_irq() before swapping MSP/VTOR for the jump) -- without
     * clearing it here no interrupt ever fires in the slot app and the
     * transports go silent (silicon-caught 2026-06-04: CM33 retried init
     * 1600+ times against a slot app whose CS-EXTI could never run).
     * Re-enable now that OUR vector table is live; on a plain power-on
     * boot PRIMASK is already clear and this is a no-op. */
    __enable_irq();
#endif

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
     * contention with whatever the board might pull / drive on
     * those pads.  bridge_hw_gpio_write() promotes individual
     * pads to OUTPUT on demand. */
    for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
        gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP,
                      gpio_pad_map[i].pin);
        gpio_is_output[i] = false;
    }

    /* TRNG bring-up: configure + enable only.  The NIST pipeline's
     * first conditioned word can lag past any boot-time wait we are
     * willing to spin; readiness is promoted lazily by the first
     * TRNG_READ's short DRDY poll (by which point the host's boot
     * settle has given the analog source seconds, not milliseconds). */
    trng_started = trng_start();
    if (trng_started) {
        /* Boot-time readiness grace: the transports aren't up yet, so
         * spending a few milliseconds here is free and spares the
         * FIRST host TRNG_READ the one-time conditioning latency
         * (observed on the functional tier: the inaugural read paid
         * the promotion and answered STATUS_IO once).  Bounded; the
         * lazy per-read path remains the safety net. */
        for (unsigned round = 0u; round < 8u && trng_started && !trng_ready; ++round) {
            (void)trng_poll_ready();
        }
    }

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
     * counter at the core clock rate (216 MHz on the GD32G553 in the
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

    /* Quadrature encoder bring-up: TIMER1..4 host the four E1M
     * encoder pairs.  Each timer uses its CH0 + CH1 input-capture
     * units as the X / Y quadrature inputs (decoder mode 2 -> X4). */
    rcu_periph_clock_enable(RCU_TIMER1);
    rcu_periph_clock_enable(RCU_TIMER2);
    rcu_periph_clock_enable(RCU_TIMER3);
    rcu_periph_clock_enable(RCU_TIMER4);
    for (size_t i = 0; i < QENC_CHANNEL_COUNT; ++i) {
        qenc_channel_init(&qenc_map[i]);
    }
}

/* Called from the SysTick handler (or the main loop's idle path) on a
 * fixed cadence.  Intentionally a no-op on this SoM revision: the
 * DA9292 fault nets (DA9292_INT/DA9292_TW) reach only the Renesas
 * (P37/P36) -- the GD32 has no pin to sample and no I2C path to the
 * PMIC.  When a future HW rev mirrors the fault nets onto GD32
 * inputs, this hook samples them and updates the byte returned by
 * bridge_hw_da9292_status_cached(). */
void bridge_hw_tick(void)
{
    /* No-op on this HW rev (nothing to sample). */
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

    /* Clear OPM if a prior bridge_hw_pwm_single_pulse left the timer
     * in one-pulse mode -- per the contract, a subsequent PWM_SET
     * returns the channel (and any other channels on the same timer)
     * to continuous output. */
    timer_single_pulse_mode_config(pwm_channels[channel].periph, TIMER_SP_MODE_REPETITIVE);

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
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_get(uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns)
{
    if (period_ns == 0 || duty_ns == 0) return BRIDGE_HW_ERR_INVAL;
    *period_ns = 0u;
    *duty_ns   = 0u;
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];

    /* Hardware read-back: convert the live CAR/CHxCV ticks to ns at
     * the fixed 1 us tick.  This reports what the pad is actually
     * doing -- including the shared-period reality (one ARR per
     * timer: a PWM_SET on a sibling channel moves this channel's
     * reported period too) and the boot default (65.536 ms period,
     * 0 duty) before the first PWM_SET.  CHxCV can legitimately read
     * ARR + 1 (single-pulse programs compare past the period for a
     * full-width pulse); clamp so duty never reports > period. */
    const uint32_t car = TIMER_CAR(ch->periph) & PWM_TIMER_ARR_MAX;
    uint32_t       cv  = timer_channel_capture_value_register_read(ch->periph, ch->channel);
    if (cv > car + 1u) cv = car + 1u;
    *period_ns = (car + 1u) * PWM_TIMER_TICK_NS;
    *duty_ns   = cv * PWM_TIMER_TICK_NS;
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

    /* A stale EOC (e.g. the in-flight conversion that completes after
     * a stream END drops continuous mode) would satisfy the first poll
     * instantly and serve OLD data as sample 1 -- clear it so every
     * EOC we consume belongs to a conversion WE triggered. */
    adc_flag_clear(ch->periph, ADC_FLAG_EOC);

    /* Take `samples` consecutive conversions, software-triggered per
     * sample.  Polled EOC with a HARD BOUND: this body runs inside the
     * CS-EXTI handler, and an unbounded spin on a wedged ADC took the
     * WHOLE LINK down with it (silicon 2026-06-04: after an adc_stream
     * cycle the next read's EOC never came; the handler never returned,
     * the SPI RX DMA was never re-armed -- captured live with CH3
     * frozen disabled at CNT=66 -- and every subsequent command on
     * every surface failed).  A healthy conversion is ~6.3 us; the
     * ~100k-iteration bound is the abort latch, and on timeout the
     * peripheral is re-initialised (deinit + reconfig + recalibrate)
     * so the NEXT read starts from a clean converter -- same
     * self-healing shape as the TRNG fault path. */
    for (uint8_t i = 0; i < samples; ++i) {
        adc_software_trigger_enable(ch->periph, ADC_ROUTINE_CHANNEL);
        uint32_t to = 100000u;
        while (!adc_flag_get(ch->periph, ADC_FLAG_EOC) && --to) {
            /* spin, bounded */
        }
        if (to == 0u) {
            adc_periph_init(ch->periph);
            return BRIDGE_HW_ERR_IO;
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
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    /* v0.3 partial: accept the default settings that bridge_hw_init's
     * pwm_timer_init() programs (edge-aligned counter, no dead-time,
     * no break input) so the host's idempotent "set to defaults"
     * config calls succeed.  Non-defaults need timer-wide reconfigs
     * (CAM field via re-init, timer_break_config struct) that share
     * across all channels on the same timer; defer to a follow-up
     * with a per-timer apply path + last-write-wins semantics. */
    if (align_mode != 0u) return BRIDGE_HW_ERR_NOTIMPL;
    if (dead_time_ns != 0u) return BRIDGE_HW_ERR_NOTIMPL;
    if (break_cfg != 0u) return BRIDGE_HW_ERR_NOTIMPL;
    return BRIDGE_HW_OK;
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

/* Stream-DMA bring-up state.  Two parallel streams: stream 0 binds
 * DMA0_CH0, stream 1 binds DMA1_CH0.  Each stream owns a circular
 * ring buffer that the DMA fills peripheral-to-memory at the ADC
 * clock; bridge_hw_adc_stream_read drains samples between the host's
 * polls.
 *
 * Ring size is chosen so a 100 kHz stream fills it in ~10 ms (1024
 * samples) -- comfortably above the host's typical poll cadence yet
 * small enough to keep the SRAM footprint inside the GD32G553's
 * 128 KB budget.  Total cost: 2 streams x 1024 samples x 2 bytes =
 * 4 KB. */
#define BRIDGE_ADC_STREAM_RING_SAMPLES 1024u
#define BRIDGE_ADC_STREAM_COUNT 2u

/* Honest sample-rate contract: the requested rate is realised by a
 * dedicated pacing timer (below), not ignored.  100 kHz cap = the
 * ring's documented design point (fills in ~10 ms); 0 is INVAL at
 * the call site, anything above the cap is RANGE. */
#define BRIDGE_ADC_STREAM_RATE_MAX_HZ 100000u

/* Pacing-timer clock.  TIMER5/6 are APB1 basic timers; with APB1 at
 * DIV1 they tick at the full 216 MHz core clock (same base the PWM
 * timers use -- see PWM_TIMER_CLK_HZ).  The silicon validation
 * cross-checks this constant: a wrong base shows up directly as a
 * got-count mismatch over a timed dwell. */
#define BRIDGE_ADC_PACE_CLK_HZ 216000000u

typedef struct {
    bool     in_use;
    uint8_t  channel;     /* ADC channel index this stream watches */
    uint32_t dma_periph;  /* DMA0 or DMA1                          */
    uint8_t  dma_channel; /* dma_channel_enum value                */
    uint32_t pace_timer;  /* TIMER5 (stream 0) or TIMER6 (stream 1) */
    uint16_t ring[BRIDGE_ADC_STREAM_RING_SAMPLES];
    uint16_t read_idx; /* host's consumer cursor                */
    uint8_t  dsp_chain_id;
    bool     dsp_bound;
} adc_stream_state_t;

static adc_stream_state_t adc_streams[BRIDGE_ADC_STREAM_COUNT];

/* TRIGSEL route target for an ADC peripheral's routine-group trigger. */
static trigsel_periph_enum adc_stream_routrg(uint32_t adc_periph)
{
    if (adc_periph == ADC1) return TRIGSEL_OUTPUT_ADC1_ROUTRG;
    if (adc_periph == ADC2) return TRIGSEL_OUTPUT_ADC2_ROUTRG;
    if (adc_periph == ADC3) return TRIGSEL_OUTPUT_ADC3_ROUTRG;
    return TRIGSEL_OUTPUT_ADC0_ROUTRG;
}

/* DMA write-cursor read.  The DMA channel counter counts DOWN from
 * the configured transfer length; converting to a write index uses
 * `ring_samples - remaining`.  Wraps naturally via the circular-mode
 * reload. */
static uint16_t adc_stream_write_index(const adc_stream_state_t *s)
{
    const uint32_t remaining =
        dma_transfer_number_get(s->dma_periph, (dma_channel_enum)s->dma_channel);
    if (remaining > BRIDGE_ADC_STREAM_RING_SAMPLES) return 0u;
    return (uint16_t)(BRIDGE_ADC_STREAM_RING_SAMPLES - remaining);
}

int bridge_hw_adc_stream_begin(uint8_t stream_id, uint8_t channel, uint32_t sample_rate_hz)
{
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;
    if (channel >= ADC_CHANNEL_MAP_COUNT) return BRIDGE_HW_ERR_RANGE;
    if (sample_rate_hz == 0u) return BRIDGE_HW_ERR_INVAL;
    if (sample_rate_hz > BRIDGE_ADC_STREAM_RATE_MAX_HZ) return BRIDGE_HW_ERR_RANGE;

    adc_stream_state_t *s = &adc_streams[stream_id];
    if (s->in_use) return BRIDGE_HW_ERR_INVAL; /* stream already running */

    const gd32_adc_ch_t *ch = &adc_channels_map[channel];

    /* One stream per ADC converter: both streams sharing a peripheral
     * would fight over routine rank 0 AND the TRIGSEL routine-trigger
     * route -- the second begin would silently re-pace and re-point
     * the first.  Refuse honestly instead. */
    for (uint8_t i = 0u; i < BRIDGE_ADC_STREAM_COUNT; ++i) {
        if (i != stream_id && adc_streams[i].in_use &&
            adc_channels_map[adc_streams[i].channel].periph == ch->periph) {
            return BRIDGE_HW_ERR_INVAL;
        }
    }

    /* Stream 0 -> DMA0, stream 1 -> DMA1.  Channel 0 of each DMA
     * controller is the first free slot in the GD32G5x3 dma_channel
     * enum; bridge brings up no other DMA users today so collisions
     * are not a concern. */
    s->dma_periph  = (stream_id == 0u) ? DMA0 : DMA1;
    s->dma_channel = (uint8_t)DMA_CH0;
    s->pace_timer  = (stream_id == 0u) ? TIMER5 : TIMER6;

    /* The DMAMUX request routing dma_init() writes below lands on a
     * clock-gated register unless the mux clock is up.  The SPI
     * transport happens to enable it first at boot today -- own the
     * dependency here instead of relying on bring-up order (silicon
     * 2026-06-04 audit: an I2C-only build would stream zero samples). */
    rcu_periph_clock_enable(RCU_DMAMUX);
    rcu_periph_clock_enable((stream_id == 0u) ? RCU_DMA0 : RCU_DMA1);
    dma_deinit(s->dma_periph, (dma_channel_enum)s->dma_channel);

    dma_parameter_struct init;
    dma_struct_para_init(&init); /* all fields defined before the explicit set */
    init.periph_addr  = (uint32_t)(uintptr_t)&ADC_RDATA(ch->periph);
    init.memory_addr  = (uint32_t)(uintptr_t)s->ring;
    init.direction    = DMA_PERIPHERAL_TO_MEMORY;
    init.number       = BRIDGE_ADC_STREAM_RING_SAMPLES;
    init.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    init.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    init.periph_width = DMA_PERIPHERAL_WIDTH_16BIT;
    init.memory_width = DMA_MEMORY_WIDTH_16BIT;
    init.priority     = DMA_PRIORITY_MEDIUM;
    /* DMAMUX request: route the channel to this ADC instance.  Without
     * this the request id is left uninitialised and the channel triggers
     * on the wrong (or no) source. */
    init.request = (ch->periph == ADC1)   ? DMA_REQUEST_ADC1
                   : (ch->periph == ADC2) ? DMA_REQUEST_ADC2
                   : (ch->periph == ADC3) ? DMA_REQUEST_ADC3
                                          : DMA_REQUEST_ADC0;
    dma_init(s->dma_periph, (dma_channel_enum)s->dma_channel, &init);

    /* Circular mode -- DMA reloads `number` after each cycle so the
     * channel keeps running without firmware re-arms.  Combined with
     * adc_dma_mode_enable below this produces a steady-state
     * peripheral-to-ring pipeline with no firmware in the hot path. */
    dma_circulation_enable(s->dma_periph, (dma_channel_enum)s->dma_channel);

    /* Reconfigure the converter for streaming with ADCON CLEAR, in
     * the vendor's proven order (Examples/ADC/ADC0_routine_channel_
     * with_DMA): mode + trigger + DMA controls all land BEFORE the
     * enable.  Programming CTL1 on an already-running converter is
     * exactly how the v0.2.3 stream silently produced zero samples.
     * Calibration is NOT redone here: an ADCON toggle preserves the
     * boot calibration from adc_periph_init, and recalibrating would
     * be an unbounded vendor spin inside the CS-EXTI handler. */
    adc_disable(ch->periph);
    adc_routine_channel_config(ch->periph, 0u, ch->channel, adc_sample_cycles_cache[channel]);

    /* Each pacing-timer TRGO edge starts exactly ONE routine
     * conversion -- the honest realisation of `sample_rate_hz`.  No
     * continuous mode: the silicon ignores trigger edges that land
     * mid-conversion, so an over-asked rate degrades to the channel's
     * achievable rate instead of corrupting the ring. */
    adc_external_trigger_config(ch->periph, ADC_ROUTINE_CHANNEL, EXTERNAL_TRIGGER_RISING);

    /* THE v0.2.3 got==0 root cause: CTL1.DMA alone stops issuing
     * requests after one DMA run.  CTL1.DDM (request-after-last) keeps
     * the request line live so the circular channel refills forever --
     * the vendor reference enables BOTH, in this order. */
    adc_dma_request_after_last_enable(ch->periph);
    adc_dma_mode_enable(ch->periph);

    /* Clear any End-Of-Conversion left by a prior single-shot
     * bridge_hw_adc_read on this peripheral BEFORE the converter
     * re-enables -- a stale EOC otherwise fires one spurious DMA
     * beat the moment the request unmasks, depositing a phantom
     * zeroth sample and desynchronising the ring cursor. */
    adc_flag_clear(ch->periph, ADC_FLAG_EOC);
    adc_enable(ch->periph);
    for (volatile uint32_t stab = 0u; stab < 4096u; ++stab) {
        /* tSTAB dwell after ADCON, same bound adc_periph_init uses */
    }

    dma_channel_enable(s->dma_periph, (dma_channel_enum)s->dma_channel);

    /* Route the pacing timer's update-event TRGO0 to this converter's
     * routine trigger, then run the timer at the requested rate.  Two
     * prescaler regimes keep the 16-bit period in range: a 1 MHz tick
     * covers 16 Hz..100 kHz exactly where it matters; below 16 Hz a
     * 10 kHz tick stretches to 1 Hz.  Division truncates -- worst-case
     * quantisation is one tick (documented in the protocol spec). */
    rcu_periph_clock_enable(RCU_TRIGSEL);
    trigsel_init(adc_stream_routrg(ch->periph),
                 (stream_id == 0u) ? TRIGSEL_INPUT_TIMER5_TRGO0 : TRIGSEL_INPUT_TIMER6_TRGO0);

    rcu_periph_clock_enable((stream_id == 0u) ? RCU_TIMER5 : RCU_TIMER6);
    timer_deinit(s->pace_timer);
    uint32_t psc, period_ticks;
    if (sample_rate_hz >= 16u) {
        psc          = (BRIDGE_ADC_PACE_CLK_HZ / 1000000u) - 1u; /* 1 MHz tick  */
        period_ticks = 1000000u / sample_rate_hz;                /* 10..62500   */
    } else {
        psc          = (BRIDGE_ADC_PACE_CLK_HZ / 10000u) - 1u; /* 10 kHz tick */
        period_ticks = 10000u / sample_rate_hz;                /* 667..10000  */
    }
    timer_parameter_struct tp;
    timer_struct_para_init(&tp);
    tp.prescaler = (uint16_t)psc;
    tp.period    = period_ticks - 1u;
    timer_init(s->pace_timer, &tp);
    timer_master_output0_trigger_source_select(s->pace_timer, TIMER_TRI_OUT0_SRC_UPDATE);
    timer_enable(s->pace_timer);

    s->in_use       = true;
    s->channel      = channel;
    s->read_idx     = 0u;
    s->dsp_chain_id = 0u;
    s->dsp_bound    = false;
    return BRIDGE_HW_OK;
}

int bridge_hw_adc_stream_read(uint8_t stream_id, uint8_t max_samples, uint8_t *got_samples,
                              uint16_t *mv)
{
    if (got_samples == 0) return BRIDGE_HW_ERR_INVAL;
    *got_samples = 0u;
    if (mv == 0) return BRIDGE_HW_ERR_INVAL;
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;

    adc_stream_state_t *s = &adc_streams[stream_id];
    if (!s->in_use) return BRIDGE_HW_ERR_INVAL;

    /* Drain as many fresh samples as the host asked for, capped by
     * what the DMA has actually deposited since the last read.  The
     * DMA cursor walks forward through the ring; producer/consumer
     * indices wrap independently, and a write cursor that has lapped
     * the reader is detected via the unsigned-arithmetic delta. */
    const uint16_t w = adc_stream_write_index(s);
    uint16_t       avail;
    if (w >= s->read_idx) {
        avail = (uint16_t)(w - s->read_idx);
    } else {
        avail = (uint16_t)((BRIDGE_ADC_STREAM_RING_SAMPLES - s->read_idx) + w);
    }
    if (avail == 0u) return BRIDGE_HW_OK; /* empty ring; host should poll later */

    uint16_t to_emit = (avail < max_samples) ? avail : max_samples;
    for (uint16_t i = 0u; i < to_emit; ++i) {
        uint32_t code = s->ring[s->read_idx];
        if (code > ADC_FULL_SCALE) code = ADC_FULL_SCALE;
        mv[i]       = (uint16_t)((code * ADC_VREF_MV) / ADC_FULL_SCALE);
        s->read_idx = (uint16_t)((s->read_idx + 1u) % BRIDGE_ADC_STREAM_RING_SAMPLES);
    }
    *got_samples = (uint8_t)to_emit;
    return BRIDGE_HW_OK;
}

int bridge_hw_adc_stream_end(uint8_t stream_id)
{
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;
    adc_stream_state_t *s = &adc_streams[stream_id];
    if (!s->in_use) return BRIDGE_HW_OK; /* idempotent */

    /* Stop the trigger SOURCE first (pacing timer), then disarm the
     * ADC's DMA request generation, then the DMA channel -- the other
     * order can leave one in-flight transfer landing after the
     * channel is disabled. */
    const gd32_adc_ch_t *ch = &adc_channels_map[s->channel];
    timer_disable(s->pace_timer);
    timer_deinit(s->pace_timer);
    adc_dma_request_after_last_disable(ch->periph);
    adc_dma_mode_disable(ch->periph);
    dma_channel_disable(s->dma_periph, (dma_channel_enum)s->dma_channel);

    /* A trigger edge may have started a conversion just before the
     * timer stopped.  Dwell past one conversion time (~6.3 us healthy;
     * the spin below is comfortably longer) so it lands, then clear
     * EOC unconditionally -- whether the last EOC went to the DMA or
     * is still latched, the converter must idle CLEAN.  A leftover
     * conversion/EOC straddling into the next single-shot read on the
     * same peripheral is what started the 2026-06-04 link-rot chain. */
    for (volatile uint32_t settle = 0u; settle < 8192u; ++settle) {
        /* fixed dwell, ~tens of microseconds */
    }

    /* Full single-shot restore: deinit + reconfigure + recalibrate.
     * This puts EXTERNAL_TRIGGER_DISABLE, routine length 1 and the
     * boot calibration back so a following bridge_hw_adc_read sees
     * the exact converter state adc_periph_init promised it -- the
     * same self-heal shape the read path's timeout branch uses. */
    adc_periph_init(ch->periph);

    s->in_use    = false;
    s->dsp_bound = false;
    return BRIDGE_HW_OK;
}

int bridge_hw_trng_read(uint8_t *dest, size_t len)
{
    if (dest == 0) return BRIDGE_HW_ERR_INVAL;
    if (len == 0u || len > 32u) return BRIDGE_HW_ERR_RANGE;

    /* Lazy readiness ladder (see trng_started/trng_ready above): a
     * unit that never started reconfigures here; a configured unit
     * that hasn't produced its first word yet gets another short DRDY
     * poll.  Each step is bounded handler-safe work; the host's reply
     * re-read schedule absorbs the one-time latency, and a persistent
     * failure keeps answering STATUS_IO honestly. */
    if (!trng_started) {
        trng_started = trng_start();
        if (!trng_started) return BRIDGE_HW_ERR_IO;
    }
    if (!trng_ready && !trng_poll_ready()) return BRIDGE_HW_ERR_IO;

    /* Pull 32-bit randoms and pack their LSB bytes into `dest`.  A
     * single trng_get_true_random_data() call drains one entry from
     * the TRNG's output FIFO and the unit refills autonomously; we
     * poll DRDY between pulls so a starved-noise condition doesn't
     * silently emit a stale word.
     *
     * The DRDY budget is SHARED across the whole request, NOT
     * per-word: a max-length pull is 8 words = one full 256-bit NIST
     * conditioning round, so the FIFO routinely runs dry mid-request
     * and per-word waits stack up to ~9 ms of handler time -- far
     * beyond the host's reply window.  The overrun reply then
     * tar-pitted the next several commands (functional tier
     * 2026-06-04: trng-32B plus exactly three followers failed on
     * every fresh boot).  When the budget runs out the unit is
     * HEALTHY, just mid-conditioning -- answer BUSY and let the host
     * retry into the next round. */
    uint32_t budget = 2u * TRNG_READY_TIMEOUT;
    size_t   off    = 0u;
    while (off < len) {
        if (trng_faulted()) {
            /* Fault (latched seed/clock error included) -- checked
             * BEFORE the DRDY wait, since a parked unit never raises
             * DRDY again.  Demote so the next call rebuilds (full
             * re-seed); fail THIS call loudly -- never pad with
             * non-random bytes. */
            trng_demote();
            return BRIDGE_HW_ERR_IO;
        }
        while ((RESET == trng_flag_get(TRNG_FLAG_DRDY)) && (budget != 0u)) {
            --budget;
        }
        if (RESET == trng_flag_get(TRNG_FLAG_DRDY)) {
            if (trng_faulted()) {
                trng_demote();
                return BRIDGE_HW_ERR_IO;
            }
            return BRIDGE_HW_ERR_BUSY; /* healthy, mid-conditioning */
        }

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
     * in_b is the actual second operand.
     *
     * ANGLE UNITS: the TMU's SIN/COS angle input is in units of PI
     * (the vendor example writes `theta / DEMO_PI`), while the wire
     * contract is RADIANS -- normalize the F32 angle here.  Q31
     * angles are theta/pi by definition of the fixed-point format
     * and pass through unchanged.  (Caught building the functional
     * tier 2026-06-04: sin(0) == 0 in both conventions, so the soak's
     * zero-angle probe could never see the unit mismatch.) */
    uint32_t a = in_a;
    uint32_t b = in_b;
    if (sin_or_cos) {
        if (format == 1u) {
            a = f32_to_bits(bits_to_f32(in_a) / 3.14159265358979f);
            b = f32_to_bits(1.0f); /* IEEE-754 unit modulus */
        } else {
            b = 0x7FFFFFFFu; /* Q31 ~1.0 */
        }
    }

    /* PACED input writes -- do NOT use the vendor tmu_two_*_write():
     * it issues the two IDATA stores back-to-back, and with a warm
     * i-cache they land ~1 AHB cycle apart and the TMU SWALLOWS the
     * second word; the engine then waits forever for input word 2
     * and ENDF never sets.  Silicon 2026-06-04: SIN computed exactly
     * once per power-up (the cache-cold first call) and timed out on
     * every subsequent call, while hand-driven SWD register writes --
     * milliseconds apart -- always computed.  A TMU_CS read-back
     * between the stores forces an AHB round-trip so the input latch
     * takes word 1 before word 2 arrives.  Raw IDATA stores also skip
     * the bits->float->bits soft-float churn (the wire carries the
     * IEEE-754 pattern already). */
    TMU_IDATA = a;
    if (d->inputs == 2u) {
        (void)TMU_CS;
        TMU_IDATA = b;
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
     * only one read.
     *
     * ANGLE UNITS, output side: ATAN/ATAN2 results come out of the
     * CORDIC in units of PI (the mirror image of the SIN/COS input
     * convention above) -- scale back to the wire's RADIANS for F32.
     * Caught by the functional tier 2026-06-04: atan(1) answered
     * 0.25 (pi/4 in pi-units) with STATUS_OK.  Q31 angle results stay
     * native (theta/pi is the documented Q31 convention). */
    const bool angle_out = (d->mode == TMU_MODE_ATAN) || (d->mode == TMU_MODE_ATAN2);

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
            if (angle_out) {
                fr *= 3.14159265358979f;
            }
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
    if (position == 0) return BRIDGE_HW_ERR_INVAL;
    *position = 0;
    if (encoder >= QENC_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    /* Cast the raw counter (uint32_t) to int32_t.  For 16-bit timers
     * (TIMER2, TIMER3) the upper bits read zero so the value is
     * always positive; for 32-bit timers (TIMER1, TIMER4) the value
     * wraps the full int32_t range.  The host detects wraps via
     * deltas. */
    *position = (int32_t)timer_counter_read(qenc_map[encoder].timer_periph);
    return BRIDGE_HW_OK;
}

int bridge_hw_qenc_reset(uint8_t encoder)
{
    if (encoder >= QENC_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    timer_counter_value_config(qenc_map[encoder].timer_periph, 0u);
    return BRIDGE_HW_OK;
}

int bridge_hw_counter_read(uint8_t counter, uint32_t *ticks)
{
    if (ticks == 0) return BRIDGE_HW_ERR_INVAL;
    *ticks = 0u;
    /* Single free-running counter exposed today; future revisions can
     * carve out additional ids for derived (slower) tick bases.  The
     * DWT counter ticks at the core clock (216 MHz on GD32G553),
     * wraps every ~17.9 s, and is monotonically non-decreasing across
     * reads -- the host can compute deltas without watching for
     * mid-read consistency since the register is atomic. */
    if (counter != 0u) return BRIDGE_HW_ERR_RANGE;
    *ticks = DWT->CYCCNT;
    return BRIDGE_HW_OK;
}

uint8_t bridge_hw_da9292_status_cached(void)
{
    /* 0xFF sentinel unconditionally: no DA9292 net reaches the GD32 on
     * this SoM rev (fault pins are Renesas P37/P36 inputs). */
    return 0xFFu;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B.2) -- advanced timer extras                             */
/* ----------------------------------------------------------------- */

/* Per-channel input-capture state.  Polled model -- the bridge
 * dispatcher runs single-threaded between transport ISRs so the
 * host's bridge_hw_pwm_capture_read poll-loop naturally drains the
 * capture register.  Each entry caches the most-recent (period,
 * pulse-width) pair latched from the timer's CCxVAL register.
 *
 * V2N-specific caveat: the PWM map binds the COMPLEMENTARY output
 * pad (CHxN) of each advanced-timer channel.  The TIx input-capture
 * path reads the MAIN CHx pad, which is a physically different pin
 * on the GD32G5x3.  BEGIN below switches the channel from output
 * mode to input-capture mode -- the CHCTL field is rewritten so the
 * complementary output stops driving -- but the capture pad routing
 * has to be reworked by the maintainer in a hardware-bring-up commit
 * before READ delivers real edges.  Until then the firmware
 * structure (config + polled drain + correct unit conversion) is
 * exercised end-to-end and READ surfaces BRIDGE_HW_ERR_NOTIMPL
 * ("ring empty") as documented. */
typedef struct {
    uint32_t last_tick;         /* most-recent CCxVAL                  */
    uint32_t period_ticks;      /* drained delta -- same-edge to same  */
    uint32_t pulse_width_ticks; /* rising-to-falling delta             */
    uint8_t  edge;              /* 0 rising / 1 falling / 2 both       */
    uint8_t  state;             /* 0 waiting first / 1 have-rising /
                                   2 have-falling                       */
    bool     in_capture;        /* true between BEGIN..END             */
    bool     have_period;       /* a period sample is ready to drain   */
    bool     have_pulse;        /* a pulse-width sample is ready       */
} pwm_capture_state_t;

static pwm_capture_state_t pwm_capture[PWM_CHANNEL_COUNT];

/* CCxIF flag for a channel index 0..3.  Mirrors timer.h's flag macros
 * but exposed here so the polled drain can pick the right one without
 * a switch. */
static uint32_t pwm_capture_flag(uint16_t ch)
{
    switch (ch) {
    case TIMER_CH_0:
        return TIMER_FLAG_CH0;
    case TIMER_CH_1:
        return TIMER_FLAG_CH1;
    case TIMER_CH_2:
        return TIMER_FLAG_CH2;
    case TIMER_CH_3:
        return TIMER_FLAG_CH3;
    default:
        return 0u;
    }
}

/* Drain any newly-latched capture from the timer's CCxVAL into the
 * per-channel state.  Polled from bridge_hw_pwm_capture_read; safe to
 * call when no edge has occurred (clears nothing, leaves state). */
static void pwm_capture_drain(uint8_t channel)
{
    pwm_capture_state_t *s    = &pwm_capture[channel];
    const gd32_pwm_ch_t *ch   = &pwm_channels[channel];
    const uint32_t       flag = pwm_capture_flag(ch->channel);
    if (flag == 0u) return;
    if (RESET == timer_flag_get(ch->periph, flag)) return;

    const uint32_t now = timer_channel_capture_value_register_read(ch->periph, ch->channel);
    timer_flag_clear(ch->periph, flag);

    if (s->edge == 2u) {
        /* Both-edge polarity.  Alternate the state machine: the
         * first edge after BEGIN seeds last_tick; the second is a
         * rising-to-falling delta (pulse_width); the third closes
         * the period.  After the third edge the cycle repeats. */
        switch (s->state) {
        case 0u: /* first edge -- seed */
            s->last_tick = now;
            s->state     = 1u;
            break;
        case 1u: /* second edge -- pulse_width sample */
            s->pulse_width_ticks = now - s->last_tick;
            s->have_pulse        = true;
            s->last_tick         = now;
            s->state             = 2u;
            break;
        case 2u: /* third edge -- period closer */
        default:
            s->period_ticks = (now - s->last_tick) + s->pulse_width_ticks;
            s->have_period  = true;
            s->last_tick    = now;
            s->state        = 1u;
            break;
        }
    } else {
        /* Single-edge polarity (rising OR falling).  Delta between
         * consecutive same-edge captures is the period; pulse_width
         * is not observable in this mode -- left at zero. */
        if (s->state == 0u) {
            s->last_tick = now;
            s->state     = 1u;
        } else {
            s->period_ticks = now - s->last_tick;
            s->have_period  = true;
            s->last_tick    = now;
        }
    }
}

int bridge_hw_pwm_capture_begin(uint8_t channel, uint8_t edge)
{
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
    if (edge > 2u) return BRIDGE_HW_ERR_INVAL;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];

    /* Tear down any output state on this channel.  Disabling the
     * complementary output before switching the channel direction
     * stops the pad driving immediately; bridge_hw_pwm_set will
     * fully re-init both directions on a subsequent call. */
    timer_channel_output_state_config(ch->periph, ch->channel, TIMER_CCX_DISABLE);
    timer_channel_complementary_output_state_config(ch->periph, ch->channel, TIMER_CCXN_DISABLE);

    /* Switch the pad's GPIO mode to AF input (pull-up keeps a
     * disconnected line at a defined level).  The pad still uses the
     * timer's AF -- on the GD32G5 the same AF carries both directions
     * for a given (port, pin) so we don't touch gpio_af_set. */
    gpio_mode_set(ch->gpio_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, ch->gpio_pin);

    /* Configure the input-capture parameters.  Filter=0 (no debounce
     * cycles) -- the host can ask for a fixed filter via a follow-up
     * opcode if a glitchy source needs it.  Prescaler=DIV1 captures
     * every selected edge. */
    timer_ic_parameter_struct ic;
    timer_channel_input_struct_para_init(&ic);
    ic.icpolarity  = (edge == 0u)   ? TIMER_IC_POLARITY_RISING
                     : (edge == 1u) ? TIMER_IC_POLARITY_FALLING
                                    : TIMER_IC_POLARITY_BOTH_EDGE;
    ic.icselection = TIMER_IC_SELECTION_DIRECTTI;
    ic.icprescaler = TIMER_IC_PSC_DIV1;
    ic.icfilter    = 0u;
    timer_input_capture_config(ch->periph, ch->channel, &ic);

    /* Clear any stale capture flag so the first drained edge after
     * BEGIN is genuinely the first new edge. */
    timer_flag_clear(ch->periph, pwm_capture_flag(ch->channel));

    pwm_capture[channel].last_tick         = 0u;
    pwm_capture[channel].period_ticks      = 0u;
    pwm_capture[channel].pulse_width_ticks = 0u;
    pwm_capture[channel].edge              = edge;
    pwm_capture[channel].state             = 0u;
    pwm_capture[channel].in_capture        = true;
    pwm_capture[channel].have_period       = false;
    pwm_capture[channel].have_pulse        = false;
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_capture_read(uint8_t channel, uint32_t *period_ns, uint32_t *pulse_width_ns)
{
    if (period_ns == 0 || pulse_width_ns == 0) return BRIDGE_HW_ERR_INVAL;
    *period_ns      = 0u;
    *pulse_width_ns = 0u;
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    pwm_capture_state_t *s = &pwm_capture[channel];
    if (!s->in_capture) return BRIDGE_HW_ERR_INVAL;

    /* Sample the timer's CCxVAL if a new edge has landed since the
     * last poll.  The drain updates `have_period` / `have_pulse`
     * when enough edges have arrived to compose a full tuple. */
    pwm_capture_drain(channel);
    if (!s->have_period) return BRIDGE_HW_ERR_NOTIMPL; /* ring empty */

    /* Convert ticks back to nanoseconds.  The PWM timers run at the
     * 1 us tick configured by pwm_timer_init (prescaler 216-1 against
     * the 216 MHz timer clock); the bridge_hw.h doc-comment's
     * "~4.16 ns LSB" refers to the unscaled core clock used by the
     * counter peripheral, not the prescaled PWM tick used here. */
    *period_ns      = s->period_ticks * PWM_TIMER_TICK_NS;
    *pulse_width_ns = s->pulse_width_ticks * PWM_TIMER_TICK_NS;

    /* Mark the period drained.  pulse_width is consumed alongside
     * because a subsequent READ should not surface stale data; the
     * next pair of edges populates both fresh. */
    s->have_period = false;
    s->have_pulse  = false;
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_capture_end(uint8_t channel)
{
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];
    /* Disable the input-capture channel.  The contract is "return the
     * pin to high-impedance"; leaving the pad in INPUT + PULL_UP from
     * BEGIN is the closest HW approximation -- a subsequent
     * bridge_hw_pwm_set re-configures it as alt-function output
     * push-pull if the host wants to drive again. */
    timer_channel_output_state_config(ch->periph, ch->channel, TIMER_CCX_DISABLE);
    timer_flag_clear(ch->periph, pwm_capture_flag(ch->channel));

    pwm_capture[channel].in_capture = false;
    return BRIDGE_HW_OK;
}

int bridge_hw_pwm_single_pulse(uint8_t channel, uint32_t pulse_ns)
{
    if (channel >= PWM_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;

    uint32_t pulse_us = pulse_ns / PWM_TIMER_TICK_NS;
    if (pulse_us == 0u) return BRIDGE_HW_ERR_RANGE;
    if (pulse_us > PWM_TIMER_ARR_MAX + 1u) pulse_us = PWM_TIMER_ARR_MAX + 1u;

    const gd32_pwm_ch_t *ch = &pwm_channels[channel];

    /* Reset the timer counter so the pulse starts from t=0 then
     * program ARR = pulse_us (counts up; the channel output stays
     * high until the counter reaches the compare value).  Setting
     * compare = ARR + 1 keeps the output high through the entire
     * period so the pulse width matches `pulse_us`.  After ARR the
     * SP=SINGLE bit halts the timer until the next bridge_hw_pwm_set
     * or another bridge_hw_pwm_single_pulse re-arms it. */
    timer_counter_value_config(ch->periph, 0u);
    timer_autoreload_value_config(ch->periph, (uint32_t)(pulse_us - 1u));
    timer_channel_output_pulse_value_config(ch->periph, ch->channel, pulse_us);
    timer_single_pulse_mode_config(ch->periph, TIMER_SP_MODE_SINGLE);
    timer_enable(ch->periph);

    /* A follow-up bridge_hw_pwm_get reads CAR/CHxCV directly and
     * reports duty == period for the one-shot (compare sits past the
     * period so the pulse spans the full window). */
    return BRIDGE_HW_OK;
}

/* Master/slave id (host-side enum) -> GD32 peripheral base address.
 * The protocol-level contract numbers the three advanced timers
 * compactly as 0 = TIMER0, 1 = TIMER7, 2 = TIMER19; this keeps the
 * wire byte small without naming the underlying SoC peripheral. */
static uint32_t timer_sync_periph(uint8_t id)
{
    switch (id) {
    case 0u:
        return TIMER0;
    case 1u:
        return TIMER7;
    case 2u:
        return TIMER19;
    default:
        return 0u;
    }
}

int bridge_hw_timer_sync(uint8_t master, uint8_t slave, uint8_t mode)
{
    if (master == slave) return BRIDGE_HW_ERR_INVAL;
    const uint32_t mp = timer_sync_periph(master);
    const uint32_t sp = timer_sync_periph(slave);
    if (mp == 0u || sp == 0u) return BRIDGE_HW_ERR_RANGE;

    /* Translate the wire `mode` byte (host-defined: 0 disabled,
     * 1 reset, 2 gated, 3 trigger, 4 external-clock, 5 encoder-mode-1)
     * to the vendor's SMC encoding (defined in gd32g5x3_syscfg.h:
     * TIMER_SLAVE_MODE_* + TIMER_QUAD_DECODER_MODE*).  The two
     * encodings differ in numeric value, so an explicit table is
     * needed here; anything outside the documented range returns
     * INVAL rather than silently selecting a default. */
    uint32_t slave_mode;
    switch (mode) {
    case 0u:
        slave_mode = TIMER_SLAVE_MODE_DISABLE;
        break;
    case 1u:
        slave_mode = TIMER_SLAVE_MODE_RESTART;
        break;
    case 2u:
        slave_mode = TIMER_SLAVE_MODE_PAUSE;
        break;
    case 3u:
        slave_mode = TIMER_SLAVE_MODE_EVENT;
        break;
    case 4u:
        slave_mode = TIMER_SLAVE_MODE_EXTERNAL0;
        break;
    case 5u:
        slave_mode = TIMER_QUAD_DECODER_MODE1;
        break;
    default:
        return BRIDGE_HW_ERR_INVAL;
    }

    /* Master side: emit the update event as TRGO0 (the canonical
     * "I just rolled over" pulse that synchronises a downstream
     * slave to the master's period) + flip the master-slave mode
     * bit so the master broadcasts its trigger.  Idempotent. */
    timer_master_output0_trigger_source_select(mp, TIMER_TRI_OUT0_SRC_UPDATE);
    timer_master_slave_mode_config(mp, TIMER_MASTER_SLAVE_MODE_ENABLE);

    /* Slave side: listen to internal trigger 0 (ITI0).  The
     * SYSCFG_TIMERCFG router maps the slave's ITI0 to a physical
     * upstream TIMER's TRGO; the chip-default routing is the
     * reference-manual table for the slave-master pair selected
     * here, and bring-up tests will tune SYSCFG if a non-default
     * routing is needed for a given (master, slave) combination.
     * Following the vendor pattern, configure the slave-mode AFTER
     * the input-trigger selection so the slave doesn't act on a
     * stale TRGI source. */
    timer_input_trigger_source_select(sp, TIMER_SMCFG_TRGSEL_ITI0);
    timer_slave_mode_select(sp, slave_mode);
    return BRIDGE_HW_OK;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B.3) -- system power-mode set                             */
/* ----------------------------------------------------------------- */

/* ALP_POWER_WAKE_* bits the firmware supports (mirrors the wire
 * encoding in <alp/power.h>).
 *
 * Mapping notes per V2N hardware reality:
 *
 *   - GPIO : routes through PMU_WAKEUP_PIN0..4 -- five fixed pads
 *            on the GD32G553 + board wires the desired triggers
 *            onto them.  Landed §C.15c.
 *   - RTC  : RTC alarm 0 fires on a scheduled wallclock; the
 *            wakeup timer also surfaces under this bit so the
 *            firmware uses the timer (simpler than absolute-time
 *            alarms in a partial bring-up).  Landed §C.25.
 *   - TIMER: any non-zero `wake_after_ms` -- same RTC wakeup-timer
 *            path as RTC.  The bit is redundant when wake_after_ms
 *            > 0 (the timer wakes the chip implicitly per the
 *            <alp/power.h> contract); honouring the bit explicitly
 *            lets a caller arm a "wake on next tick" without a
 *            specific deadline.  Landed §C.25.
 *   - UART_RX / USB / ETH_LINK : no hardware path on the GD32G5
 *            (no LPUART wake / no USB OTG / no MAC).  Future SoCs
 *            on the bridge slot could populate these via the same
 *            opcode; today the firmware rejects them so the host
 *            knows the request is moot.
 */
#define POWER_WAKE_RTC 0x00000001u
#define POWER_WAKE_GPIO 0x00000002u
#define POWER_WAKE_UART_RX 0x00000004u
#define POWER_WAKE_TIMER 0x00000008u
#define POWER_WAKE_USB 0x00000010u
#define POWER_WAKE_ETH_LINK 0x00000020u
#define POWER_WAKE_MASK_SUPPORTED (POWER_WAKE_RTC | POWER_WAKE_GPIO | POWER_WAKE_TIMER)
#define POWER_WAKE_MASK_HW_GATED (POWER_WAKE_UART_RX | POWER_WAKE_USB | POWER_WAKE_ETH_LINK)

/* RTC wakeup timer LSB: with IRC32K (~32 kHz internal) clock and
 * the /16 divider, the timer ticks at 32000/16 = 2000 Hz -- 0.5 ms
 * per tick.  Max wake = 65535 / 2000 = 32.7 s.  Longer waits would
 * need the CKSPRE_2EXP16 mode which sits in a future commit. */
#define POWER_WAKE_LSB_HZ 2000u
#define POWER_WAKE_TIMER_MAX_MS (65535u * 1000u / POWER_WAKE_LSB_HZ)

/* One-time RTC + LSI bring-up that arms the wakeup timer.  Idempotent
 * across multiple power_mode_set calls -- the LSI stays enabled, the
 * RTC source latches to IRC32K once.  Failure (LSI never stabilises,
 * write-protected register won't unlock) leaves rtc_wakeup_ready
 * false and bridge_hw_power_mode_set returns NOSUPPORT for any
 * timer-bearing call. */
static bool rtc_wakeup_ready = false;

static bool rtc_wakeup_init_once(void)
{
    if (rtc_wakeup_ready) return true;

    /* Bring up IRC32K (internal LSI) as the RTC clock source. */
    rcu_osci_on(RCU_IRC32K);
    /* Spin until IRC32K stabilises -- typical < 50 us, the upper
     * bound keeps a dead oscillator from hanging the bridge. */
    uint32_t to = 200000u;
    while (--to && RESET == rcu_flag_get(RCU_FLAG_IRC32KSTB)) {
        /* spin */
    }
    if (to == 0u) return false;

    rcu_periph_clock_enable(RCU_PMU);
    pmu_backup_write_enable();
    rcu_rtc_clock_config(RCU_RTCSRC_IRC32K);
    rcu_periph_clock_enable(RCU_RTC);

    rtc_wakeup_ready = true;
    return true;
}

static int rtc_wakeup_arm_ms(uint32_t wake_after_ms)
{
    if (!rtc_wakeup_init_once()) return BRIDGE_HW_ERR_IO;
    if (wake_after_ms > POWER_WAKE_TIMER_MAX_MS) return BRIDGE_HW_ERR_RANGE;

    /* Compute ticks (round up so a sub-LSB request still waits at
     * least one tick rather than zero). */
    uint32_t ticks = (wake_after_ms * POWER_WAKE_LSB_HZ + 999u) / 1000u;
    if (ticks == 0u) ticks = 1u;
    if (ticks > 65535u) ticks = 65535u;

    /* The vendor sequence: disable the wakeup timer, switch its
     * clock source, set the counter, re-enable.  rtc_wakeup_disable
     * may return ERROR if the WTWF flag never sets; treat as IO. */
    if (SUCCESS != rtc_wakeup_disable()) return BRIDGE_HW_ERR_IO;
    if (SUCCESS != rtc_wakeup_clock_set(WAKEUP_RTCCK_DIV16)) return BRIDGE_HW_ERR_IO;
    if (SUCCESS != rtc_wakeup_timer_set((uint16_t)(ticks - 1u))) return BRIDGE_HW_ERR_IO;
    rtc_wakeup_enable();
    return BRIDGE_HW_OK;
}

static void power_wake_pins_enable(uint32_t wake_bitmap)
{
    if ((wake_bitmap & POWER_WAKE_GPIO) == 0u) return;
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN0);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN1);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN2);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN3);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN4);
}

int bridge_hw_power_mode_set(uint8_t mode, uint32_t wake_bitmap, uint32_t wake_after_ms)
{
    /* Mode 0 (run) + mode 1 (sleep) are accepted no-ops -- main()'s
     * `for (;;) { __WFI(); bridge_hw_tick(); }` already runs the CPU
     * in WFI between transport interrupts, which IS "sleep" on the
     * GD32G5.  Mode 2 (deep-sleep) + mode 3 (standby) call into the
     * vendor's PMU helpers below.
     *
     * Wake-source semantics: `wake_bitmap` enumerates the explicit
     * sources the host wants armed; `wake_after_ms` is a timed
     * fallback that arms the RTC wakeup timer regardless of the
     * bitmap (per the <alp/power.h> contract: the timer is implicit
     * when wake_after_ms > 0).  Unsupported bits (UART_RX / USB /
     * ETH_LINK on the GD32G5 baseline) reject so the host knows the
     * request was not honoured. */
    if ((wake_bitmap & POWER_WAKE_MASK_HW_GATED) != 0u) return BRIDGE_HW_ERR_NOTIMPL;

    switch (mode) {
    case 0u: /* run -- no-op */
    case 1u: /* sleep -- already in WFI between transport ISRs */
        return BRIDGE_HW_OK;
    case 2u: /* deep-sleep */
        rcu_periph_clock_enable(RCU_PMU);
        power_wake_pins_enable(wake_bitmap);
        if (wake_after_ms != 0u || (wake_bitmap & (POWER_WAKE_RTC | POWER_WAKE_TIMER)) != 0u) {
            const uint32_t ms = (wake_after_ms != 0u) ? wake_after_ms : POWER_WAKE_TIMER_MAX_MS;
            int            rc = rtc_wakeup_arm_ms(ms);
            if (rc != BRIDGE_HW_OK) return rc;
        }
        /* PMU_LDO_LOWPOWER drops the core LDO into its low-power
         * regulation point during deepsleep (saves a few hundred
         * uA at the cost of a slightly slower wakeup); WFI_CMD
         * issues the actual `wfi` instruction that suspends the
         * core.  Returns here once a wakeup source fires. */
        pmu_to_deepsleepmode(PMU_LDO_LOWPOWER, WFI_CMD);
        return BRIDGE_HW_OK;
    case 3u: /* standby */
        rcu_periph_clock_enable(RCU_PMU);
        power_wake_pins_enable(wake_bitmap);
        if (wake_after_ms != 0u || (wake_bitmap & (POWER_WAKE_RTC | POWER_WAKE_TIMER)) != 0u) {
            const uint32_t ms = (wake_after_ms != 0u) ? wake_after_ms : POWER_WAKE_TIMER_MAX_MS;
            int            rc = rtc_wakeup_arm_ms(ms);
            if (rc != BRIDGE_HW_OK) return rc;
        }
        /* Standby powers down the core + SRAM (except backup) and
         * wakes via reset -- pmu_to_standbymode() never returns;
         * the SoC re-runs Reset_Handler when a wakeup source fires.
         * The caller's host link will see the bridge re-issue its
         * handshake on the next transport packet, which is the
         * documented contract. */
        pmu_to_standbymode();
        /* Unreachable in normal operation; keep the return so the
         * compiler doesn't warn about a missing terminator. */
        return BRIDGE_HW_OK;
    default:
        return BRIDGE_HW_ERR_INVAL;
    }
    (void)POWER_WAKE_MASK_SUPPORTED;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                     */
/* ----------------------------------------------------------------- */

/* Pool sizing -- mirrors the constants in `<alp/chips/gd32g553.h>`
 * so the host's view of "what fits" agrees with the firmware's
 * actual buffer reservation.  These local copies avoid pulling the
 * SDK header into the firmware tree (which would drag in alp_status_t
 * + supplementary ALP types the firmware doesn't otherwise consume).
 * Bumping any of them requires a coordinated edit on both sides --
 * see `docs/gd32-bridge-protocol.md` §3.x for the wire-format
 * implications. */
#define BRIDGE_DSP_MAX_CHAINS 4u
#define BRIDGE_DSP_MAX_STAGES 4u
#define BRIDGE_DSP_MAX_STAGE_BYTES 260u

/* Valid `kind` byte range -- alp_dsp_stage_kind_t mirrors the wire
 * encoding: 0 FIR, 1 IIR, 2 WINDOW, 3 FFT.  Anything outside this
 * range rejects at stage_push so a typo from the host is caught
 * before any bytes hit the per-stage buffer. */
#define BRIDGE_DSP_KIND_MAX 3u

typedef struct {
    uint8_t  kind;           /* alp_dsp_stage_kind_t (valid when total_size > 0) */
    uint16_t total_size;     /* declared in first chunk; locks for the stage    */
    uint16_t bytes_received; /* running count toward total_size                  */
    bool     complete;       /* bytes_received == total_size                     */
    uint8_t  data[BRIDGE_DSP_MAX_STAGE_BYTES];
} adc_dsp_stage_t;

typedef struct {
    bool            in_use;
    bool            bound;
    adc_dsp_stage_t stages[BRIDGE_DSP_MAX_STAGES];
} adc_dsp_chain_t;

/* 4 chains x 4 stages x 260 B = 4160 bytes of stage-data RAM + ~80
 * bytes of metadata; well inside the GD32G553's 128 KB SRAM. */
static adc_dsp_chain_t adc_dsp_chains[BRIDGE_DSP_MAX_CHAINS];

int                    bridge_hw_adc_dsp_chain_open(uint8_t *chain_id)
{
    if (chain_id == 0) return BRIDGE_HW_ERR_INVAL;
    *chain_id = 0u;

    /* First-fit search over the chain pool.  The pool is small (4
     * entries today) so the linear scan is comfortably faster than
     * any free-list bookkeeping would be; if the pool grows, this
     * function is the natural place to add a free-list head. */
    for (uint8_t i = 0u; i < BRIDGE_DSP_MAX_CHAINS; ++i) {
        if (!adc_dsp_chains[i].in_use) {
            /* Zero the chain state so a previously-released chain
             * doesn't leak stale stage data into the new allocation. */
            for (uint8_t s = 0u; s < BRIDGE_DSP_MAX_STAGES; ++s) {
                adc_dsp_chains[i].stages[s].kind           = 0u;
                adc_dsp_chains[i].stages[s].total_size     = 0u;
                adc_dsp_chains[i].stages[s].bytes_received = 0u;
                adc_dsp_chains[i].stages[s].complete       = false;
            }
            adc_dsp_chains[i].in_use = true;
            adc_dsp_chains[i].bound  = false;
            *chain_id                = i;
            return BRIDGE_HW_OK;
        }
    }
    /* Pool exhaustion.  Protocol layer maps BRIDGE_HW_ERR_NOTIMPL to
     * STATUS_NOSUPPORT today; a STATUS_NOMEM-equivalent would be
     * more accurate but doesn't exist on the wire yet. */
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_dsp_stage_push(uint8_t chain_id, uint8_t stage_index, uint8_t kind,
                                 uint16_t chunk_offset, uint16_t chunk_total_size,
                                 const uint8_t *chunk_data, size_t chunk_data_len)
{
    if (chain_id >= BRIDGE_DSP_MAX_CHAINS) return BRIDGE_HW_ERR_RANGE;
    if (stage_index >= BRIDGE_DSP_MAX_STAGES) return BRIDGE_HW_ERR_RANGE;
    if (kind > BRIDGE_DSP_KIND_MAX) return BRIDGE_HW_ERR_INVAL;
    if (chunk_total_size == 0u) return BRIDGE_HW_ERR_INVAL;
    if (chunk_total_size > BRIDGE_DSP_MAX_STAGE_BYTES) return BRIDGE_HW_ERR_RANGE;
    if (chunk_data_len == 0u || chunk_data == 0) return BRIDGE_HW_ERR_INVAL;
    /* `chunk_offset + chunk_data_len <= chunk_total_size` -- guard
     * against integer overflow on the addition (both inputs are
     * 16-bit-bounded above) by doing the subtraction. */
    if (chunk_data_len > (size_t)(chunk_total_size - chunk_offset)) return BRIDGE_HW_ERR_RANGE;

    adc_dsp_chain_t *chain = &adc_dsp_chains[chain_id];
    if (!chain->in_use) return BRIDGE_HW_ERR_INVAL;
    if (chain->bound) return BRIDGE_HW_ERR_INVAL; /* mutation after bind */

    adc_dsp_stage_t *st = &chain->stages[stage_index];

    if (chunk_offset == 0u) {
        /* First chunk of this stage.  Seed `kind` + `total_size`;
         * any subsequent chunks must agree with these values so a
         * mid-upload re-target of the stage is caught as INVAL. */
        st->kind           = kind;
        st->total_size     = chunk_total_size;
        st->bytes_received = 0u;
        st->complete       = false;
    } else {
        /* Continuation chunk.  The host must keep the same kind +
         * total_size as the first chunk of this (chain, stage)
         * pair -- otherwise the buffer would be a mix of two
         * different stage payloads. */
        if (st->total_size == 0u) return BRIDGE_HW_ERR_INVAL; /* stage not yet opened */
        if (st->kind != kind) return BRIDGE_HW_ERR_INVAL;
        if (st->total_size != chunk_total_size) return BRIDGE_HW_ERR_INVAL;
        if (st->complete) return BRIDGE_HW_ERR_INVAL; /* already done */
    }

    for (size_t i = 0u; i < chunk_data_len; ++i) {
        st->data[chunk_offset + i] = chunk_data[i];
    }
    st->bytes_received += (uint16_t)chunk_data_len;
    if (st->bytes_received == st->total_size) {
        st->complete = true;
    }
    return BRIDGE_HW_OK;
}

int bridge_hw_adc_dsp_chain_bind(uint8_t chain_id, uint8_t stream_id)
{
    if (chain_id >= BRIDGE_DSP_MAX_CHAINS) return BRIDGE_HW_ERR_RANGE;
    if (stream_id >= BRIDGE_ADC_STREAM_COUNT) return BRIDGE_HW_ERR_RANGE;

    adc_dsp_chain_t *chain = &adc_dsp_chains[chain_id];
    if (!chain->in_use) return BRIDGE_HW_ERR_INVAL;
    if (chain->bound) return BRIDGE_HW_ERR_INVAL; /* already attached */

    /* Validate every populated stage is complete + the chain
     * follows the ordering rules documented in
     * `bridge_hw_adc_dsp_chain_bind`'s contract:
     *   - FFT must be the terminal stage (no stage after it),
     *   - WINDOW must immediately precede FFT,
     *   - empty stages (total_size == 0) are allowed only at
     *     contiguous tail positions -- not interleaved with
     *     populated stages. */
    uint8_t fft_index            = BRIDGE_DSP_MAX_STAGES;
    uint8_t window_index         = BRIDGE_DSP_MAX_STAGES;
    uint8_t last_populated_index = BRIDGE_DSP_MAX_STAGES;
    for (uint8_t i = 0u; i < BRIDGE_DSP_MAX_STAGES; ++i) {
        adc_dsp_stage_t *st = &chain->stages[i];
        if (st->total_size == 0u) continue;
        if (!st->complete) return BRIDGE_HW_ERR_INVAL; /* mid-upload */
        if (last_populated_index != BRIDGE_DSP_MAX_STAGES &&
            (uint8_t)(i - last_populated_index) != 1u) {
            return BRIDGE_HW_ERR_INVAL; /* gap in stage list */
        }
        last_populated_index = i;
        if (st->kind == 3u /* FFT */) {
            if (fft_index != BRIDGE_DSP_MAX_STAGES) return BRIDGE_HW_ERR_INVAL;
            fft_index = i;
        } else if (st->kind == 2u /* WINDOW */) {
            if (window_index != BRIDGE_DSP_MAX_STAGES) return BRIDGE_HW_ERR_INVAL;
            window_index = i;
        }
    }
    if (last_populated_index == BRIDGE_DSP_MAX_STAGES) {
        return BRIDGE_HW_ERR_INVAL; /* empty chain */
    }
    if (fft_index != BRIDGE_DSP_MAX_STAGES) {
        /* FFT must be terminal -- no populated stage after it. */
        if (fft_index != last_populated_index) return BRIDGE_HW_ERR_INVAL;
        /* WINDOW (if present) must directly precede the FFT. */
        if (window_index != BRIDGE_DSP_MAX_STAGES &&
            (fft_index == 0u || window_index != fft_index - 1u)) {
            return BRIDGE_HW_ERR_INVAL;
        }
    } else if (window_index != BRIDGE_DSP_MAX_STAGES) {
        /* WINDOW without a terminating FFT has no defined meaning in
         * the filtered-samples path -- reject per docs/gd32-bridge-
         * protocol.md §3.x. */
        return BRIDGE_HW_ERR_INVAL;
    }

    adc_stream_state_t *s = &adc_streams[stream_id];
    if (!s->in_use) return BRIDGE_HW_ERR_INVAL;   /* stream not running */
    if (s->dsp_bound) return BRIDGE_HW_ERR_INVAL; /* stream already has a chain */

    /* Attachment is a state flip on both halves.  Runtime DSP
     * application happens inside stream_read once the wave-2 FFT/FAC
     * dispatch lands; for now the bound chain simply rides alongside
     * the raw stream and the host sees raw mV values until the
     * dispatcher hook ships. */
    s->dsp_chain_id = chain_id;
    s->dsp_bound    = true;
    chain->bound    = true;
    return BRIDGE_HW_OK;
}
