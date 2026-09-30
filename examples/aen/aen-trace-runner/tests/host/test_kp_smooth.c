/* tests/host/test_kp_smooth.c -- src/vision/kp_smooth.c, the HP's One-Euro
 * keypoint smoothing, on a synthetic upright (400x640) player with MoveNet-
 * like jitter, judged where it matters -- the intents track.c derives:
 *   1. standing still: keypoint jitter drops to <= 60 % RMS, and no
 *      jump/duck/lane intent fires from noise;
 *   2. a jump, a duck and a lane step each still register within 2 frames
 *      (the smoothing's lag stays bounded), jitter and all, and the
 *      smoothed track settles on the new position within 3 frames;
 *   3. an unsure keypoint is held, not jumped to its unsure decode; one gone
 *      past TR_KS_HOLD_MAX frames, or a stalled stream, re-seeds from raw. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/vision/kp_smooth.h"

#define UW    400
#define UH    640
#define DT_MS 38u /* the HP loop at ~26 Hz: the slowest real frame clock, the most lag */

static uint32_t rng = 12345u;

static int noise(int amp)
{
	rng = rng * 1664525u + 1013904223u;
	return (int)((rng >> 16) % (uint32_t)(2 * amp + 1)) - amp;
}

static tr_pose_t figure(int cx, int top, int feet)
{
	static const struct {
		int8_t  dx;
		uint8_t fy;
	} body[TR_POSE_KP] = {
		{ 0, 7 },    { 4, 5 },   { -4, 5 },   { 8, 6 },    { -8, 6 },    { 25, 20 },
		{ -25, 20 }, { 30, 35 }, { -30, 35 }, { 30, 48 },  { -30, 48 },  { 15, 52 },
		{ -15, 52 }, { 15, 75 }, { -15, 75 }, { 15, 100 }, { -15, 100 },
	};
	tr_pose_t p;

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k] = (tr_kp_t){ (int16_t)(cx + body[k].dx),
			                 (int16_t)(top + (feet - top) * body[k].fy / 100),
			                 200 };
	}
	return p;
}

static tr_pose_t jitter(tr_pose_t p, int amp)
{
	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k].x = (int16_t)(p.kp[k].x + noise(amp));
		p.kp[k].y = (int16_t)(p.kp[k].y + noise(amp));
	}
	return p;
}

/* Calibrate a tracker on a steady, jittery stance through the smoother. */
static void settle(tr_kp_smooth_t *s, tr_track_t *t, tr_pose_t stand, int frames)
{
	tr_kp_smooth_reset(s);
	tr_track_init(t, UW, UH);
	for (int i = 0; i < frames; i++) {
		tr_pose_t p = jitter(stand, 5);

		tr_kp_smooth_step(s, &p, DT_MS);
		if (i == frames / 2) {
			tr_track_calibrate(t, tr_pose_box(&p), UW);
		} else if (i > frames / 2) {
			(void)tr_track_update(t, tr_pose_box(&p));
		}
	}
	assert(t->calibrated);
}

/* Frames from a step (0 = the step frame itself) until `hit` holds. */
static int frames_to(tr_kp_smooth_t *s, tr_track_t *t, tr_pose_t target, int want)
{
	for (int i = 0; i < 10; i++) {
		tr_pose_t   p = jitter(target, 5);
		tr_intent_t in;

		tr_kp_smooth_step(s, &p, DT_MS);
		in = tr_track_update(t, tr_pose_box(&p));
		if ((want == 0 && in.jump) || (want == 1 && in.duck) || (want == 2 && in.lane_delta != 0)) {
			return i;
		}
	}
	return 99;
}

int main(void)
{
	tr_pose_t      stand = figure(UW / 2, 100, 600); /* stance height 500 */
	tr_kp_smooth_t s;
	tr_track_t     t;

	/* 1. standing still */
	{
		double raw2 = 0.0, sm2 = 0.0;
		int    n = 0, intents = 0;

		settle(&s, &t, stand, 40);
		for (int i = 0; i < 300; i++) {
			tr_pose_t p = jitter(stand, 5), q = p;

			tr_kp_smooth_step(&s, &q, DT_MS);
			for (int k = 0; k < TR_POSE_KP; k++) {
				double rx = p.kp[k].x - stand.kp[k].x, ry = p.kp[k].y - stand.kp[k].y;
				double sx = q.kp[k].x - stand.kp[k].x, sy = q.kp[k].y - stand.kp[k].y;

				raw2 += rx * rx + ry * ry;
				sm2 += sx * sx + sy * sy;
				n++;
			}
			tr_intent_t in = tr_track_update(&t, tr_pose_box(&q));

			intents += in.jump || in.duck || in.lane_delta != 0;
		}
		double raw = sqrt(raw2 / n), sm = sqrt(sm2 / n);

		printf("kp_smooth: standing jitter RMS %.2f px raw -> %.2f px smoothed (%.0f %%), %d stray "
		       "intents\n",
		       raw,
		       sm,
		       100.0 * sm / raw,
		       intents);
		assert(sm <= 0.6 * raw && intents == 0);
	}

	/* 2. steps: registered within 2 frames, settled within 5 */
	{
		int f;

		settle(&s, &t, stand, 40);
		f = frames_to(
		    &s, &t, figure(UW / 2, 100 - 100, 600 - 100), 0); /* jump: body up 20 % of stance */
		printf("kp_smooth: jump registers %d frame(s) after the step\n", f);
		assert(f <= 1);

		settle(&s, &t, stand, 40);
		f = frames_to(
		    &s, &t, figure(UW / 2, 100 + 100, 600 + 100), 1); /* duck: torso 100 px down */
		printf("kp_smooth: duck registers %d frame(s) after the step\n", f);
		assert(f <= 1);

		settle(&s, &t, stand, 40);
		f = frames_to(&s, &t, figure(60, 100, 600), 2); /* lane: torso into the left third */
		printf("kp_smooth: lane step registers %d frame(s) after the step\n", f);
		assert(f <= 1);

		/* lag: noise-free, the nose within 3 px of a 100 px step by the third frame */
		tr_pose_t up = figure(UW / 2, 0, 500), p;

		tr_kp_smooth_reset(&s);
		for (int i = 0; i < 20; i++) {
			p = stand;
			tr_kp_smooth_step(&s, &p, DT_MS);
		}
		for (int i = 0; i < 3; i++) {
			p = up;
			tr_kp_smooth_step(&s, &p, DT_MS);
			printf("kp_smooth: step frame %d nose y %d (target %d)\n",
			       i,
			       p.kp[TR_KP_NOSE].y,
			       up.kp[TR_KP_NOSE].y);
		}
		assert(abs(p.kp[TR_KP_NOSE].y - up.kp[TR_KP_NOSE].y) <= 3);
	}

	/* 3. unsure keypoints held; long gone or stalled -> re-seed */
	{
		tr_pose_t p;

		tr_kp_smooth_reset(&s);
		for (int i = 0; i < 20; i++) {
			p = stand;
			tr_kp_smooth_step(&s, &p, DT_MS);
		}
		for (int i = 0; i < 3; i++) {
			p = stand;
			p.kp[TR_KP_LSHO] =
			    (tr_kp_t){ 0, 0, TR_POSE_KP_MIN - 1 }; /* an unsure decode, far off */
			tr_kp_smooth_step(&s, &p, DT_MS);
			assert(p.kp[TR_KP_LSHO].x == stand.kp[TR_KP_LSHO].x &&
			       p.kp[TR_KP_LSHO].y == stand.kp[TR_KP_LSHO].y);
			assert(p.kp[TR_KP_LSHO].score ==
			       TR_POSE_KP_MIN - 1); /* still unsure to every consumer */
		}
		p = stand;
		tr_kp_smooth_step(&s, &p, DT_MS); /* back: no jump */
		assert(p.kp[TR_KP_LSHO].x == stand.kp[TR_KP_LSHO].x &&
		       p.kp[TR_KP_LSHO].y == stand.kp[TR_KP_LSHO].y);

		for (int i = 0; i <= TR_KS_HOLD_MAX; i++) {
			p                = stand;
			p.kp[TR_KP_LSHO] = (tr_kp_t){ 0, 0, 10 };
			tr_kp_smooth_step(&s, &p, DT_MS);
		}
		assert(p.kp[TR_KP_LSHO].x == 0 && p.kp[TR_KP_LSHO].y == 0); /* gone: no longer held */
		p                  = stand;
		p.kp[TR_KP_LSHO].x = 300;
		tr_kp_smooth_step(&s, &p, DT_MS);
		assert(p.kp[TR_KP_LSHO].x ==
		       300); /* re-seeded from raw, not dragged from the old position */

		p         = stand;
		p.kp[0].y = 400;
		tr_kp_smooth_step(&s, &p, TR_KS_DT_MAX + 1u); /* stalled stream */
		assert(p.kp[0].y == 400);
		printf("kp_smooth: unsure held, gone re-seeded, stall re-seeded\n");
	}
	return 0;
}
