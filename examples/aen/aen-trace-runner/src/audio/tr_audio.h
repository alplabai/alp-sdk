/* src/audio/tr_audio.h -- Trace Runner's synth: a synthwave music loop plus
 * the game's sound effects, rendered to mono S16 at TR_AUDIO_RATE.
 *
 * Integer only (no float, no libm), so the same event sequence renders the
 * same samples bit for bit on the host, the M55-HE and the M55-HP -- the
 * host test pins that with a CRC. One global instance: the HP firmware has
 * exactly one speaker pair to feed.
 *
 *   tr_audio_init(seed);
 *   tr_audio_event(TR_AEV_MUSIC, TR_MUSIC_PLAY);    // kinds: src/ipc/tr_aring.h
 *   for (;;) { pop events -> tr_audio_event(); tr_audio_render(block, n); }
 *
 * Events take effect at the start of the next rendered sample, so their
 * timing resolution is the caller's block length (256 frames = 16 ms at
 * 16 kHz).
 */
#ifndef TR_AUDIO_H
#define TR_AUDIO_H

#include <stdint.h>

#include "../ipc/tr_aring.h" /* TR_AEV_* / TR_MUSIC_* -- the wire event kinds */

/* 16 kHz: the rate the I2S3 -> TAS2563 path was proven audible at on the
 * reworked EVK (bench PROBE_LISTEN/PROBE_MELODY). Must be a multiple of 20
 * so a 100 BPM sixteenth is a whole number of samples. */
#ifndef TR_AUDIO_RATE
#define TR_AUDIO_RATE 16000u
#endif

/* TR_AUDIO_V2 (default 1): the 2026-09-23 bench fixes -- band-limited
 * (polyBLEP) square/saw, a soft master limiter instead of a hard clamp,
 * lower crash/debris/zap levels, and 48 kHz support. 0 renders the first
 * version bit for bit (golden 0xDBF70C58), kept for bench A/B only. */
#ifndef TR_AUDIO_V2
#define TR_AUDIO_V2 1
#endif
/* TR_AUDIO_V3 (default 1, needs V2): the small-speaker voicing from the
 * 2026W36-0002 loopback (2026-09-23: the EVK speakers only play cleanly from
 * ~450-500 Hz up and rattle into harmonics below). A 4th-order 450 Hz
 * high-pass on the master bus, every sound voiced from ~500 Hz up (bass as
 * an upper-harmonic stack, kick/crash punch from >= 600 Hz, footstep a
 * click), and the master raised to put the lost energy back where the
 * speakers play. 0 = V2. */
#ifndef TR_AUDIO_V3
#define TR_AUDIO_V3 TR_AUDIO_V2
#endif
_Static_assert(!TR_AUDIO_V3 || TR_AUDIO_V2, "TR_AUDIO_V3 builds on TR_AUDIO_V2");

/* 16 kHz, or 48 kHz with V2: the rates the I2S3 bit clock divides exactly
 * from 76.8 MHz (76.8 MHz / (64 x rate) = 75 or 25; 32 kHz gives 37.5). */
_Static_assert(TR_AUDIO_RATE == 16000u || (TR_AUDIO_V2 && TR_AUDIO_RATE == 48000u),
               "TR_AUDIO_RATE: 16000, or 48000 with TR_AUDIO_V2");

#define TR_AUDIO_BPM        100u
#define TR_AUDIO_STEP       (TR_AUDIO_RATE * 3u / 20u) /* samples per sixteenth at 100 BPM */
#define TR_AUDIO_SONG_BARS  16u
#define TR_AUDIO_SONG_STEPS (TR_AUDIO_SONG_BARS * 16u) /* 38.4 s loop */

void tr_audio_init(uint32_t seed);
/* Returns the event's serial (1, 2, ... since init), see tr_audio_event_voices(). */
uint32_t tr_audio_event(uint8_t kind, uint8_t param);
void     tr_audio_render(int16_t *buf, unsigned n);

/* Diagnostics (host tests, bench counters). */
uint32_t tr_audio_clipped(void);                 /* samples hard-clamped since init */
uint32_t tr_audio_limited(void);                 /* V2: samples the soft limiter bent since init */
unsigned tr_audio_event_voices(uint32_t serial); /* SFX voices still owned by that event */
void     tr_audio_seek(uint32_t step);           /* jump the music to a sixteenth (bench scripts) */

/* The V3 master high-pass: 4th-order Butterworth (two biquads) at
 * TR_AUDIO_HPF_HZ, Q28 coefficients, int64 accumulate -- exposed for the
 * host test. 450 Hz: the 2026W36-0002 loopback put the speakers' usable low end at
 * ~450-500 Hz (THD at -6 dBFS: 1 kHz -42..-49 dB, 500 Hz -20, 300 Hz -7..+1,
 * 200 Hz +26..+30). */
#define TR_AUDIO_HPF_HZ 450u
typedef struct {
	int32_t x1, x2, y1, y2; /* y in Q12 */
} tr_biquad_t;
typedef struct {
	tr_biquad_t s[2];
} tr_hpf_t;
int32_t  tr_audio_hpf(tr_hpf_t *st, int32_t x);
unsigned tr_audio_sfx_active(void); /* SFX voices still sounding or scheduled */
uint32_t tr_audio_song_step(void);  /* music position, 0 .. TR_AUDIO_SONG_STEPS-1 */

#endif /* TR_AUDIO_H */
