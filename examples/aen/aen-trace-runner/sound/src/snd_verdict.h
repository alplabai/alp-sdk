/* sound/src/snd_verdict.h -- PASS/FAIL rule of the TEST image's PDM-mic
 * loopback, pure C so the host test (tests/host/test_snd_verdict.c) can feed
 * it the capture that fooled the first version: every sample 0, every I2S
 * write failed, and it still printed PASS because 0 >= 100 * 0.
 * tools/audio_spectro.py applies the same rule to a capture dump.
 *
 * Windows: 0 silence (stream running, zeros), 1 = 1 kHz tone, 2 = music+SFX.
 * rms = AC RMS per mic, bin = normalised 1 kHz Goertzel power (A^2/4 for a
 * sine of amplitude A), both in S16 LSB units.
 */
#ifndef SND_VERDICT_H
#define SND_VERDICT_H

#include <stdint.h>

#define SND_MAX_FAILS   2u    /* write or mic-read failures tolerated per run */
#define SND_LIVE_RMS    0.5f  /* a working PDM mic is never digitally silent */
#define SND_TONE_FLOOR  25.0f /* 1 kHz bin >= amplitude 10 LSB */
#define SND_MUSIC_FLOOR 5.0f  /* music window AC RMS in LSB */

typedef struct {
	float    rms[3][2], bin[3][2];
	uint32_t frames, want_frames;
	uint32_t write_fail, mic_fail;
	uint32_t played_ms, want_ms;
} snd_stats_t;

/* NULL = PASS, else the first reason it failed. */
static inline const char *snd_verdict(const snd_stats_t *s)
{
	if (s->write_fail > SND_MAX_FAILS)
		return "I2S3 writes failed (bit clock not running?)";
	if (s->mic_fail > SND_MAX_FAILS)
		return "PDM mic reads failed";
	if (s->frames != s->want_frames)
		return "capture incomplete";
	if (s->played_ms > s->want_ms + s->want_ms / 10u)
		return "playback slower than real time";

	unsigned live = 0, tone = 0, music = 0;
	for (unsigned c = 0; c < 2u; c++) {
		if (!(s->rms[0][c] >= SND_LIVE_RMS))
			continue; /* dead or all-zero channel proves nothing */
		live++;
		float b0 = s->bin[0][c], b1 = s->bin[1][c], r1 = s->rms[1][c];
		if (b1 >= SND_TONE_FLOOR && b1 >= 100.0f * b0 && 2.0f * b1 >= 0.1f * r1 * r1)
			tone++;
		if (s->rms[2][c] >= SND_MUSIC_FLOOR && s->rms[2][c] >= 2.0f * s->rms[0][c])
			music++;
	}
	if (live == 0u)
		return "no live mic (silence window digitally zero)";
	if (tone == 0u)
		return "1 kHz tone not heard";
	if (music == 0u)
		return "music not heard";
	return 0;
}

#endif /* SND_VERDICT_H */
