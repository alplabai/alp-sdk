/* tests/host/test_pose.c -- pose -> box, stillness calibration, and the
 * intents they drive through track.c: arm-raise lane change and jump, duck,
 * no-person noise. Synthetic poses; test_pose_clip.c replays a real MoveNet trace. */
#include <assert.h>
#include <stdlib.h>

#include "../../src/vision/pose.h"

#define FW 640
#define FH 400

/* A standing figure: head top `top`, feet `feet`, centred on cx. */
static tr_pose_t figure(int cx, int top, int feet, uint8_t score)
{
	int       h = feet - top;
	tr_pose_t p;
	static const struct {
		int8_t  dx;
		uint8_t fy; /* % of height below the head top */
	} body[TR_POSE_KP] = {
		{ 0, 7 },    { 4, 5 },   { -4, 5 },   { 8, 6 },    { -8, 6 },    { 25, 20 },
		{ -25, 20 }, { 30, 35 }, { -30, 35 }, { 30, 48 },  { -30, 48 },  { 15, 52 },
		{ -15, 52 }, { 15, 75 }, { -15, 75 }, { 15, 100 }, { -15, 100 },
	};

	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k].x     = (int16_t)(cx + body[k].dx);
		p.kp[k].y     = (int16_t)(top + h * body[k].fy / 100);
		p.kp[k].score = score;
	}
	return p;
}

static tr_pose_t jitter(tr_pose_t p, int amp)
{
	for (int k = 0; k < TR_POSE_KP; k++) {
		p.kp[k].x = (int16_t)(p.kp[k].x + (rand() % (2 * amp + 1)) - amp);
		p.kp[k].y = (int16_t)(p.kp[k].y + (rand() % (2 * amp + 1)) - amp);
	}
	return p;
}

static void box_geometry(void)
{
	tr_pose_t p = figure(320, 80, 380, 150);
	tr_box_t  b = tr_pose_box(&p);

	/* The torso box (track.h): shoulders at 20 %, hips at 52 % of 300. */
	assert(b.valid && b.confidence == 150);
	assert(b.x == 320 - 25 && b.w == 50);      /* torso centre x, shoulder width */
	assert(b.y == 80 + 60 && b.h == 156 - 60); /* shoulder-mid y, torso length */

	/* Arms up, a head turn, legs out of frame: none of it moves the box. */
	tr_pose_t up            = p;
	up.kp[TR_KP_LWRI]       = (tr_kp_t){ 420, 20, 200 };
	up.kp[TR_KP_RWRI]       = (tr_kp_t){ 220, 20, 200 };
	up.kp[TR_KP_NOSE]       = (tr_kp_t){ 360, 70, 200 };
	up.kp[TR_KP_LANK].score = 10;
	up.kp[TR_KP_RKNE].score = 10;
	tr_box_t bu             = tr_pose_box(&up);
	assert(bu.x == b.x && bu.w == b.w && bu.y == b.y && bu.h == b.h);

	/* Hips out of frame: a shoulder-only box, h = 0 (track.c scales it by
	 * the shoulder width). */
	tr_pose_t cut            = p;
	cut.kp[TR_KP_LHIP].score = 10;
	cut.kp[TR_KP_RHIP].score = 10;
	tr_box_t bc              = tr_pose_box(&cut);
	assert(bc.valid && bc.h == 0 && bc.y == b.y && bc.w == 50 && bc.x + bc.w / 2 == 320);

	/* One hip is enough; one shoulder leaves the width unknown. */
	cut.kp[TR_KP_LHIP].score = 200;
	assert(tr_pose_box(&cut).h == b.h);
	cut.kp[TR_KP_RSHO].score = 10;
	assert(tr_pose_box(&cut).valid && tr_pose_box(&cut).w == 0);
}

static void no_person(void)
{
	/* Empty booth: MoveNet still emits 17 points, all low score. */
	tr_pose_t p = figure(250, 300, 360, 12);
	assert(!tr_pose_box(&p).valid);

	/* Hips without a shoulder is not a body; a shoulder is (a close player). */
	p                      = figure(320, 80, 380, 10);
	p.kp[TR_KP_LHIP].score = 200;
	p.kp[TR_KP_RHIP].score = 200;
	assert(!tr_pose_box(&p).valid);
	p.kp[TR_KP_RSHO].score = 200;
	assert(tr_pose_box(&p).valid);
}

static void stillness(void)
{
	tr_still_t s;
	tr_pose_t  p = figure(320, 80, 380, 150);

	tr_still_reset(&s);
	for (int n = 1; n < TR_STILL_FRAMES; n++) {
		tr_pose_t j = jitter(p, 3); /* keypoint noise inside the tolerance */
		assert(!tr_still_step(&s, tr_pose_box(&j)));
	}
	assert(tr_still_step(&s, tr_pose_box(&p)));

	/* Moving restarts the count; so does losing the player. */
	tr_still_reset(&s);
	for (int n = 1; n < TR_STILL_FRAMES; n++) {
		(void)tr_still_step(&s, tr_pose_box(&p));
	}
	tr_pose_t moved = figure(360, 80, 380, 150);
	assert(!tr_still_step(&s, tr_pose_box(&moved)));
	for (int n = 1; n < TR_STILL_FRAMES - 1; n++) {
		assert(!tr_still_step(&s, tr_pose_box(&moved)));
	}
	assert(!tr_still_step(&s, (tr_box_t){ .valid = false }));
	for (int n = 1; n < TR_STILL_FRAMES; n++) {
		assert(!tr_still_step(&s, tr_pose_box(&moved)));
	}
	assert(tr_still_step(&s, tr_pose_box(&moved)));
}

static void intents(void)
{
	tr_track_t  t;
	tr_pose_t   stand = figure(320, 80, 380, 150);
	tr_intent_t i;

	tr_track_init(&t, FH);
	tr_track_calibrate(&t, tr_pose_box(&stand));
	assert(t.calibrated);

	/* Standing, with keypoint noise: nothing. */
	for (int n = 0; n < 60; n++) {
		tr_pose_t j = jitter(stand, 3);
		i           = tr_track_update(&t, tr_pose_box(&j));
		assert(i.source == TR_INPUT_VISION && i.lane_delta == 0 && !i.jump && !i.duck);
	}

	/* One arm up: exactly one lane step. The label-LEFT keypoints sit at larger x, and
	 * with the default TR_CAM_MIRROR (a selfie view) that is the player's RIGHT
	 * arm (pose.c; the other mirror setting is test_arms.c's). */
	tr_pose_t one        = stand;
	int       sum        = 0;
	one.kp[TR_KP_LWRI].y = (int16_t)(one.kp[TR_KP_LSHO].y - 60); /* 1.2 shoulder widths up */
	for (int n = 0; n < 20; n++) {
		i = tr_track_update(&t, tr_pose_box(&one));
		sum += i.lane_delta;
		assert(!i.jump && !i.duck);
	}
	assert(sum == +1);
	for (int n = 0; n < 20; n++) {
		sum += tr_track_update(&t, tr_pose_box(&stand)).lane_delta;
	}
	assert(sum == +1); /* lowering asks for nothing */

	/* Walking sideways asks for nothing. */
	tr_pose_t aside = figure(107, 80, 380, 150);
	for (int n = 0; n < 20; n++) {
		i = tr_track_update(&t, tr_pose_box(&aside));
		assert(i.lane_delta == 0 && !i.jump && !i.duck);
	}
	for (int n = 0; n < 20; n++) {
		(void)tr_track_update(&t, tr_pose_box(&stand));
	}

	/* Jump: both arms up, once; the whole body lifting is not one. */
	tr_pose_t air = figure(320, 20, 320, 150);
	for (int n = 0; n < 4; n++) {
		i = tr_track_update(&t, tr_pose_box(&air));
		assert(!i.jump && !i.duck && i.lane_delta == 0);
	}
	for (int n = 0; n < 20; n++) {
		(void)tr_track_update(&t, tr_pose_box(&stand));
	}
	tr_pose_t both        = stand;
	int       jumps       = 0;
	both.kp[TR_KP_LWRI].y = both.kp[TR_KP_RWRI].y = (int16_t)(both.kp[TR_KP_LSHO].y - 60);
	for (int n = 0; n < 20; n++) {
		i = tr_track_update(&t, tr_pose_box(&both));
		jumps += i.jump;
		assert(!i.duck && i.lane_delta == 0);
	}
	assert(jumps == 1);
	for (int n = 0; n < 20; n++) {
		(void)tr_track_update(&t, tr_pose_box(&stand));
	}

	/* Crouch: the body sinks 20 % of its height, the torso kept (a squat
	 * whose feet leave the bottom of the frame). Held -> duck from the
	 * second frame (TR_TRACK_DEBOUNCE) on. */
	tr_pose_t crouch = figure(320, 140, 440, 150);
	for (int n = 0; n < 15; n++) {
		i = tr_track_update(&t, tr_pose_box(&crouch));
		assert(i.duck == (n >= TR_TRACK_DEBOUNCE - 1) && !i.jump && i.lane_delta == 0);
	}
	for (int n = 0; n < 10; n++) {
		(void)tr_track_update(&t, tr_pose_box(&stand));
	}

	/* Nobody there (low-score noise): no intent acted on, then lost. */
	for (int n = 0; n < TR_TRACK_LOST_LIMIT; n++) {
		tr_pose_t ghost = jitter(figure(rand() % FW, 250, 390, 15), 40);
		i               = tr_track_update(&t, tr_pose_box(&ghost));
		assert(i.source == TR_INPUT_NONE && i.lane_delta == 0 && !i.jump && !i.duck);
	}
	assert(tr_track_player_lost(&t));
}

int main(void)
{
	srand(7);
	box_geometry();
	no_person();
	stillness();
	intents();
	return 0;
}
