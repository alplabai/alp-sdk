/* src/game/hiscore.c -- see hiscore.h. */
#include "hiscore.h"

#include <string.h>

void tr_hs_init(tr_hiscore_t *t)
{
	memset(t, 0, sizeof(*t));
	t->last = TR_HS_NONE;
}

int tr_hs_rank(const tr_hiscore_t *t, uint32_t score)
{
	if (score == 0u) {
		return -1;
	}
	for (int i = 0; i < t->n; i++) {
		if (score > t->e[i].score) {
			return i; /* strictly better: an equal score stays below */
		}
	}
	return t->n < TR_HS_N ? t->n : -1;
}

int tr_hs_insert(tr_hiscore_t *t, uint32_t score, const char *name)
{
	int r = tr_hs_rank(t, score);

	if (r < 0) {
		return -1;
	}
	int last = t->n < TR_HS_N ? t->n : TR_HS_N - 1; /* a full table drops its last place */

	memmove(&t->e[r + 1], &t->e[r], (size_t)(last - r) * sizeof(t->e[0]));
	t->e[r].score = score;
	strncpy(t->e[r].name, name, 3);
	t->e[r].name[3] = '\0';
	t->n            = (uint8_t)(last + 1);
	t->last         = (uint8_t)r;
	return r;
}

uint32_t tr_hs_top(const tr_hiscore_t *t)
{
	return t->n != 0u ? t->e[0].score : 0u;
}

void tr_hs_arm(tr_score_t *s, const tr_hiscore_t *t)
{
	s->hs_top = tr_hs_top(t);
}

uint8_t tr_hs_entry_input(bool hud_up, bool imu_ok, tr_mode_t mode)
{
	if (!hud_up) {
		return TR_HS_IN_NONE; /* a frozen frame with no letters on it helps nobody */
	}
	if (mode == TR_MODE_VISION) {
		return TR_HS_IN_VISION;
	}
	return imu_ok ? TR_HS_IN_TILT : TR_HS_IN_NONE;
}

/* ---------------------------------------------------------------- initials */
static const char alphabet[] = TR_INI_ALPHABET;
#define ALPHA_N ((int)sizeof(alphabet) - 1)

static int index_of(char c)
{
	const char *p = c != '\0' ? strchr(alphabet, c) : NULL;

	return p != NULL ? (int)(p - alphabet) : ALPHA_N - 1; /* unknown: the space */
}

void tr_ini_start(tr_initials_t *e, const char *deflt, int rank)
{
	memset(e, 0, sizeof(*e));
	for (int i = 0; i < 3; i++) {
		char c = *deflt != '\0' ? *deflt++ : ' ';

		e->name[i] = alphabet[index_of(c)];
	}
	e->rank = (int8_t)rank;
}

bool tr_ini_step(tr_initials_t *e, tr_intent_t in)
{
	if (e->why != TR_INI_ENTERING) {
		return false;
	}
	e->frames++;
	e->idle++;
	if (in.lane_delta != 0 || in.jump || in.duck) {
		e->idle = 0u;
	}
	if (in.lane_delta != 0) {
		int i = (index_of(e->name[e->pos]) + (in.lane_delta > 0 ? 1 : ALPHA_N - 1)) % ALPHA_N;

		e->name[e->pos] = alphabet[i];
	}
	if (in.jump && ++e->pos == 3u) {
		e->pos = 2u;
		e->why = TR_INI_DONE;
	} else if (in.duck && e->pos > 0u) {
		e->pos--;
	}
	if (e->why == TR_INI_ENTERING && e->idle >= TR_INI_IDLE_FRAMES) {
		e->why = TR_INI_WALKAWAY;
	}
	if (e->why == TR_INI_ENTERING && e->frames >= TR_INI_TIMEOUT_FRAMES) {
		e->why = TR_INI_TIMEOUT;
	}
	return e->why == TR_INI_ENTERING;
}

bool tr_ini_tilt_frame(tr_initials_t *e, tr_tilt_t *t, int16_t x_q8, int16_t y_q8)
{
	int ax = x_q8 < 0 ? -x_q8 : x_q8, ay = y_q8 < 0 ? -y_q8 : y_q8;

	t->idle_ticks++; /* tr_tilt_intent() zeroes it on a gesture */
	tr_intent_t in = tr_tilt_intent(t, x_q8, y_q8);

	if (ax >= TR_TILT_EDGE_Q8 && ax > ay) {
		e->held = e->held < 0xFFFFu ? (uint16_t)(e->held + 1u) : e->held;
		if (e->held >= TR_INI_REPEAT_DELAY &&
		    (e->held - TR_INI_REPEAT_DELAY) % TR_INI_REPEAT_EVERY == 0u) {
			in.lane_delta =
			    (int8_t)((x_q8 < 0 ? -1 : 1) * TR_TILT_STEER_SIGN); /* tilt.c's direction */
			t->idle_ticks = 0u;
		}
	} else {
		e->held = 0u;
	}
	return tr_ini_step(e, in);
}

bool tr_ini_vision_frame(tr_initials_t *e, tr_track_t *t, tr_box_t b, bool present)
{
	tr_intent_t in = tr_track_update(t, b);

	return tr_ini_step(e, present ? in : tr_intent_none());
}
