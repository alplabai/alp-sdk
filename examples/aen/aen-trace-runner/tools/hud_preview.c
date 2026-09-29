/* tools/hud_preview.c -- HUD over a host scene frame, blended the way the
 * CDC200 does it (layer 2 over layer 1: c' = a*c2 + (1 - a)*c1, ARGB4444
 * widened by bit replication), for art review. Not a test.
 *
 *   cc -std=c11 -O2 -Isrc -o /tmp/hud_preview tools/hud_preview.c src/hud/hud.c src/game/score.c \
 *      src/game/hiscore.c src/game/tilt.c
 *   /tmp/hud_preview SCENE.ppm OUT.ppm play|pickup|combo|hiscore-pop|crash|attract|table|initials|banner [frame]
 *
 * SCENE.ppm: a 720x1280 P6 frame, e.g. TR_DUMP=1 test_r3d_scene's
 * /tmp/tr-scene-*.ppm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud/hud.h"
#include "ipc/tr_mbox.h"

#define W 720
#define H 1280

static uint8_t  img[W * H * 3];
static uint16_t hud[TR_HUD_W * TR_HUD_H];

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s SCENE.ppm OUT.ppm play|pickup|combo|hiscore-pop|crash|attract|table|initials|banner [frame]\n",
			argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	int   w, h, mx;

	if (f == NULL || fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w != W || h != H || fgetc(f) < 0 ||
	    fread(img, 1, sizeof(img), f) != sizeof(img)) {
		fprintf(stderr, "bad scene %s\n", argv[1]);
		return 1;
	}
	fclose(f);

	tr_score_t    s;
	tr_hud_view_t v;
	const char   *m     = argv[3];
	uint32_t      frame = argc > 4 ? (uint32_t)atoi(argv[4]) : 0u, start = 0u - TR_HUD_POPUP_FRAMES;
	uint8_t       ban   = TR_BANNER_NONE;
	bool          att   = false;

	tr_hiscore_t  hs;
	tr_initials_t ini;
	bool          entering = false;

	tr_hs_init(&hs);
	(void)tr_hs_insert(&hs, 15230u, "ACE");
	(void)tr_hs_insert(&hs, 12480u, "E8 ");
	(void)tr_hs_insert(&hs, 9001u, "PRO");
	(void)tr_hs_insert(&hs, 4410u, "SOL");
	(void)tr_hs_insert(&hs, 8645u, "PIX"); /* the latest: gold */
	tr_score_init(&s);
	s.best       = 12480u;
	s.score      = 8645u;
	s.metres     = 1542u;
	s.combo      = 3u;
	s.popup_pts  = 30u;
	s.popup_mult = 3u;
	s.popup_seq  = 7u;
	if (strcmp(m, "pickup") == 0) {
		start = 0u; /* popup `frame` frames old */
	} else if (strcmp(m, "combo") == 0) { /* booth: the top multiplier */
		start        = 0u;
		s.combo      = 5u;
		s.popup_pts  = 50u;
		s.popup_mult = 5u;
	} else if (strcmp(m, "hiscore-pop") == 0) { /* booth: passed the table's best */
		start      = 0u;
		s.score    = 15231u;
		s.popup_hs = 1u;
	} else if (strcmp(m, "table") == 0) { /* booth: the attract card's table page */
		ban = TR_BANNER_ATTRACT;
		att = true;
		frame += TR_HUD_PAGE_FRAMES;
	} else if (strcmp(m, "initials") == 0) { /* booth: entering "AC" + the third letter */
		entering = true;
		s.score  = 13100u;
		tr_ini_start(&ini, "PRO", tr_hs_rank(&hs, s.score));
		ini.name[0] = 'A';
		ini.name[1] = 'C';
		ini.name[2] = 'E';
		ini.pos     = 2u;
	} else if (strcmp(m, "crash") == 0) {
		ban = TR_BANNER_GAME_OVER;
	} else if (strcmp(m, "crash-best") == 0) {
		ban        = TR_BANNER_GAME_OVER;
		s.score    = 15230u;
		s.best     = 15230u;
		s.new_best = 1u;
	} else if (strcmp(m, "attract") == 0) {
		ban = TR_BANNER_ATTRACT;
		att = true;
	} else if (strcmp(m, "banner") == 0) {
		ban = TR_BANNER_STEP_BACK;
	}
	memset(&v, 0, sizeof(v));
	tr_hud_view_set(&v, &s, ban, att, TR_HUD_INVITE_TILT);
	v.character = getenv("TR_CHAR") ? (uint8_t)atoi(getenv("TR_CHAR")) : 0u;
	tr_hud_view_booth(&v, &hs, entering ? &ini : NULL);
	{
		tr_perf_t     p   = { 0 };
		tr_perf_mem_t mem = { 5883904u, 8388608u, 346112u, 180224u, 64512u };
		tr_perf_raw_t r   = { 0, 0, 0, 0, 0, 0, 0, 0 };

		tr_perf_sample(&p, &r, &mem, &v);
		r = (tr_perf_raw_t){ 500000u, 20u, 20u * 2210000u, 20u * 2150000u, 17u, 100u, 0u, 0u };
		tr_perf_sample(&p, &r, &mem, &v);
	}
	tr_hud_paint_all(hud, &v, frame, start, 0u - TR_HUD_ZONE_FRAMES);
	for (int y = 0; y < TR_HUD_H; y++) {
		for (int x = 0; x < TR_HUD_W; x++) {
			uint32_t p = hud[y * TR_HUD_W + x], a = (p >> 12) * 17u;
			uint8_t *d = &img[(y * W + x) * 3];

			for (int c = 0; c < 3; c++) {
				uint32_t cs = ((p >> (8 - 4 * c)) & 15u) * 17u;

				d[c] = (uint8_t)((a * cs + (255u - a) * d[c] + 127u) / 255u);
			}
		}
	}
	f = fopen(argv[2], "wb");
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	fwrite(img, 1, sizeof(img), f);
	fclose(f);
	return 0;
}
