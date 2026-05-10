/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * audio-loopback -- mic-in to speaker-out, the simplest end-to-end
 * use of <alp/audio.h>.  Hand-written reference for the v0.2
 * audio surface; lives next to gpio-button-led, i2c-scanner,
 * i2s-tone, etc.
 *
 * What it does
 *   1. open  a PDM mic on ALP_E1M_PDM0 (16 kHz mono, S16)
 *   2. open  an I2S DAC on ALP_E1M_I2S0 (same rate / channels)
 *   3. start both, then in a tight loop: read a block from the
 *      mic, write it to the DAC.  The wrapper runs the ALP
 *      DSP chain (DC-block in v0.2) inside alp_audio_in_read,
 *      so the loop here stays trivial.
 *   4. stop / close both, print "[audio] done" so twister's
 *      console harness can see the example completed.
 *
 * On native_sim there are no PDM or I2S devices, so the open()
 * calls return NULL with NOSUPPORT and the loop is skipped.
 * The ` [audio] done` line still hits, which is what the
 * twister scenario asserts on.
 *
 * On a real E1M-AEN board (with the EVK overlay enabled and
 * CONFIG_AUDIO_DMIC=y / CONFIG_I2S=y), the loop runs ~1 second
 * of audio (50 blocks * 256 frames @ 16 kHz) before tearing down.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/audio.h"
#include "alp/e1m_pinout.h"

/* Loop budget -- 50 blocks of 256 frames @ 16 kHz = ~0.8 s.
 * Long enough to hear a "loopback works" cue on a real board,
 * short enough that the example terminates and twister's
 * console harness sees [audio] done within the build timeout. */
#define BLOCKS 50
#define FRAMES 256
#define SR_HZ 16000
#define CHANS 1

/* Stack-friendly scratch buffer: 256 frames * 1 ch * 2 B = 512 B. */
static int16_t g_pcm[FRAMES * CHANS];

int            main(void)
{
    printf("[audio] audio-loopback v0.2 reference -- mic -> DSP -> DAC\n");

    alp_audio_config_t cfg = {
        .peripheral_id    = ALP_E1M_PDM0,
        .sample_rate_hz   = SR_HZ,
        .channels         = CHANS,
        .format           = ALP_AUDIO_FMT_S16_LE,
        .frames_per_block = FRAMES,
    };

    alp_audio_in_t *mic = alp_audio_in_open(&cfg);
    if (mic == NULL) {
        printf("[audio]   alp_audio_in_open               skip (no DMIC, last_err=%d)\n",
               (int)alp_last_error());
        goto done;
    }
    printf("[audio]   alp_audio_in_open(PDM0)         ok\n");

    cfg.peripheral_id    = ALP_E1M_I2S0;
    alp_audio_out_t *spk = alp_audio_out_open(&cfg);
    if (spk == NULL) {
        printf("[audio]   alp_audio_out_open              skip (no I2S, last_err=%d)\n",
               (int)alp_last_error());
        alp_audio_in_close(mic);
        goto done;
    }
    printf("[audio]   alp_audio_out_open(I2S0)        ok\n");

    /* Set software volume to ~60 % (0x9A in Q8 of 0x100 = ~60 %).
     * Real apps would also drive the codec's gain pin if exposed. */
    (void)alp_audio_out_set_volume(spk, 0x9A);

    if (alp_audio_in_start(mic) != ALP_OK) {
        printf("[audio]   alp_audio_in_start              fail\n");
        goto teardown;
    }
    if (alp_audio_out_start(spk) != ALP_OK) {
        printf("[audio]   alp_audio_out_start             fail\n");
        goto teardown;
    }
    printf("[audio]   streaming %d blocks of %d frames @ %d Hz\n", BLOCKS, FRAMES, SR_HZ);

    /* The actual loop -- read, then write.  The wrapper applies
     * the DC-block + software volume internally; the user code
     * stays this small. */
    for (int b = 0; b < BLOCKS; ++b) {
        size_t got = 0;
        if (alp_audio_in_read(mic, g_pcm, FRAMES, &got, 1000) != ALP_OK || got == 0) {
            break;
        }
        size_t pushed = 0;
        (void)alp_audio_out_write(spk, g_pcm, got, &pushed, 1000);
    }

    (void)alp_audio_out_stop(spk);
    (void)alp_audio_in_stop(mic);
    printf("[audio]   loopback complete\n");

teardown:
    alp_audio_out_close(spk);
    alp_audio_in_close(mic);

done:
    printf("[audio] done\n");
    return 0;
}
