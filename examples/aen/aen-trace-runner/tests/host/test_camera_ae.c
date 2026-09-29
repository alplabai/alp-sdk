/* tests/host/test_camera_ae.c -- src/vision/camera_ae.c: the OV9281 software
 * AE controller (design sec 2: the sensor has no usable on-chip AEC). */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../src/vision/camera_ae.h"

#define EXP_MIN  0x05
#define EXP_MAX  1071 /* VTS 1096 - 25, the 640x400 mode */
#define GAIN_MAX TR_AE_GAIN_IDX_MAX

/* A synthetic scene: pixel = min(255, reflectance * total / 64), total =
 * lines x gain register -- brightness proportional to exposure, clipped
 * like a real sensor. The left-top corner is a daylight window, 20x the
 * room, saturated at any useful exposure; the player (centre) is a bit
 * darker than the room. */
#define SW 160
#define SH 100
static uint8_t g_img[SW * SH];

static void render(uint32_t total, uint32_t illum_x4)
{
	for (int y = 0; y < SH; y++) {
		for (int x = 0; x < SW; x++) {
			uint32_t r = x < SW / 4 && y < SH / 2 ? 40u : (x >= SW / 3 && x < 2 * SW / 3) ? 3u : 2u;
			uint64_t v = (uint64_t)r * illum_x4 * total / (64u * 4u);

			g_img[y * SW + x] = (uint8_t)(v > 255u ? 255u : v);
		}
	}
}

static uint32_t total_of(const tr_ae_t *ae)
{
	return (uint32_t)ae->exposure * tr_ae_gain_reg_map[ae->gain_idx];
}

/* Runs up to `frames` frames with a one-frame register latency (a write
 * during frame k is first seen by frame k+2). Asserts the total only ever
 * moves one way (no ping-pong), that it settles within the band, and that
 * it then stays put. Returns the number of changes. */
static int converge(tr_ae_t *ae, uint32_t illum_x4, int frames)
{
	uint32_t sensor = total_of(ae), queued = sensor;
	int      dir = 0, changes = 0, last_change = -1;
	uint8_t  mean = 0;

	for (int f = 0; f < frames; f++) {
		render(sensor, illum_x4);
		mean = tr_ae_meter(g_img, SW, SH);

		uint32_t before = total_of(ae);

		if (tr_ae_step(ae, mean, EXP_MIN, EXP_MAX, GAIN_MAX)) {
			int d = total_of(ae) > before ? 1 : -1;

			assert(dir == 0 || d == dir); /* monotonic: never reverses */
			dir = d;
			changes++;
			last_change = f;
			/* per-update change limited to x/÷ 1.25, plus one rounding quantum
			 * (half a line, or one gain step: <= 3 %) */
			uint64_t after = total_of(ae);

			assert(after * 100u <= (uint64_t)before * 125u * 103u / 100u + 0x10u * 100u);
			assert(after * 100u * 103u / 100u + 0x10u * 100u >= (uint64_t)before * 80u);
		}
		assert(ae->exposure >= EXP_MIN && ae->exposure <= EXP_MAX && ae->gain_idx <= GAIN_MAX);
		sensor = queued;
		queued = total_of(ae);
	}
	assert(last_change < frames - 20); /* settled, then held for 20+ frames */
	assert(!ae->hunting);
	assert(mean >= TR_AE_TARGET_MEAN - TR_AE_BAND_OUT &&
	       mean <= TR_AE_TARGET_MEAN + TR_AE_BAND_OUT);
	return changes;
}

int main(void)
{
	/* --- tr_ae_meter(): clip + centre weight --- */
	uint8_t flat[16 * 16];

	for (int i = 0; i < 16 * 16; i++) {
		flat[i] = 100;
	}
	assert(tr_ae_meter(flat, 16, 16) == 100);
	for (int i = 0; i < 16 * 16; i++) {
		flat[i] = 255;
	}
	assert(tr_ae_meter(flat, 16, 16) == TR_AE_CLIP); /* highlights clipped */
	for (int y = 0; y < 16; y++) {
		for (int x = 0; x < 16; x++) {
			bool centre = x >= 4 && x < 12 && y >= 4 && y < 12;

			flat[y * 16 + x] = centre ? 120 : 0;
		}
	}
	/* sampled 4x4: 4 centre samples (weight 3) of 16 -> 1440 / 24 = 60, vs 30 unweighted */
	assert(tr_ae_meter(flat, 16, 16) == 60);

	/* --- deadband with hysteresis --- */
	tr_ae_t ae;

	tr_ae_init(&ae, 0x100, 0);
	assert(!tr_ae_step(&ae, TR_AE_TARGET_MEAN + TR_AE_BAND_OUT, EXP_MIN, EXP_MAX, GAIN_MAX));
	assert(!tr_ae_step(&ae, TR_AE_TARGET_MEAN - TR_AE_BAND_OUT, EXP_MIN, EXP_MAX, GAIN_MAX));
	assert(tr_ae_step(&ae, TR_AE_TARGET_MEAN + TR_AE_BAND_OUT + 1, EXP_MIN, EXP_MAX, GAIN_MAX));
	assert(ae.hunting && ae.settle == TR_AE_SETTLE_FRAMES);
	/* the next frame is still the old exposure: skipped */
	assert(!tr_ae_step(&ae, 250, EXP_MIN, EXP_MAX, GAIN_MAX) && ae.settle == 0);
	/* hunting: inside the outer band but outside the inner one still moves */
	uint16_t e0 = ae.exposure;

	assert(tr_ae_step(&ae, TR_AE_TARGET_MEAN + TR_AE_BAND_IN + 4, EXP_MIN, EXP_MAX, GAIN_MAX));
	assert(ae.exposure < e0);
	ae.settle = 0;
	assert(!tr_ae_step(&ae, TR_AE_TARGET_MEAN + TR_AE_BAND_IN, EXP_MIN, EXP_MAX, GAIN_MAX) &&
	       !ae.hunting);

	/* --- the silicon bang-bang case: a daylight window, from both ends of
	 * the range, then illumination steps x4 and /4 --- */
	tr_ae_init(&ae, EXP_MIN, 0);
	int n = converge(&ae, 4, 80);

	assert(n > 0);
	printf("ae: dark start -> %u lines gain %u after %d updates\n", ae.exposure, ae.gain_idx, n);
	uint32_t settled = total_of(&ae);

	tr_ae_init(&ae, EXP_MAX, (uint8_t)GAIN_MAX);
	n = converge(&ae, 4, 80);
	printf("ae: bright start -> %u lines gain %u after %d updates\n", ae.exposure, ae.gain_idx, n);
	/* both ends settle on the same exposure (within the band) */
	assert(total_of(&ae) * 5u > settled * 4u && total_of(&ae) * 4u < settled * 5u);

	n = converge(&ae, 16, 40); /* 4x brighter */
	assert(n > 0 && total_of(&ae) < settled);
	n = converge(&ae, 1, 60); /* 16x darker */
	assert(n > 0 && total_of(&ae) > settled);
	n = converge(&ae, 4, 40); /* back */
	assert(total_of(&ae) * 5u > settled * 4u && total_of(&ae) * 4u < settled * 5u);

	/* A dark room past the exposure ceiling spills into gain; saturated in
	 * both levers the controller reports no change. */
	tr_ae_init(&ae, EXP_MAX, 0);
	n = 0;
	for (int i = 0; i < 200; i++) {
		ae.settle = 0;
		n += tr_ae_step(&ae, 0, EXP_MIN, EXP_MAX, GAIN_MAX);
	}
	assert(ae.exposure == EXP_MAX && ae.gain_idx == GAIN_MAX && n > 0);
	tr_ae_init(&ae, EXP_MIN, 0);
	assert(!tr_ae_step(&ae, 255, EXP_MIN, EXP_MAX, GAIN_MAX));

	/* --- fix round 11 (VTS-derived ceiling), corrected fix round 13
	 * (maintainer): NO shift -- tr_ae_exposure_max_from_vts() returns
	 * WHOLE LINES directly (vts - margin), the same units chips/ov9281's
	 * own ctrls->exposure.range.max always used. OV9281_EXPOSURE_DEFAULT
	 * (0x2a9 = 681) IS 681 whole lines already, comfortably under VTS
	 * 1096's own 1071-line ceiling -- the "far too dark" finding's real
	 * fix is making this LIVE (per-mode VTS), not rescaling it. Pinned
	 * against every real sensor mode (chips/ov9281's own table,
	 * OV9281_EXP_MAX_OFFSET margin = 25 lines, hp_vision/src/main.c). --- */
	assert(tr_ae_exposure_max_from_vts(1096, 25) == 1096u - 25u); /* 640x400 @ 100 fps */
	assert(tr_ae_exposure_max_from_vts(1096, 25) == 1071u);
	assert(tr_ae_exposure_max_from_vts(1820, 25) == 1820u - 25u); /* 1280x720 @ 50 fps */
	assert(tr_ae_exposure_max_from_vts(910, 25) == 910u - 25u);   /* 1280x800 @ 100 fps */
	/* 0x2a9 (681) is comfortably under every real mode's own ceiling --
	 * confirms the old fixed constant was always a PLAUSIBLE ceiling
	 * (conservative, not per-mode-live, but never the "42 lines" round 11
	 * mistakenly diagnosed). */
	assert(0x2A9u < 1096u - 25u);
	assert(0x2A9u < 910u - 25u);
	/* Degenerate: a margin >= vts (a corrupt or unsupported read) clamps to
	 * 0, not underflow/wraparound. */
	assert(tr_ae_exposure_max_from_vts(20, 25) == 0u);
	assert(tr_ae_exposure_max_from_vts(25, 25) == 0u);
	/* No overflow risk any more: vts is itself uint16_t, so vts - margin
	 * can never exceed 0xFFFF -- unlike round 12's <<4, which could. */
	assert(tr_ae_exposure_max_from_vts(65535, 25) == 65535u - 25u);

	/* --- tr_ae_exposure_regs() -- the H/M/L register split hp_vision
	 * writes DIRECTLY over I2C (fix round 12 finding A), bypassing
	 * chips/ov9281's own ov9281_set_ctrl() clamp (whole lines, always
	 * correct -- NOT the bug, fix round 13's correction). `val` is WHOLE
	 * LINES: pinned against the sensor's OWN reset-default register triple
	 * (chips/ov9281's mode-init table: {0x3500,0x00},{0x3501,0x2a},
	 * {0x3502,0x90}) for exactly 681 lines, and against 1071 lines (VTS
	 * 1096's own real ceiling) for 00/42/f0 -- fix round 12 mistakenly fed
	 * this function 17136 (681*16-scale confusion) for that same ceiling,
	 * silicon-confirmed DARKER (mean 8 vs 16.5, far past the 1096-line
	 * frame) -- see this function's own header comment for the full
	 * derivation of why 1071 lines is correct and 17136 was not. */
	{
		uint8_t h, m, l;

		tr_ae_exposure_regs(
		    681u, &h, &m, &l); /* OV9281_EXPOSURE_DEFAULT, the sensor's own reset value */
		assert(h == 0x00u && m == 0x2Au && l == 0x90u);

		tr_ae_exposure_regs(1071u, &h, &m, &l); /* the 640x400@100fps ceiling, VTS 1096 - 25 */
		assert(h == 0x00u && m == 0x42u && l == 0xF0u);

		tr_ae_exposure_regs(0xFFFFu, &h, &m, &l); /* max representable */
		assert(h == 0x0Fu && m == 0xFFu && l == 0xF0u);

		tr_ae_exposure_regs(0u, &h, &m, &l);
		assert(h == 0u && m == 0u && l == 0u);
	}

	return 0;
}
