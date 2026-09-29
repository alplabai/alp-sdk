/* tests/host/test_snd_verdict.c -- sound/src/snd_verdict.h, the TEST
 * image's loopback PASS/FAIL. The first bench run (2026W36-0002, 2026-09-23)
 * printed PASS on a capture that was all zeros with every I2S write failed;
 * that exact case must FAIL now, and so must its variants. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../sound/src/snd_verdict.h"

static snd_stats_t good(void)
{
	snd_stats_t s;
	memset(&s, 0, sizeof(s));
	s.frames = s.want_frames = 142848u;
	s.played_ms              = 23400u;
	s.want_ms                = 23280u;
	for (unsigned c = 0; c < 2u; c++) {
		s.rms[0][c] = 30.0f, s.bin[0][c] = 0.05f;     /* room noise */
		s.rms[1][c] = 1414.0f, s.bin[1][c] = 1.0e6f;  /* 2000 LSB tone */
		s.rms[2][c] = 500.0f, s.bin[2][c] = 2.0f;     /* music */
	}
	return s;
}

static void expect(const snd_stats_t *s, const char *why)
{
	const char *got = snd_verdict(s);
	if (got == NULL || strcmp(got, why) != 0) {
		printf("test_snd_verdict: want \"%s\", got \"%s\"\n", why, got ? got : "PASS");
		assert(0);
	}
}

int main(void)
{
	snd_stats_t s = good();
	assert(snd_verdict(&s) == NULL);

	/* the bench capture: all zero, 1455 write + 205 mic failures, 187722 ms */
	memset(&s, 0, sizeof(s));
	s.frames = s.want_frames = 142848u;
	s.write_fail = 1455u, s.mic_fail = 205u, s.played_ms = 187722u, s.want_ms = 23280u;
	expect(&s, "I2S3 writes failed (bit clock not running?)");

	/* all zero with no failures counted: no live mic */
	s.write_fail = s.mic_fail = 0u, s.played_ms = 23300u;
	expect(&s, "no live mic (silence window digitally zero)");

	/* each guard on its own: every case below passes all other guards, so
	 * dropping that one guard turns its FAIL into a PASS */
	s = good(), s.write_fail = 3u;
	expect(&s, "I2S3 writes failed (bit clock not running?)");
	s = good(), s.mic_fail = 3u;
	expect(&s, "PDM mic reads failed");
	s = good(), s.frames -= 768u;
	expect(&s, "capture incomplete");
	s = good(), s.played_ms = 30000u;
	expect(&s, "playback slower than real time");
	s = good(), s.rms[0][0] = s.rms[0][1] = 0.0f; /* digitally silent mics, tone + music "loud" */
	expect(&s, "no live mic (silence window digitally zero)");
	s = good(), s.bin[1][0] = s.bin[1][1] = 20.0f, s.rms[1][0] = s.rms[1][1] = 6.0f; /* only the 25 floor */
	expect(&s, "1 kHz tone not heard");
	s = good(), s.bin[0][0] = s.bin[0][1] = 20.0f; /* only tone >= 100 x silence */
	s.bin[1][0] = s.bin[1][1] = 1000.0f, s.rms[1][0] = s.rms[1][1] = 40.0f;
	expect(&s, "1 kHz tone not heard");
	s = good(), s.bin[1][0] = s.bin[1][1] = 1000.0f; /* only the 10%-of-power check (rms 1414) */
	expect(&s, "1 kHz tone not heard");
	s = good(), s.rms[0][0] = s.rms[0][1] = 1.0f, s.rms[2][0] = s.rms[2][1] = 4.0f; /* only the 5 LSB floor */
	expect(&s, "music not heard");
	s = good(), s.rms[2][0] = s.rms[2][1] = 40.0f; /* only music >= 2 x silence (30) */
	expect(&s, "music not heard");
	s = good(), s.rms[0][1] = 0.0f; /* one dead mic, the other hears it: PASS */
	assert(snd_verdict(&s) == NULL);
	s = good(), s.write_fail = 2u, s.mic_fail = 2u; /* small N tolerated */
	assert(snd_verdict(&s) == NULL);

	puts("test_snd_verdict: ok");
	return 0;
}
