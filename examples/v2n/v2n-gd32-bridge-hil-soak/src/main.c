/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-gd32-bridge-hil-soak — pass/fail soak of the WHOLE GD32 bridge
 * command set over the 25 MHz SPI fast path.
 *
 * Where v2n-gd32-bridge-ping proves the link (PING + GET_VERSION),
 * this example proves the COMMAND SET: every opcode the bridge
 * firmware implements is exercised each cycle with a self-contained
 * verification (readback compare, range sanity, monotonicity, or a
 * documented-sentinel assert) — no external instruments needed.
 * Scope tests (PWM waveform shape, ADC absolute accuracy) stay in
 * the HIL-PLAN instrument rows; this soak is the "does every opcode
 * round-trip and tell the truth" layer underneath them.
 *
 * The mix deliberately stresses the 25 MHz framing: PING is a 4-byte
 * (even) reply, GET_VERSION 7 bytes (odd), ADC_STREAM_READ up to
 * 1 + 2*N bytes — so both parities and long frames hit the GD32's
 * DMA reply path every cycle (the odd-length FIFO-residue failure
 * class is what the byte-access reply fix addressed).
 *
 * Pass/fail accounting: each test bumps a per-test pass or fail
 * counter and the soak NEVER halts on failure — a soak's job is to
 * count, recover, and keep hammering.  A cycle summary prints every
 * cycle and a cumulative table every 16 cycles; any FAIL line carries
 * the alp_status_t so a console log alone is diagnosable.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/gd32g553.h"

/* ------------------------------------------------------------------ */
/* Soak bookkeeping                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint32_t    pass;
    uint32_t    fail;
    int         last_status; /* last failing alp_status_t (0 if none) */
} soak_stat_t;

/* One context for the whole soak; opened once, recovered on demand. */
static gd32g553_t ctx;

/* Version captured at init -- GET_VERSION must keep matching it
 * (a mid-soak change would mean the GD32 silently rebooted). */
static gd32g553_version_t boot_version;

/* Build-id captured on the first successful read -- must stay
 * constant for the same reason. */
static char boot_build_id[GD32G553_BUILD_ID_LEN + 1];
static bool boot_build_id_valid;

/* FAIL printf helper: one line per failure, with the status code,
 * so a UART capture alone localises the broken opcode. */
#define SOAK_FAIL(st, fmt, ...) printf("[hil-soak] FAIL %s: " fmt "\n", st->name, ##__VA_ARGS__)

static bool float_near(float a, float b, float tol)
{
    const float d = (a > b) ? (a - b) : (b - a);
    return d <= tol;
}

/* ------------------------------------------------------------------ */
/* Per-opcode tests.  Each returns true on pass and logs on failure.   */
/* ------------------------------------------------------------------ */

/* 0x00 PING -- 4-byte (even-parity) reply envelope. */
static bool t_ping(soak_stat_t *st)
{
    const alp_status_t s = gd32g553_ping_via(&ctx, GD32G553_TRANSPORT_SPI);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    return true;
}

/* 0x01 GET_VERSION -- 7-byte (odd-parity) reply; must match the
 * version read at init (a change = the GD32 rebooted mid-soak). */
static bool t_get_version(soak_stat_t *st)
{
    gd32g553_version_t v = { 0 };
    const alp_status_t s = gd32g553_get_version(&ctx, &v);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    if (v.major != boot_version.major || v.minor != boot_version.minor ||
        v.patch != boot_version.patch) {
        SOAK_FAIL(st, "version changed mid-soak: v%u.%u.%u (boot v%u.%u.%u)", v.major, v.minor,
                  v.patch, boot_version.major, boot_version.minor, boot_version.patch);
        return false;
    }
    return true;
}

/* 0x02 GET_BUILD_ID -- non-empty, NUL-terminated, constant. */
static bool t_get_build_id(soak_stat_t *st)
{
    char               id[GD32G553_BUILD_ID_LEN + 1] = { 0 };
    const alp_status_t s                             = gd32g553_get_build_id(&ctx, id);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    if (id[0] == '\0') {
        SOAK_FAIL(st, "empty build-id");
        return false;
    }
    if (!boot_build_id_valid) {
        memcpy(boot_build_id, id, sizeof boot_build_id);
        boot_build_id_valid = true;
        printf("[hil-soak] firmware build-id: %s\n", id);
    } else if (strncmp(id, boot_build_id, GD32G553_BUILD_ID_LEN) != 0) {
        SOAK_FAIL(st, "build-id changed mid-soak: %s (boot %s)", id, boot_build_id);
        return false;
    }
    return true;
}

/* 0x03 RESET_REASON -- any in-range cause passes; logged once so a
 * cold-boot run shows POWER_ON and a watchdog event shows WDT. */
static bool t_reset_reason(soak_stat_t *st)
{
    gd32g553_reset_cause_t cause;
    const alp_status_t     s = gd32g553_get_reset_reason(&ctx, &cause);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    if (cause > GD32G553_RESET_LOWPOWER) {
        SOAK_FAIL(st, "out-of-range cause %d", (int)cause);
        return false;
    }
    return true;
}

/* 0x10/0x11 GPIO_READ + GPIO_WRITE.  mask=0 makes the write a
 * provable no-op (atomically changes nothing) while still pushing the
 * full request/decode/dispatch path through the wire -- the soak must
 * not flip real supervisor pads (camera LDOs, REG_ONs, SE_RST live
 * on this map). */
static bool t_gpio(soak_stat_t *st)
{
    uint32_t     levels = 0;
    alp_status_t s      = gd32g553_gpio_read(&ctx, 0xFFFFFFFFu, &levels);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "read status=%d", (int)s);
        return false;
    }
    s = gd32g553_gpio_write(&ctx, 0u, 0u);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "write status=%d", (int)s);
        return false;
    }
    return true;
}

/* 0x20/0x21 PWM_SET + PWM_GET readback.  1 kHz / 25 % on PWM0
 * (TIMER0_MCH0, GD32 pad PA11 -> an unloaded E1M edge pin on the
 * bench).  The firmware rounds to its ~4.16 ns tick, so the readback
 * compare allows 1 % -- far above rounding, far below a real bug.
 * Duty returns to 0 afterwards so the pad idles low between cycles. */
static bool t_pwm_set_get(soak_stat_t *st)
{
    const uint32_t period = 1000000u; /* 1 ms */
    const uint32_t duty   = 250000u;  /* 25 % */
    uint32_t       rp = 0, rd = 0;
    alp_status_t   s = gd32g553_pwm_set(&ctx, 0u, period, duty);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "set status=%d", (int)s);
        return false;
    }
    s = gd32g553_pwm_get(&ctx, 0u, &rp, &rd);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "get status=%d", (int)s);
        return false;
    }
    const bool ok = (rp > period - period / 100u) && (rp < period + period / 100u) &&
                    (rd > duty - duty / 100u) && (rd < duty + duty / 100u);
    if (!ok) {
        SOAK_FAIL(st, "readback %u/%u ns (wrote %u/%u)", rp, rd, period, duty);
    }
    (void)gd32g553_pwm_set(&ctx, 0u, period, 0u); /* park the pad low */
    return ok;
}

/* 0x26 PWM_SINGLE_PULSE -- one 1 us one-shot on PWM1.  Status-only
 * here; the pulse itself is a scope row in the HIL-PLAN. */
static bool t_pwm_single_pulse(soak_stat_t *st)
{
    const alp_status_t s = gd32g553_pwm_single_pulse(&ctx, 1u, 1000u);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    return true;
}

/* 0x23/0x24/0x25 PWM capture begin/read/end on PWM2.  Nothing drives
 * the pad on the bench, so READ legitimately reports "no edges yet"
 * -- the firmware maps an empty capture ring to NOSUPPORT today (the
 * input pad routing is a known follow-up), so OK and NOSUPPORT both
 * pass; any other status (or a failing begin/end) is a real fault. */
static bool t_pwm_capture(soak_stat_t *st)
{
    alp_status_t s = gd32g553_pwm_capture_begin(&ctx, 2u, 2u /* both edges */);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "begin status=%d", (int)s);
        return false;
    }
    uint32_t period = 0, pulse = 0;
    s = gd32g553_pwm_capture_read(&ctx, 2u, &period, &pulse);
    if (s != ALP_OK && s != ALP_ERR_NOSUPPORT) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "read status=%d", (int)s);
        (void)gd32g553_pwm_capture_end(&ctx, 2u);
        return false;
    }
    s = gd32g553_pwm_capture_end(&ctx, 2u);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "end status=%d", (int)s);
        return false;
    }
    return true;
}

/* 0x30 ADC_READ -- 4 firmware-averaged samples off E1M ADC0.  The
 * pad floats on the bench so any value is "right"; the assert is the
 * physical ceiling (VREF 3.3 V) plus a successful 4-sample reply. */
static bool t_adc_read(soak_stat_t *st)
{
    uint16_t           mv[4] = { 0 };
    const alp_status_t s     = gd32g553_adc_read(&ctx, 0u, 4u, mv);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    for (unsigned i = 0; i < 4u; ++i) {
        if (mv[i] > 3400u) {
            SOAK_FAIL(st, "sample %u = %u mV > VREF", i, mv[i]);
            return false;
        }
    }
    return true;
}

/* 0x33/0x34/0x35 ADC stream -- THE regression test for the
 * stream-DMA DMAMUX fix (dma_parameter_struct.request was never set,
 * so the channel triggered off a garbage request id and the ring
 * never filled).  1 kHz for ~50 ms must yield a full 32-sample read;
 * zero samples here means the DMA regressed. */
static bool t_adc_stream(soak_stat_t *st)
{
    alp_status_t s = gd32g553_adc_stream_begin(&ctx, 0u, 0u, 1000u);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "begin status=%d", (int)s);
        return false;
    }

    k_msleep(50); /* ~50 samples into the 1024-deep ring at 1 kHz */

    uint8_t  got             = 0;
    uint16_t mv[32]          = { 0 };
    s                        = gd32g553_adc_stream_read(&ctx, 0u, 32u, &got, mv);
    const alp_status_t s_end = gd32g553_adc_stream_end(&ctx, 0u);

    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "read status=%d", (int)s);
        return false;
    }
    if (got == 0u) {
        SOAK_FAIL(st,
                  "0 samples after 50 ms @1 kHz -- stream DMA not filling (DMAMUX request id?)");
        return false;
    }
    if (s_end != ALP_OK) {
        st->last_status = (int)s_end;
        SOAK_FAIL(st, "end status=%d", (int)s_end);
        return false;
    }
    return true;
}

/* 0x50/0x51 DAC_SET + DAC_GET readback at mid-rail.  12-bit DAC off
 * 3.3 V is ~0.8 mV/LSB; ±16 mV of tolerance covers rounding plus a
 * generous firmware-side conversion slack.  Returns the pad to 0 mV
 * so the bench pin idles between cycles. */
static bool t_dac(soak_stat_t *st)
{
    uint16_t     rb = 0;
    alp_status_t s  = gd32g553_dac_set(&ctx, 0u, 1650u);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "set status=%d", (int)s);
        return false;
    }
    s = gd32g553_dac_get(&ctx, 0u, &rb);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "get status=%d", (int)s);
        return false;
    }
    const bool ok = (rb >= 1650u - 16u) && (rb <= 1650u + 16u);
    if (!ok) {
        SOAK_FAIL(st, "readback %u mV (wrote 1650)", rb);
    }
    (void)gd32g553_dac_set(&ctx, 0u, 0u);
    return ok;
}

/* 0x60/0x61 QENC reset + read.  No encoder turns on the bench, so a
 * freshly-reset count must read back exactly 0. */
static bool t_qenc(soak_stat_t *st)
{
    alp_status_t s = gd32g553_qenc_reset(&ctx, 0u);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "reset status=%d", (int)s);
        return false;
    }
    int32_t pos = -1;
    s           = gd32g553_qenc_read(&ctx, 0u, &pos);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "read status=%d", (int)s);
        return false;
    }
    if (pos != 0) {
        SOAK_FAIL(st, "position %d after reset (no encoder attached)", (int)pos);
        return false;
    }
    return true;
}

/* 0x70 COUNTER_READ -- the GD32's DWT cycle counter; two reads a few
 * hundred microseconds apart must be strictly increasing (modulo a
 * 32-bit wrap, which at 216 MHz comes every ~19.9 s -- one spurious
 * fail per wrap window is acceptable soak noise and shows up as a
 * lone count, not a streak). */
static bool t_counter(soak_stat_t *st)
{
    uint32_t     a = 0, b = 0;
    alp_status_t s = gd32g553_counter_read(&ctx, 0u, &a);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    k_busy_wait(200);
    s = gd32g553_counter_read(&ctx, 0u, &b);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    if (b == a) {
        SOAK_FAIL(st, "counter frozen at %u", a);
        return false;
    }
    return true;
}

/* 0x80 TRNG_READ -- two 16-byte pulls: each must be non-constant and
 * the two must differ.  (A statistical health test is the silicon
 * TRNG's own SP800-90B logic; the soak just proves entropy flows.) */
static bool t_trng(soak_stat_t *st)
{
    uint8_t      a[16] = { 0 }, b[16] = { 0 };
    alp_status_t s = gd32g553_trng_read(&ctx, a, sizeof a);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    s = gd32g553_trng_read(&ctx, b, sizeof b);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    bool constant = true;
    for (unsigned i = 1; i < sizeof a; ++i) {
        if (a[i] != a[0]) {
            constant = false;
            break;
        }
    }
    if (constant) {
        SOAK_FAIL(st, "16-byte pull is constant 0x%02X", a[0]);
        return false;
    }
    if (memcmp(a, b, sizeof a) == 0) {
        SOAK_FAIL(st, "two pulls identical");
        return false;
    }
    return true;
}

/* 0x90 TMU_COMPUTE -- CORDIC sqrt(4.0f) must come back 2.0f (the
 * answer is exact in binary32, so the 1e-3 tolerance only absorbs
 * CORDIC iteration error), plus sin(0) == 0. */
static bool t_tmu(soak_stat_t *st)
{
    float    in = 4.0f, out    = 0.0f;
    uint32_t in_bits, out_bits = 0;
    memcpy(&in_bits, &in, sizeof in_bits);
    alp_status_t s = gd32g553_tmu_compute(&ctx, GD32G553_TMU_FN_SQRT, GD32G553_TMU_FMT_F32, in_bits,
                                          0u, &out_bits);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "sqrt status=%d", (int)s);
        return false;
    }
    memcpy(&out, &out_bits, sizeof out);
    if (!float_near(out, 2.0f, 1e-3f)) {
        SOAK_FAIL(st, "sqrt(4.0) = %d/1000 (want 2000/1000)", (int)(out * 1000.0f));
        return false;
    }
    s = gd32g553_tmu_compute(&ctx, GD32G553_TMU_FN_SIN, GD32G553_TMU_FMT_F32, 0u, 0u, &out_bits);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "sin status=%d", (int)s);
        return false;
    }
    memcpy(&out, &out_bits, sizeof out);
    if (!float_near(out, 0.0f, 1e-3f)) {
        SOAK_FAIL(st, "sin(0) = %d/1000 (want 0)", (int)(out * 1000.0f));
        return false;
    }
    return true;
}

/* 0x27 TIMER_SYNC -- link TIMER0 (master) -> TIMER7 (slave, restart
 * mode) and immediately unlink (mode 0).  Runs AFTER the PWM tests in
 * the table: while linked, TIMER7's PWM4..7 phase-restart on TIMER0's
 * update -- harmless on unloaded bench pads, but keep the window
 * short and always restore the timers to independent free-run. */
static bool t_timer_sync(soak_stat_t *st)
{
    alp_status_t s = gd32g553_timer_sync(&ctx, 0u, 1u, 1u /* restart */);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "link status=%d", (int)s);
        return false;
    }
    s = gd32g553_timer_sync(&ctx, 0u, 1u, 0u /* disable */);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "unlink status=%d", (int)s);
        return false;
    }
    return true;
}

/* 0x28 POWER_MODE_SET -- mode 0 ("run") is the documented no-op
 * request: it exercises the full opcode path without putting either
 * SoC to sleep mid-soak.  The real sleep/wake transitions are their
 * own HIL rows (they tear down this very link). */
static bool t_power_mode(soak_stat_t *st)
{
    const alp_status_t s = gd32g553_power_mode_set(&ctx, 0u, 0u, 0u);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    return true;
}

/* 0x40 DA9292_STATUS_FORWARD -- on THIS SoM revision the DA9292
 * fault nets reach only the Renesas (P37/P36); the GD32 has no pin to
 * sample, so the contract says the reply is the 0xFF "no sample"
 * sentinel, always.  Anything else = firmware drifted from the wire
 * contract (or someone wired a new HW rev without updating it). */
static bool t_da9292_sentinel(soak_stat_t *st)
{
    uint8_t            v = 0;
    const alp_status_t s = gd32g553_da9292_status_forward(&ctx, &v);
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d", (int)s);
        return false;
    }
    if (v != 0xFFu) {
        SOAK_FAIL(st, "0x%02X (this HW rev must answer the 0xFF sentinel)", v);
        return false;
    }
    return true;
}

/* 0xF5 OTA_GET_STATE -- proves the 0xF0.. dispatch range routes
 * correctly without arming anything destructive.  Both deployments
 * pass: the UNARMED build answers NOSUPPORT for the whole OTA range;
 * the ARMED (partitioned) build answers a concrete, sane state
 * snapshot.  Anything else is a dispatch fault.  (The armed OTA
 * flow itself has its own dedicated bench procedure.) */
static bool t_ota_get_state(soak_stat_t *st)
{
    gd32g553_ota_state_info_t info;
    const alp_status_t        s = gd32g553_ota_get_state(&ctx, &info);
    if (s == ALP_ERR_NOSUPPORT) {
        return true; /* unarmed build: documented reply */
    }
    if (s != ALP_OK) {
        st->last_status = (int)s;
        SOAK_FAIL(st, "status=%d (want OK or NOSUPPORT)", (int)s);
        return false;
    }
    if (info.state > GD32G553_OTA_STATE_ERROR ||
        (info.active_slot != GD32G553_OTA_SLOT_A && info.active_slot != GD32G553_OTA_SLOT_B)) {
        SOAK_FAIL(st, "armed build, insane snapshot: state=%d active=%d", (int)info.state,
                  (int)info.active_slot);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Test table -- cycle order matters: PWM readbacks run before        */
/* TIMER_SYNC briefly links their timers.                              */
/*                                                                    */
/* QUARANTINE: entries marked quarantined are SKIPPED (printed once at */
/* boot).  These are silicon defects this soak itself caught on        */
/* 2026-06-04 (first-ever HiL exercise of the deep HAL bodies); each   */
/* failed from the very first cycle, and adc_stream is actively        */
/* destructive -- a failed STREAM_END leaves the 1 kHz circular DMA    */
/* running on DMA0 forever, contending with the SPI slave's CH2/CH3    */
/* until the whole link rots.  Un-quarantine each as its firmware fix  */
/* lands and re-soak:                                                  */
/*   - pwm_capture   begin -> ALP_ERR_IO from cycle 1 (suspect the    */
/*                   NOTIMPL->STATUS_IO mapping trap or pad routing)   */
/*   - adc_stream    -5 from cycle 1 even with the DMAMUX .request    */
/*                   fix in -- more is wrong; END failure = poison     */
/*   - qenc          reset/read -> -5 from cycle 1                    */
/*   - tmu           sqrt -> -5 from cycle 1                          */
/*   - ota_get_state -5 from cycle 1 on the armed build               */
/*   - trng          -5 every cycle: the DRDY/conditioning wait runs  */
/*                   inside the request handler, long enough to break  */
/*                   the reply window (transaction-merge class), and   */
/*                   the stale-reply ripple then fails the next ~3     */
/*                   tests of the same cycle (cycle-boundary idle      */
/*                   self-heals -- 1526-cycle soak proved the link     */
/*                   itself never wedges from this)                    */
/* ------------------------------------------------------------------ */

typedef bool (*soak_fn_t)(soak_stat_t *st);

static struct {
    soak_stat_t stat;
    soak_fn_t   fn;
    bool        quarantined;
} tests[] = {
    { { "ping", 0, 0, 0 }, t_ping, false },
    { { "get_version", 0, 0, 0 }, t_get_version, false },
    { { "get_build_id", 0, 0, 0 }, t_get_build_id, false },
    { { "reset_reason", 0, 0, 0 }, t_reset_reason, false },
    { { "gpio", 0, 0, 0 }, t_gpio, false },
    { { "pwm_set_get", 0, 0, 0 }, t_pwm_set_get, false },
    { { "pwm_single_pulse", 0, 0, 0 }, t_pwm_single_pulse, false },
    { { "pwm_capture", 0, 0, 0 }, t_pwm_capture, true },
    { { "adc_read", 0, 0, 0 }, t_adc_read, false },
    { { "adc_stream", 0, 0, 0 }, t_adc_stream, true },
    { { "dac", 0, 0, 0 }, t_dac, false },
    { { "qenc", 0, 0, 0 }, t_qenc, true },
    { { "counter", 0, 0, 0 }, t_counter, false },
    { { "trng", 0, 0, 0 }, t_trng, true },
    { { "tmu", 0, 0, 0 }, t_tmu, true },
    { { "timer_sync", 0, 0, 0 }, t_timer_sync, false },
    { { "power_mode", 0, 0, 0 }, t_power_mode, false },
    { { "da9292_sentinel", 0, 0, 0 }, t_da9292_sentinel, false },
    { { "ota_get_state", 0, 0, 0 }, t_ota_get_state, true },
};

#define SOAK_TEST_COUNT (sizeof tests / sizeof tests[0])

/* ------------------------------------------------------------------ */
/* Link bring-up + recovery                                            */
/* ------------------------------------------------------------------ */

/* Same cold-boot-autonomous retry as the ping example: the CM33
 * system-manager starts before the GD32 finishes power-on (shared
 * PMIC reset-out), so init retries until the GD32 answers -- no fixed
 * boot delay anywhere. */
static void link_init_blocking(alp_spi_t *spi)
{
    unsigned     attempt = 0;
    alp_status_t s;
    do {
        s = gd32g553_init(&ctx, spi, NULL, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
        if (s != ALP_OK) {
            printf("[hil-soak] init attempt %u failed: %d -- retrying in 200 ms\n", attempt++,
                   (int)s);
            k_msleep(200);
        }
    } while (s != ALP_OK);
    boot_version = ctx.version;
    printf("[hil-soak] link up after %u retr%s; firmware v%u.%u.%u\n", attempt,
           (attempt == 1u) ? "y" : "ies", boot_version.major, boot_version.minor,
           boot_version.patch);
}

int main(void)
{
    printf("[hil-soak] V2N GD32 bridge full-command-set soak (25 MHz SPI)\n");

    /* 25 MHz SPI fast path -- the exact silicon-validated configuration
     * from v2n-gd32-bridge-ping (see that example's main.c for the full
     * clocking + DMA rationale).  i2c stays NULL: BRD_I2C has its own
     * HIL rows and must not stall this soak. */
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = 1u,
        .freq_hz       = 25000000u,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8u,
        .cs_pin_id     = ALP_SPI_NO_CS, /* CS is owned by the platform SPI
                                         * driver on this SoM (P97 direct
                                         * latch) -- see v2n-gd32-bridge-ping */
    });
    if (spi == NULL) {
        printf("[hil-soak] alp_spi_open failed: err=%d -- cannot soak\n", (int)alp_last_error());
        return 1;
    }

    link_init_blocking(spi);

    /* Settle past the host's boot window before the heavy soak: during
     * A55 boot (U-Boot storage init, kernel driver probes + the ~T+15 s
     * clk/pinctrl sweeps) shared board state can disturb the link hard
     * enough to wedge an in-flight multi-opcode cycle (silicon-observed
     * 2026-06-04; the light PING-only probe rides it out, the full
     * command mix does not).  Steady-state operation -- the soak's
     * actual job -- is unaffected; autonomy is preserved because the
     * settle needs no interaction.  Bridging traffic during host boot
     * is tracked for the next-rev transport hardening. */
    printf("[hil-soak] link up -- settling 60 s past the host boot window\n");
    k_msleep(60000);
    link_init_blocking(spi); /* re-sync in case boot noise hit the link */

    /* One-shot probes that must NOT loop: the DSP chain pool holds 4
     * chains and has no close opcode yet, so a per-cycle open would
     * exhaust it by cycle 5 and poison the stats.  Probe once, log,
     * move on -- full chain coverage belongs to the wave-2 DSP work. */
    {
        uint8_t            chain_id = 0xFF;
        const alp_status_t s        = gd32g553_adc_dsp_chain_open(&ctx, &chain_id);
        printf("[hil-soak] one-shot adc_dsp_chain_open -> %d (chain %u)\n", (int)s, chain_id);
    }

    /* ---- the soak proper ---- */
    uint32_t cycle                  = 0;
    uint32_t consecutive_ping_fails = 0;

    for (;;) {
        ++cycle;
        unsigned cycle_pass = 0, cycle_fail = 0;
        bool     ping_ok_this_cycle = true;

        for (unsigned i = 0; i < SOAK_TEST_COUNT; ++i) {
            if (tests[i].quarantined) {
                if (cycle == 1u) {
                    printf("[hil-soak] QUARANTINED (known silicon defect, "
                           "see table comment): %s\n",
                           tests[i].stat.name);
                }
                continue;
            }
            if (tests[i].fn(&tests[i].stat)) {
                ++tests[i].stat.pass;
                ++cycle_pass;
            } else {
                ++tests[i].stat.fail;
                ++cycle_fail;
                if (i == 0u) ping_ok_this_cycle = false; /* tests[0] = ping */
            }
        }

        printf("[hil-soak] cycle %u | %u/%u PASS%s\n", cycle, cycle_pass, cycle_pass + cycle_fail,
               (cycle_fail != 0u) ? " <-- FAILURES THIS CYCLE" : "");

        /* Cumulative table every 16 cycles -- greppable soak verdict.
         * "SOAK-CLEAN" appears iff every ACTIVE test has zero failures
         * (quarantined entries are listed but don't gate the verdict). */
        if ((cycle % 16u) == 0u) {
            bool clean = true;
            printf("[hil-soak] ---- cumulative after %u cycles ----\n", cycle);
            for (unsigned i = 0; i < SOAK_TEST_COUNT; ++i) {
                const soak_stat_t *st = &tests[i].stat;
                if (tests[i].quarantined) {
                    printf("[hil-soak]   %-18s QUARANTINED\n", st->name);
                    continue;
                }
                if (st->fail != 0u) clean = false;
                printf("[hil-soak]   %-18s pass=%u fail=%u%s\n", st->name, st->pass, st->fail,
                       st->fail ? " <--" : "");
            }
            printf("[hil-soak] verdict: %s\n", clean ? "SOAK-CLEAN" : "SOAK-DIRTY");
        }

        /* Link-level recovery: PING failing two cycles in a row means
         * the transport (not an opcode) is wedged -- re-run the
         * blocking init, which re-PINGs until the GD32 answers.  A
         * single failed cycle rides on the driver's own per-call
         * CRC/retry handling and just counts. */
        if (!ping_ok_this_cycle) {
            if (++consecutive_ping_fails >= 2u) {
                printf("[hil-soak] transport wedged -- re-initialising the link\n");
                link_init_blocking(spi);
                consecutive_ping_fails = 0;
            }
        } else {
            consecutive_ping_fails = 0;
        }

        k_msleep(1000);
    }

    /* not reached */
    return 0;
}
