/* tests/host/test_hud_rot.c -- the HUD buffer turned for a panel mounted
 * turned (hud.h tr_hud_t.rot, render/panel_rot.h): the 352 x 720 layer written
 * by tr_hud_update() at rotation 90 / 270 holds exactly the 720 x 352 portrait
 * HUD tr_hud_paint_all() paints, pixel for pixel, for the attract card, the
 * play screen with a popup and the game-over card. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/hud/hud.h"
#include "../../src/render/panel_rot.h"

static uint16_t portrait[TR_HUD_W * TR_HUD_H];
static uint16_t turned[TR_ROT_HUD_W * TR_HUD_W];

static void check(int rot, tr_hud_view_t *v)
{
	tr_hud_t h;

	tr_hud_init(&h);
	h.rot = rot;
	/* The first update is hud frame 0 and repaints every tile: the reference
	 * is the whole HUD painted at frame 0. */
	tr_hud_paint_all(portrait, v, 0u, h.popup_start, h.zone_start);
	memset(turned, 0, sizeof(turned));
	(void)tr_hud_update(&h, turned, v, NULL);
	for (int y = 0; y < TR_HUD_H; y++) {
		for (int x = 0; x < TR_HUD_W; x++) {
			assert(turned[tr_rot_idx(rot, TR_ROT_HUD_W, TR_HUD_W, x, y)] ==
			       portrait[y * TR_HUD_W + x]);
		}
	}
}

int main(void)
{
	static const uint8_t modes[] = { TR_HUD_PLAY, TR_HUD_ATTRACT, TR_HUD_CRASH };

	_Static_assert(TR_HUD_H == TR_ROT_HUD_W, "the rotated layer is TR_HUD_H wide");
	for (unsigned m = 0; m < sizeof(modes); m++) {
		tr_hud_view_t v;

		memset(&v, 0, sizeof(v));
		v.mode  = modes[m];
		v.score = 1234u;
		v.best  = 99999u;
		v.combo = 3;
		check(90, &v);
		check(270, &v);
	}
	puts("hud rotation ok");
	return 0;
}
