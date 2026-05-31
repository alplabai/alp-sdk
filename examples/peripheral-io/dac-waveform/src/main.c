/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * dac-waveform -- generate a sine wave on BOARD_DAC0.
 *
 * Walks a 32-point sine lookup table at a configurable sample rate,
 * writing each sample to the DAC via alp_dac_write_mv.  The
 * staircase output approximates a sine at the requested frequency
 * (32 samples per cycle -> 32-step staircase).  Pop a DSO probe on
 * the ANA_OUT0 pad to see it.
 *
 * Runs on both EVKs: the E1M EVK drives the Alif Ensemble's native
 * 2-channel DAC; the E1M-X EVK drives the V2N's two DAC channels
 * through the on-module GD32G553 bridge.  BOARD_DAC0 (from
 * <alp/board.h>) resolves to the selected board's DAC0 pad.
 *
 * Default waveform: 100 Hz sine, 1.65 V mean, 1.65 V amplitude
 * (full-range on a 3.3 V reference).  Override SINE_FREQ_HZ +
 * SINE_DC_OFFSET_MV + SINE_AMPLITUDE_MV to match your application.
 *
 * What success looks like:
 *
 *   [dac] open BOARD_DAC0 (initial 1650 mV)
 *   [dac] generating sine: freq=100 Hz, mean=1650 mV, ampl=1650 mV
 *   [dac] cycle 0: peak=3299 mV trough=0 mV
 *   [dac] cycle 1: peak=3299 mV trough=0 mV
 *   ...
 *   [dac] done
 *
 * Scope view: 100 Hz sine on ANA_OUT0, swinging from ~0 V to ~3.3 V.
 * Both converters are 12-bit (~0.8 mV/LSB on a 3.3 V reference):
 * on E1M the Alif native DAC; on E1M-X the GD32-bridged DAC.  The
 * visible staircase is barely perceptible without a high-bandwidth scope.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/adc.h"               /* alp_dac_* lives in adc.h */
#include "alp/board.h"             /* BOARD_DAC0 -> the selected EVK's DAC0 pad */

/* ------------------------------------------------------------------
 * Waveform parameters.  Tweak these for your application.
 * ------------------------------------------------------------------ */

/* Sine sample count per cycle.  32 is a comfortable trade-off:
 * enough resolution that the staircase steps aren't audible on an
 * audio DAC, few enough that the sample-rate maths is easy on a
 * Cortex-M33 without hardware FP. */
#define SINE_SAMPLES 32u

/* Output frequency.  100 Hz is mains-frequency-territory -- visible
 * on any decent scope's timebase + easy to verify with a DMM in AC
 * mode (which integrates the RMS value).
 *
 * SINE_SAMPLE_RATE = SINE_FREQ_HZ * SINE_SAMPLES.
 * At 100 Hz * 32 samples we write 3200 samples / second --
 * comfortably within both the Alif native DAC's update rate and the
 * GD32 bridge's 32-sample-per-batch ceiling on E1M-X / V2N. */
#define SINE_FREQ_HZ 100u

/* DC bias of the sine (centre voltage).  1650 mV puts the waveform
 * centred on the rail mid-point for a 3.3 V supply -- maximum
 * amplitude without clipping. */
#define SINE_DC_OFFSET_MV 1650u

/* Peak amplitude.  1650 mV combined with the 1650 mV DC offset
 * gives a 0..3300 mV swing -- full-range output. */
#define SINE_AMPLITUDE_MV 1650u

/* Number of full cycles to generate before exiting.  Capped so the
 * native_sim build doesn't stall the twister harness; real firmware
 * would loop forever (or until a control loop deactivates the
 * generator). */
#define CYCLES_TO_GENERATE 3u

/* ------------------------------------------------------------------
 * Sine lookup table.
 *
 * Pre-computed Q15 samples covering one full cycle.  Stored as
 * int16_t so multiplication with the amplitude fits in int32_t
 * (no overflow) and the per-sample math stays integer-only on
 * Cortex-M without an FPU.
 *
 * Generated offline (Python):
 *   import math
 *   for i in range(32):
 *       v = math.sin(2 * math.pi * i / 32)
 *       print(int(round(v * 32767)))
 * ------------------------------------------------------------------ */
static const int16_t SINE_LUT_Q15[SINE_SAMPLES] = {
        0,   6393,  12539,  18204,  23170,  27245,  30273,  32137,
    32767,  32137,  30273,  27245,  23170,  18204,  12539,   6393,
        0,  -6393, -12539, -18204, -23170, -27245, -30273, -32137,
   -32767, -32137, -30273, -27245, -23170, -18204, -12539,  -6393,
};

/* ------------------------------------------------------------------
 * Convert a Q15 sine sample (-32768..32767) to a millivolt setpoint
 * around the configured DC offset.  Integer-only; result is clamped
 * to [0, UINT16_MAX] so the DAC backend's saturate-to-rail handling
 * never sees an out-of-range request.
 * ------------------------------------------------------------------ */
static uint16_t lut_to_mv(int16_t q15)
{
    /* mv_offset = (q15 * amplitude_mv) / 32768
     * Use a 32-bit intermediate so the multiplication doesn't
     * overflow on Cortex-M.  q15 is signed; the cast to int32_t
     * sign-extends correctly. */
    int32_t mv_offset = ((int32_t)q15 * (int32_t)SINE_AMPLITUDE_MV) / 32768;
    int32_t mv        = (int32_t)SINE_DC_OFFSET_MV + mv_offset;
    if (mv < 0) mv = 0;
    if (mv > 0xFFFF) mv = 0xFFFF;
    return (uint16_t)mv;
}

int main(void) {
    printf("[dac] open BOARD_DAC0 (initial %u mV)\n", SINE_DC_OFFSET_MV);

    /* Open the DAC at the centre voltage.  The wrapper rounds to
     * the converter's hardware-achievable resolution.
     *
     * channel_id = BOARD_DAC0 -> the selected board's DAC0 pad
     * (E1M_DAC0 on E1M / Alif; E1M_X_DAC0 -> GD32 PA4 on E1M-X / V2N). */
    alp_dac_t *dac = alp_dac_open(&(alp_dac_config_t){
        .channel_id = BOARD_DAC0,
        .initial_mv = SINE_DC_OFFSET_MV,
    });
    if (dac == NULL) {
        /* Likely causes:
         *   * ALP_ERR_NOT_READY -- DAC backend not yet initialised
         *     (e.g. GD32 firmware not running on E1M-X / V2N, or
         *     the Alif DAC clock not enabled on E1M / AEN).
         *   * ALP_ERR_NOSUPPORT -- the targeted SoM SKU has no DAC
         *     backend compiled in (e.g. a future SoM variant with
         *     no DAC silicon and no bridge).
         *   * ALP_ERR_OUT_OF_RANGE -- channel_id out of range for
         *     this board's DAC count.
         *   * On native_sim there's no DAC controller; open
         *     returns NULL with NOT_READY. */
        printf("[dac] open failed: alp_last_error=%d "
               "(NOT_READY on native_sim; DAC backend not ready on real hardware)\n",
               (int)alp_last_error());
        printf("[dac] done\n");
        return 0;
    }

    printf("[dac] generating sine: freq=%u Hz, mean=%u mV, ampl=%u mV\n",
           SINE_FREQ_HZ, SINE_DC_OFFSET_MV, SINE_AMPLITUDE_MV);

    /* Per-sample delay.  SINE_SAMPLES * delay_us = 1 / freq_hz.
     *
     *   delay_us = 1e6 / (SINE_FREQ_HZ * SINE_SAMPLES)
     *
     * At 100 Hz * 32 samples = 3200 Hz sample rate -> 312.5 us /
     * sample.  alp_delay_us takes an integer micro count; round
     * DOWN to keep the worst-case frequency above target. */
    const uint32_t sample_delay_us =
        (uint32_t)(1000000u / (SINE_FREQ_HZ * SINE_SAMPLES));

    /* Generate CYCLES_TO_GENERATE complete cycles, then exit.
     * Real firmware would loop forever (or until a control event
     * trips a flag). */
    for (uint32_t cycle = 0; cycle < CYCLES_TO_GENERATE; cycle++) {
        uint16_t peak = 0;
        uint16_t trough = 0xFFFF;

        /* Walk the LUT once per cycle.  alp_dac_write_mv blocks
         * until the new setpoint is in the converter's data register.
         * On E1M-X / V2N the dispatch goes through the GD32 bridge
         * (~10-100 us); on E1M / Alif the native DAC register write
         * is sub-microsecond. */
        for (uint32_t i = 0; i < SINE_SAMPLES; i++) {
            uint16_t mv = lut_to_mv(SINE_LUT_Q15[i]);
            alp_status_t s = alp_dac_write_mv(dac, mv);
            if (s != ALP_OK) {
                /* Write failures are rare; on E1M-X / V2N usually
                 * a transient supervisor-busy.  Log and continue;
                 * the next sample likely succeeds. */
                printf("[dac] write_mv(%u) -> %d\n", mv, (int)s);
                break;
            }
            if (mv > peak)   peak   = mv;
            if (mv < trough) trough = mv;
            alp_delay_us(sample_delay_us);
        }
        printf("[dac] cycle %u: peak=%u mV trough=%u mV\n",
               cycle, peak, trough);
    }

    /* Park the DAC at mid-rail before closing -- some downstream
     * circuits don't like an abrupt drop to 0 V.  Then close to
     * release the handle.  Note alp_dac_close doesn't power down
     * the converter; the output stays at the last-programmed
     * level until the next open() reprograms it. */
    (void)alp_dac_write_mv(dac, SINE_DC_OFFSET_MV);
    alp_dac_close(dac);
    printf("[dac] done\n");
    return 0;
}
