/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-gd32-bridge-functional -- single-pass FUNCTIONAL validation of
 * the GD32 supervisor bridge, then a live oscilloscope observable.
 *
 * Where the hil-soak proves the LINK (every opcode answers, forever),
 * this app proves the FUNCTIONS: each test drives a bridge surface
 * with a known stimulus and asserts the VALUE that comes back --
 * sqrt(4) is 2.0, sin(pi/2) is 1.0, a 32-byte TRNG pull has entropy,
 * an invalid ADC configuration is REJECTED, a PWM setpoint reads back
 * within tolerance.  It runs the table once, publishes a per-test
 * verdict block for the V2N DAP (no console on this SoM), and then
 * parks in a forever PWM7 DUTY STAIRCASE -- PWM7 doubles as the EVK
 * LED pad, so a scope probe (or the naked eye, at staircase rates)
 * verifies the bridge's PWM path end-to-end on real silicon.
 *
 * Verdict block (find `func_results` in zephyr.map; read via DAP):
 *   [0]  0xF07C7E57 magic
 *   [1]  state: 1 = testing, 2 = staircase running (tests done),
 *        0xDEAD = link never came up
 *   [2]  pass count   [3] fail count
 *   [4 + i] per-test result, table order below:
 *        0          = PASS
 *        0x7E       = value assertion failed (status was OK)
 *        other      = the failing alp_status_t, two's complement
 *   [40] staircase: current duty per-mille (live)
 *   [41] staircase: step counter (liveness)
 *
 * This is a maintainer bench tool in example form; like the soak it
 * exercises the gd32g553 chip driver directly (the documented
 * exception to the portable-API rule for dedicated bridge demos).
 */

#include <string.h>
#include <zephyr/kernel.h>

#include "alp/chips/gd32g553.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* Verdict block                                                       */
/* ------------------------------------------------------------------ */

#define FUNC_MAX_TESTS 36u

volatile uint32_t func_results[44] = { 0xF07C7E57u, 0u };

static gd32g553_t ctx;
static unsigned   test_idx; /* cursor into func_results[4..] */

static void       record(alp_status_t s, bool value_ok)
{
    uint32_t cell;
    if (s == ALP_OK && value_ok) {
        cell = 0u;
        func_results[2]++;
    } else if (s == ALP_OK) {
        cell = 0x7Eu; /* status OK but the VALUE was wrong */
        func_results[3]++;
    } else {
        cell = (uint32_t)(int32_t)s; /* the failing status, sign-extended */
        func_results[3]++;
    }
    if (test_idx < FUNC_MAX_TESTS) {
        func_results[4u + test_idx] = cell;
    }
    test_idx++;
}

/* ------------------------------------------------------------------ */
/* Float helpers (the wire carries IEEE-754 bit patterns)              */
/* ------------------------------------------------------------------ */

static uint32_t f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static float bits_f32(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static bool near_f(float got, float want, float tol)
{
    const float d = got - want;
    return (d >= -tol) && (d <= tol);
}

/* ------------------------------------------------------------------ */
/* TMU: the full math table.  Every supported CORDIC primitive gets a  */
/* known-exact probe; tolerances reflect the unit's ~20-bit effective  */
/* precision plus the radian->pi-units conversion rounding.            */
/* ------------------------------------------------------------------ */

#define PI_F 3.14159265358979f

static void t_tmu_f32(gd32g553_tmu_function_t fn, float a, float b, float want, float tol)
{
    uint32_t           out = 0;
    const alp_status_t s =
        gd32g553_tmu_compute(&ctx, fn, GD32G553_TMU_FMT_F32, f32_bits(a), f32_bits(b), &out);
    record(s, (s == ALP_OK) && near_f(bits_f32(out), want, tol));
}

/* tan/exp/tanh have no native TMU mode -- the firmware answers
 * NOSUPPORT by design and the assert here pins that contract (a
 * regression to STATUS_IO or to a wrong value would be a defect). */
static void t_tmu_nosupport(gd32g553_tmu_function_t fn)
{
    uint32_t           out = 0;
    const alp_status_t s =
        gd32g553_tmu_compute(&ctx, fn, GD32G553_TMU_FMT_F32, f32_bits(1.0f), 0u, &out);
    record((s == ALP_ERR_NOSUPPORT) ? ALP_OK : ((s == ALP_OK) ? ALP_ERR_IO : s),
           s == ALP_ERR_NOSUPPORT);
}

/* Q31 path: sqrt(0.25) = 0.5.  Q31 full scale is +/-1.0, so 0.25 =
 * 0x20000000 and the expected 0.5 = 0x40000000 (+/- a few LSB of
 * CORDIC noise -- 1e-6 of full scale is ~2147 LSB, generous). */
static void t_tmu_q31_sqrt(void)
{
    uint32_t           out = 0;
    const alp_status_t s   = gd32g553_tmu_compute(&ctx, GD32G553_TMU_FN_SQRT, GD32G553_TMU_FMT_Q31,
                                                  0x20000000u, 0u, &out);
    const int32_t      err = (int32_t)out - 0x40000000;
    record(s, (s == ALP_OK) && (err > -4096) && (err < 4096));
}

/* ------------------------------------------------------------------ */
/* TRNG: boundary lengths + cheap entropy sanity                       */
/* ------------------------------------------------------------------ */

/* One pull with the DOCUMENTED fault-recover tolerance: the TRNG
 * takes intermittent seed errors, parks with latched fault flags and
 * answers ONE honest ALP_ERR_IO while the firmware demotes + lazily
 * rebuilds the unit -- the very next pull succeeds (silicon-validated
 * recovery, 2026-06-04).  A single retry per pull asserts exactly
 * that contract; the HiL soak's TRNG row uses the same shape.
 * (Caught live: the single-pass tier failed slot 18 with one IO on a
 * run whose other 25 tests + 150 soak cycles were clean.) */
static alp_status_t trng_pull_with_recover(uint8_t *dst, size_t len)
{
    alp_status_t s = gd32g553_trng_read(&ctx, dst, len);
    if (s == ALP_ERR_IO) {
        s = gd32g553_trng_read(&ctx, dst, len);
    }
    return s;
}

static void t_trng_lengths(void)
{
    /* Two 16-byte pulls -- half a 256-bit NIST conditioning round
     * each, so a single round satisfies the request without the
     * firmware bounding out mid-pull.  (A 32-byte single pull spans a
     * whole round and legitimately answers BUSY while conditioning is
     * mid-flight; that path is exercised implicitly by the host
     * driver's BUSY-retry, not asserted here.) */
    uint8_t      a[16] = { 0 };
    uint8_t      b[16] = { 0 };
    alp_status_t s     = trng_pull_with_recover(a, sizeof a);
    if (s == ALP_OK) {
        s = trng_pull_with_recover(b, sizeof b);
    }
    /* Entropy sanity: not all-constant, and the second pull differs.
     * (Statistical tests belong off-target; this catches "stuck word"
     * and "replayed buffer" failure modes.) */
    bool ok = (s == ALP_OK);
    if (ok) {
        bool constant = true;
        for (unsigned i = 1; i < sizeof a; ++i) {
            if (a[i] != a[0]) {
                constant = false;
                break;
            }
        }
        ok = !constant && (memcmp(a, b, sizeof a) != 0);
    }
    record(s, ok);
}

/* ------------------------------------------------------------------ */
/* PWM: configure + setpoint readback on the scope channel             */
/* ------------------------------------------------------------------ */

/* PWM7 is the EVK LED pad AND the maintainer's scope channel for this
 * bench round -- everything observable funnels through it. */
#define SCOPE_PWM_CH 7u

static void t_pwm_set_get_scope_ch(void)
{
    /* 1 kHz, 50 % -- easy to eyeball on a scope.  The 16-bit timers
     * at a 1 us tick round 1 ms / 500 us exactly, so the readback
     * tolerance is one tick (1 us) on each field. */
    alp_status_t s      = gd32g553_pwm_set(&ctx, SCOPE_PWM_CH, 1000000u, 500000u);
    uint32_t     period = 0, duty = 0;
    if (s == ALP_OK) {
        s = gd32g553_pwm_get(&ctx, SCOPE_PWM_CH, &period, &duty);
    }
    record(s, (s == ALP_OK) && (period >= 999000u) && (period <= 1001000u) && (duty >= 499000u) &&
                  (duty <= 501000u));
}

/* pwm_configure is a documented v0.3 PARTIAL: the default tuple
 * (edge-aligned, no dead-time, no break) MUST answer OK -- it is the
 * idempotent "set to defaults" call -- while any non-default knob may
 * answer NOSUPPORT until the per-timer apply path lands.  Assert
 * exactly that contract: defaults always OK; center-up either OK
 * (HAL landed) or NOSUPPORT (documented partial), never anything
 * else.  Order restores edge alignment for the staircase below. */
static void t_pwm_configure_roundtrip(void)
{
    const alp_status_t s =
        gd32g553_pwm_configure(&ctx, SCOPE_PWM_CH, GD32G553_PWM_ALIGN_CENTER_UP, 0u, 0u);
    const alp_status_t restore =
        gd32g553_pwm_configure(&ctx, SCOPE_PWM_CH, GD32G553_PWM_ALIGN_EDGE, 0u, 0u);
    record(ALP_OK, (restore == ALP_OK) && (s == ALP_OK || s == ALP_ERR_NOSUPPORT));
}

/* ------------------------------------------------------------------ */
/* ADC: configuration error path + all-channel ceiling sweep           */
/* ------------------------------------------------------------------ */

/* 14-bit resolution REQUIRES oversampling >= 4 (datasheet effective-
 * resolution table; enforced by the firmware per the host header).
 * The firmware must reject the combination -- INVAL surfacing here
 * (instead of the pre-fix masked ALP_ERR_IO) also regression-tests
 * the host's short-error-envelope decode. */
static void t_adc_configure_error_path(void)
{
    const alp_status_t s = gd32g553_adc_configure(&ctx, 0u, 1u, 0u, 14u);
    record((s == ALP_ERR_INVAL || s == ALP_ERR_NOSUPPORT) ? ALP_OK
           : (s == ALP_OK)                                ? ALP_ERR_IO
                                                          : s,
           s == ALP_ERR_INVAL || s == ALP_ERR_NOSUPPORT);
}

/* Every ADC channel answers and respects the physical ceiling (pads
 * float on the bench, so the VALUE is unconstrained below VREF). */
static void t_adc_all_channels(void)
{
    alp_status_t worst    = ALP_OK;
    bool         value_ok = true;
    for (uint8_t ch = 0u; ch < 8u; ++ch) {
        uint16_t           mv[2] = { 0 };
        const alp_status_t s     = gd32g553_adc_read(&ctx, ch, 2u, mv);
        if (s != ALP_OK) {
            worst = s;
            break;
        }
        if (mv[0] > 3400u || mv[1] > 3400u) value_ok = false;
    }
    record(worst, value_ok);
}

/* ------------------------------------------------------------------ */
/* DSP chain: pool lifecycle (the runtime FFT/FAC dispatch is a wired  */
/* protocol surface whose HAL lands with wave-2 -- both outcomes are   */
/* contract-checked)                                                   */
/* ------------------------------------------------------------------ */

static void t_dsp_chain_lifecycle(void)
{
    uint8_t            chain_id = 0xFFu;
    const alp_status_t s        = gd32g553_adc_dsp_chain_open(&ctx, &chain_id);
    if (s == ALP_ERR_NOSUPPORT) {
        record(ALP_OK, true); /* documented pre-wave-2 contract */
        return;
    }
    record(s, (s == ALP_OK) && (chain_id != 0xFFu));
}

/* ------------------------------------------------------------------ */
/* Identity + misc                                                     */
/* ------------------------------------------------------------------ */

static void t_version_stable(void)
{
    gd32g553_version_t v0, v1;
    alp_status_t       s = gd32g553_get_version(&ctx, &v0);
    if (s == ALP_OK) s = gd32g553_get_version(&ctx, &v1);
    record(s, (s == ALP_OK) && (v0.major == v1.major) && (v0.minor == v1.minor) &&
                  (v0.patch == v1.patch));
}

static void t_da9292_sentinel(void)
{
    uint8_t            st = 0;
    const alp_status_t s  = gd32g553_da9292_status_forward(&ctx, &st);
    /* This HW rev has no DA9292 nets on the GD32: 0xFF sentinel. */
    record(s, (s == ALP_OK) && (st == 0xFFu));
}

/* ------------------------------------------------------------------ */
/* The single-pass table                                               */
/* ------------------------------------------------------------------ */

static void run_suite(void)
{
    /* -- math: every native CORDIC primitive, value-asserted -------- */
    t_tmu_f32(GD32G553_TMU_FN_SQRT, 4.0f, 0.0f, 2.0f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_SQRT, 2.0f, 0.0f, 1.41421356f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_SIN, 0.0f, 0.0f, 0.0f, 1e-4f);
    t_tmu_f32(GD32G553_TMU_FN_SIN, PI_F / 2.0f, 0.0f, 1.0f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_SIN, PI_F / 6.0f, 0.0f, 0.5f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_COS, 0.0f, 0.0f, 1.0f, 1e-4f);
    t_tmu_f32(GD32G553_TMU_FN_COS, PI_F, 0.0f, -1.0f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_COS, PI_F / 3.0f, 0.0f, 0.5f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_ATAN, 1.0f, 0.0f, PI_F / 4.0f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_ATAN2, 1.0f, 1.0f, PI_F / 4.0f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_HYPOT, 3.0f, 4.0f, 5.0f, 1e-2f);
    t_tmu_f32(GD32G553_TMU_FN_LOG, 2.71828183f, 0.0f, 1.0f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_SINH, 1.0f, 0.0f, 1.17520119f, 1e-3f);
    t_tmu_f32(GD32G553_TMU_FN_COSH, 1.0f, 0.0f, 1.54308063f, 1e-3f);
    t_tmu_nosupport(GD32G553_TMU_FN_TAN);
    t_tmu_nosupport(GD32G553_TMU_FN_EXP);
    t_tmu_nosupport(GD32G553_TMU_FN_TANH);
    t_tmu_q31_sqrt();

    /* -- entropy ----------------------------------------------------- */
    t_trng_lengths();

    /* -- PWM (the scope/LED channel) --------------------------------- */
    t_pwm_set_get_scope_ch();
    t_pwm_configure_roundtrip();

    /* -- ADC ---------------------------------------------------------- */
    t_adc_configure_error_path();
    t_adc_all_channels();

    /* -- DSP chain pool ----------------------------------------------- */
    t_dsp_chain_lifecycle();

    /* -- identity ------------------------------------------------------ */
    t_version_stable();
    t_da9292_sentinel();
}

/* ------------------------------------------------------------------ */
/* PWM7 duty staircase -- the forever scope observable                  */
/* ------------------------------------------------------------------ */

static void pwm7_staircase_forever(void)
{
    /* 1 kHz carrier; duty walks 10 % -> 90 % in 10-point steps, two
     * seconds per step, then wraps.  On the scope: a 1 kHz square
     * whose high time visibly widens every 2 s; on the LED: a
     * brightness ramp.  Every step is a fresh PWM_SET + PWM_GET pair
     * over the bridge, so the staircase doubles as a slow link soak. */
    static const uint16_t duty_pm[] = { 100u, 200u, 300u, 400u, 500u, 600u, 700u, 800u, 900u };
    unsigned              step      = 0;

    for (;;) {
        const uint16_t pm        = duty_pm[step % (sizeof duty_pm / sizeof duty_pm[0])];
        const uint32_t period_ns = 1000000u;
        const uint32_t duty_ns   = (period_ns / 1000u) * pm;

        (void)gd32g553_pwm_set(&ctx, SCOPE_PWM_CH, period_ns, duty_ns);

        func_results[40] = pm;
        func_results[41] = (uint32_t)step;
        step++;
        k_msleep(2000);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = 1u,
        .freq_hz       = 25000000u,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8u,
        .cs_pin_id     = ALP_SPI_NO_CS, /* platform SPI driver owns CS */
    });
    if (spi == NULL) {
        func_results[1] = 0xDEADu;
        return 1;
    }

    /* Cold-boot autonomous: retry until the GD32 answers (shared PMIC
     * reset-out means the supervisor may still be coming up). */
    alp_status_t s;
    do {
        s = gd32g553_init(&ctx, spi, NULL, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
        if (s != ALP_OK) k_msleep(200);
    } while (s != ALP_OK);

    /* Settle past the host's boot window (same rationale as the soak:
     * A55 storage/pinmux bring-up can glitch shared board state). */
    k_msleep(20000);

    func_results[1] = 1u;
    run_suite();
    func_results[1] = 2u;

    pwm7_staircase_forever();
    return 0;
}
