/* tests/host/hud_screens.h -- six fixed HUD screens and a hash of what they paint, for the tests
 * that pin the header card: a build without a partner logo must render exactly what it always did
 * (test_hud.c checks the hashes), and a build with one must change nothing but the attract header
 * card. Included by test_hud.c (and by whatever prints new hashes). */
#ifndef TR_TEST_HUD_SCREENS_H
#define TR_TEST_HUD_SCREENS_H

#include <string.h>

#include "../../src/game/hiscore.h"
#include "../../src/hud/hud.h"
#include "../../src/ipc/tr_mbox.h"

enum {
	SCR_PLAY,
	SCR_ATTRACT,
	SCR_TABLE, /* the attract card's high-score page */
	SCR_CRASH,
	SCR_BANNER,
	SCR_INITIALS,
	SCR_N
};

/* The attract header card, today: x 16 .. 387, y 74 .. 137 (hud.c LOGO_CARD_*). The hash of the
 * attract screens leaves out x 16 .. 399 x y 74 .. 137, which a partner header may widen into: the
 * rest of the screen must not change. */
#define HDR_X0 16
#define HDR_Y0 74
#define HDR_X1 400
#define HDR_Y1 138

static uint32_t scr_hash(const uint16_t *fb, bool skip_header)
{
	uint32_t h = 2166136261u;

	for (int y = 0; y < TR_HUD_H; y++) {
		for (int x = 0; x < TR_HUD_W; x++) {
			if (skip_header && x >= HDR_X0 && x < HDR_X1 && y >= HDR_Y0 && y < HDR_Y1) {
				continue;
			}
			h = (h ^ fb[y * TR_HUD_W + x]) * 16777619u;
		}
	}
	return h;
}

/* Paints screen `which` (a fixed view: the same numbers, text and frame every time) into fb. */
static void scr_paint(int which, uint16_t *fb)
{
	static const uint8_t ban[SCR_N] = { TR_BANNER_NONE,      TR_BANNER_ATTRACT,
		                                TR_BANNER_ATTRACT,   TR_BANNER_GAME_OVER,
		                                TR_BANNER_STEP_BACK, TR_BANNER_NONE };
	tr_hiscore_t  hs;
	tr_initials_t ini;
	tr_score_t    s;
	tr_hud_view_t v;
	uint32_t      fr = which == SCR_TABLE ? TR_HUD_PAGE_FRAMES : 10u;

	tr_hs_init(&hs);
	(void)tr_hs_insert(&hs, 12345u, "ABC");
	(void)tr_hs_insert(&hs, 9000u, "XYZ");
	memset(&ini, 0, sizeof(ini));
	strcpy(ini.name, "AB ");
	ini.pos = 2u;
	tr_score_init(&s);
	s.score  = 12345u;
	s.metres = 321u;
	s.best   = 9000u;
	memset(&v, 0, sizeof(v));
	tr_hud_view_set(&v,
	                &s,
	                ban[which],
	                which == SCR_ATTRACT || which == SCR_TABLE,
	                TR_HUD_INVITE_STEP_IN);
	if (which == SCR_INITIALS) {
		v.mode = TR_HUD_INITIALS;
	}
	tr_hud_view_booth(&v, &hs, which == SCR_INITIALS ? &ini : NULL);
	strcpy(v.perf[0], "FPS 40.0");
	tr_hud_paint_all(fb, &v, fr, 0u - 100u, 0u - 200u);
}

#endif
