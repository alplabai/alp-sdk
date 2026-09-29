/* tools/zone_preview.c -- world zones (P15) for art review: the A32 scene
 * (host raster, bit-identical to the A32's) with the HE's layer-2 HUD
 * blended over it the way the CDC200 does, driven like the HE drives it: an
 * attract-played run at the 30 Hz play pace, tr_zone_t stepped with the game
 * and shipped in the packet. Not a test.
 *
 *   cc -std=c11 -O2 -ffp-contract=off -DTR_PANEL_HZ=30 -Isrc -o /tmp/zone_preview tools/zone_preview.c \
 *      <every src .c the host tests link: tests/host/runner.sh> -lm
 *   /tmp/zone_preview still OUTDIR   per zone and character (P16, TR_CHAR_*) a frame mid-zone:
 *                                    OUTDIR/zone-<n>-<c>.ppm, and the gate coming up: OUTDIR/gate-<n>-<c>.ppm
 *   /tmp/zone_preview attract OUTDIR per zone the attract card with the zone's name showing beside the
 *                                    character + tilt invitation: OUTDIR/attract-<n>.ppm
 *   /tmp/zone_preview run OUTDIR ZONE FRAMES [CHR [LEAD [SKIP]]]
 *                                    FRAMES frames at 30 Hz from ZONE's gate ~2.3 s out: OUTDIR/f<nnnn>.ppm;
 *                                    LEAD: from LEAD steps before the gate instead (mid-zone: < the zone's
 *                                    TR_ZONE_STEPS_PLAY less the gate lead, e.g. 500), SKIP frames stepped
 *                                    unrendered first; the runner's lane each frame on stdout (find a lane
 *                                    change: the attract run's first is 283 frames in, every zone alike)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/attract.h"
#include "game/react.h"
#include "game/zone.h"
#include "hud/hud.h"
#include "ipc/tr_mbox.h"
#include "render/r3d_scene.h"

#define W  TR_R3D_W
#define H  TR_R3D_H
#define HZ 30

static uint16_t       fb[W * H], hudfb[TR_HUD_W * TR_HUD_H];
static uint16_t       zb[W * TR_BAND_H], cb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl;

static void render(tr_scene_t *s, const tr_frame_in_t *in)
{
	tr_cam_t cam;
	tr_bg_t  bg;
	uint32_t ov = 0;

	tr_scene_step(s, in);
	tr_scene_build(s, in, &cam, &dl);
	tr_scene_bg(in, &cam, &bg);
	tr_scene_bg_flash(in, &bg);
	tr_bin_build(&dl, setup, bins, counts, &ov);
	for (int b = 0; b < TR_BANDS; b++) {
		tr_raster_band(fb, W, b * TR_BAND_H, (b + 1) * TR_BAND_H, zb, cb, &bg, &dl, setup, bins[b], counts[b]);
	}
}

/* The frame with layer 2 (ARGB4444, straight alpha) over rows 0..TR_HUD_H. */
static void write_ppm(const char *path, int hud)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL) {
		perror(path);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p      = fb[i];
		int      rgb[3] = {(p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3};

		if (hud && i < TR_HUD_W * TR_HUD_H) {
			uint16_t q = hudfb[i];
			int      a = q >> 12, c2[3] = {((q >> 8) & 15) * 17, ((q >> 4) & 15) * 17, (q & 15) * 17};

			for (int k = 0; k < 3; k++) {
				rgb[k] = (a * c2[k] + (15 - a) * rgb[k] + 7) / 15;
			}
		}
		fputc(rgb[0], f), fputc(rgb[1], f), fputc(rgb[2], f);
	}
	fclose(f);
}

typedef struct {
	tr_game_t    g;
	tr_attract_t at;
	tr_zone_t    z;
	tr_scene_t   s;
	tr_hud_t     hud;
	tr_score_t   score;
	tr_react_t   r;
	uint8_t      chr;
	uint32_t     phase;
} sim_t;

static void sim_init(sim_t *m, uint8_t zone, uint32_t steps_before_gate, uint8_t chr)
{
	memset(m, 0, sizeof(*m));
	m->chr = chr;
	tr_react_init(&m->r);
	tr_game_init(&m->g, 12345u);
	tr_attract_init(&m->at);
	tr_zone_reset(&m->z);
	m->z.zone  = zone;
	m->z.steps = TR_ZONE_STEPS_PLAY - tr_zone_gate_lead(H) - steps_before_gate;
	tr_scene_init(&m->s);
	tr_hud_init(&m->hud);
	tr_score_init(&m->score);
	tr_score_run_start(&m->score);
}

/* One 30 Hz frame of play driven by the attract AI; returns the packet. */
static tr_frame_in_t sim_frame(sim_t *m)
{
	tr_frame_in_t in;

	m->phase += TR_PLAY_SPEED_Q16;
	if (m->phase >= 65536u) {
		m->phase -= 65536u;
		tr_game_step(&m->g, tr_attract_intent(&m->at, &m->g, H), H);
		tr_score_step(&m->score, &m->g);
		tr_react_step(&m->r, &m->g, m->score.combo);
		tr_zone_step(&m->z, false, H);
		if (!m->g.alive) { /* keep the camera running: a fresh run, same zone schedule */
			tr_game_init(&m->g, m->g.rng);
		}
	}
	tr_frame_in_from_game(&in, &m->g, TR_BANNER_NONE, false, false);
	in.track_h = H;
	in.phase   = (uint16_t)m->phase;
	in.pace_q8 = (uint8_t)(TR_PLAY_SPEED_Q16 >> 8);
	in.flags |= (in.phase ? TR_FLAG_PHASE : 0u) | TR_FLAG_HUD_L2;
	tr_frame_in_p16(&in, m->chr, &m->r, false, 0u);
	tr_frame_in_set_zone(&in, &m->z);
	tr_react_frame(&m->r);
	return in;
}

static void hud_frame(sim_t *m, bool attract)
{
	tr_hud_view_t v;

	memset(&v, 0, sizeof(v));
	tr_hud_view_set(&v, &m->score, attract ? TR_BANNER_ATTRACT : TR_BANNER_NONE, attract,
			attract ? TR_HUD_INVITE_TILT : TR_HUD_INVITE_NONE);
	tr_hud_view_zone(&v, m->z.zone, m->z.seq);
	v.character = m->chr;
	tr_hud_update(&m->hud, hudfb, &v, NULL);
}

int main(int argc, char **argv)
{
	static sim_t m;
	char         path[512];

	if (argc >= 3 && strcmp(argv[1], "still") == 0) {
		for (uint8_t z = 0; z < TR_ZONES; z++) {
			for (uint8_t c = 0; c < TR_CHAR_N; c++) {
				/* mid-zone: gate far off; then with the gate ~1.4 s out */
				for (int gate = 0; gate < 2; gate++) {
					sim_init(&m, z, gate ? 0u : 400u, c);
					for (int f = 0; f < (gate ? 128 : 90); f++) {
						tr_frame_in_t in = sim_frame(&m);

						hud_frame(&m, false);
						if (f == (gate ? 127 : 89)) {
							render(&m.s, &in);
						} else {
							tr_scene_step(&m.s, &in);
						}
					}
					snprintf(path, sizeof(path), "%s/%s-%u-%s.ppm", argv[2], gate ? "gate" : "zone", z,
						 tr_hud_char_name(c));
					write_ppm(path, !gate);
				}
			}
		}
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "attract") == 0) {
		for (uint8_t z = 0; z < TR_ZONES; z++) {
			sim_init(&m, z, 400u, (uint8_t)(z % TR_CHAR_N));
			m.z.seq++; /* an entry: the name shows */
			for (int f = 0; f < 40; f++) { /* 1.3 s into its 2.75 s */
				tr_frame_in_t in = sim_frame(&m);

				hud_frame(&m, true);
				if (f == 39) {
					render(&m.s, &in);
				} else {
					tr_scene_step(&m.s, &in);
				}
			}
			snprintf(path, sizeof(path), "%s/attract-%u.ppm", argv[2], z);
			write_ppm(path, 1);
		}
		return 0;
	}
	if (argc >= 5 && strcmp(argv[1], "run") == 0) {
		int n = atoi(argv[4]);

		sim_init(&m, (uint8_t)atoi(argv[3]), argc >= 7 ? (uint32_t)atoi(argv[6]) : 0u, argc >= 6 ? (uint8_t)atoi(argv[5]) : 0u);
		for (int k = argc >= 8 ? atoi(argv[7]) : 0; k > 0; k--) {
			tr_frame_in_t in = sim_frame(&m);

			hud_frame(&m, false);
			tr_scene_step(&m.s, &in);
		}
		while (argc < 7 && (m.z.gate_y == TR_ZONE_NO_GATE || m.z.gate_y < 600)) { /* pre-roll: the gate ~2.3 s out */
			tr_frame_in_t in = sim_frame(&m);

			hud_frame(&m, false);
			tr_scene_step(&m.s, &in);
		}
		for (int f = 0; f < n; f++) {
			tr_frame_in_t in = sim_frame(&m);

			hud_frame(&m, false);
			render(&m.s, &in);
			snprintf(path, sizeof(path), "%s/f%04d.ppm", argv[2], f);
			write_ppm(path, 1);
			if (argc >= 7) {
				printf("%d %u\n", f, m.g.lane);
			}
		}
		return 0;
	}
	fprintf(stderr, "usage: %s still OUTDIR | attract OUTDIR | run OUTDIR ZONE FRAMES [CHR [LEAD [SKIP]]]\n", argv[0]);
	return 2;
}
