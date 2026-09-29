/* tests/host/test_scene_load.c -- the scene's bin load in real play: the
 * game (tr_game_step(), step.c's spawns, 13 parts alive over a 12 s
 * approach) and the zone schedule (zone.h: every zone, its gate coming down
 * the far road) driven for a whole loop of the zones, in play (no input, the
 * run kept alive) and in attract (tr_attract_intent() at the wheel), every
 * step built and binned the A32 way -- and once more as a crash frame at
 * the booth's full camera shake (TR_FLAG_SHAKE, 255: the projection centre
 * up to TR_SCENE_SHAKE_PX across, 0.7x that up and down, which walks the
 * horizon's band over its neighbours), and a third pass in play at the
 * ramp's 1.5x cap (ramp.h: zones in real time, the frames between steps at
 * phase 1/2). No band's bin may overflow: the
 * horizon's band holds everything past ~7,000 deep -- the far road, the far
 * scenery's billboards, the far parts, the skyline -- and a dropped
 * triangle is a hole there (the skyline's foot flickering on glass). Host
 * only (links the game; not under qemu). */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/attract.h"
#include "../../src/game/ramp.h"
#include "../../src/game/state.h"
#include "../../src/game/zone.h"
#include "../../src/ipc/tr_mbox.h"
#include "../../src/render/r3d_scene.h"
#include "tr_scene_golden.h"

#define TR_BB_STEP_PX 120 /* pixels a tile's switch may change, both sides (a dish in the antenna field: 108) */

static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl;
static uint16_t       fa[TR_R3D_W * TR_R3D_H], fbuf[TR_R3D_W * TR_R3D_H];
static uint16_t       zb[TR_R3D_W * TR_BAND_H], cb[TR_R3D_W * TR_BAND_H];

static void render(const tr_scene_t *s, const tr_frame_in_t *in, uint16_t *out)
{
	uint32_t ov = 0;
	tr_cam_t cam;
	tr_bg_t  bg;

	tr_scene_build(s, in, &cam, &dl);
	tr_scene_bg(in, &cam, &bg);
	tr_bin_build(&dl, setup, bins, counts, &ov);
	for (int b = 0; b < TR_BANDS; b++) {
		/* fix round 8: clip the last band to TR_VIEW_H (REQUIRED: bins[]
		 * is sized to TR_BANDS now), matching render_band() (r3d.h). */
		int y_hi = (b + 1) * TR_BAND_H < TR_VIEW_H ? (b + 1) * TR_BAND_H : TR_VIEW_H;

		tr_raster_band(out, TR_R3D_W, b * TR_BAND_H, y_hi, zb, cb, &bg, &dl, setup, bins[b], counts[b]);
	}
	/* Zero the ragged band's stale tail so repeated render() calls (this
	 * file compares two consecutive frames pixel-by-pixel) never depend
	 * on band-buffer call history. */
	memset(&out[TR_VIEW_H * TR_R3D_W], 0, (size_t)(TR_R3D_H - TR_VIEW_H) * TR_R3D_W * sizeof(out[0]));
}

static int contrast(uint16_t a, uint16_t b)
{
	int d = abs((a >> 11) - (b >> 11)) * 8 + abs(((a >> 5) & 63) - ((b >> 5) & 63)) * 4 + abs((a & 31) - (b & 31)) * 8;

	return d;
}

int main(void)
{
	uint32_t worst_all = 0;

	setvbuf(stdout, NULL, _IONBF, 0);

	for (int mode = 0; mode < 3; mode++) {
		int          attract = mode == 1, ramp = mode == 2;
		tr_game_t    g;
		tr_zone_t    z = {0};
		tr_attract_t a;
		tr_scene_t   s;
		uint32_t     worst[TR_ZONES] = {0}, frames = 0, gates = 0,
		         steps = attract ? 2u * 5u * TR_ZONE_STEPS_ATTRACT
				 : 5u * TR_ZONE_STEPS_PLAY * (ramp ? TR_RAMP_MAX_Q8 : 256u) / 256u + 400u;

		tr_game_init(&g, 2024u + (uint32_t)attract);
		tr_zone_reset(&z);
		tr_attract_init(&a);
		tr_scene_init(&s);
		for (uint32_t t = 0; t < steps; t++) {
			tr_intent_t in_ = attract ? tr_attract_intent(&a, &g, TR_R3D_H) : tr_intent_none();

			g.alive = true, g.crashed = false; /* the run goes on: the load, not the score */
			tr_game_step(&g, in_, TR_R3D_H);
			z.ramp_q8 = ramp ? (uint16_t)TR_RAMP_MAX_Q8 : 0u;
			tr_zone_step(&z, attract != 0, TR_R3D_H);
			if (t < 300u) {
				continue; /* until the road has filled */
			}
			tr_frame_in_t in, shaken;
			tr_cam_t      cam;
			uint32_t      ov = 0, w = 0;

			memset(&in, 0, sizeof(in));
			tr_frame_in_from_game(&in, &g, 0, attract != 0, false);
			in.flags |= TR_FLAG_ZONE | TR_FLAG_CHAR;
			in.character = TR_CHAR_SOLDER; /* the most triangles */
			in.zone      = z.zone;
			in.gate_y    = z.gate_y;
			if (ramp) {
				in.pace_q8 = tr_ramp_pace_q8(tr_ramp_frame_q16(false, TR_RAMP_STEPS));
			}
			gates += z.gate_y != TR_ZONE_NO_GATE;
			tr_scene_step(&s, &in);
			/* this step's frame; a crash here at full shake (the hit: the
			 * nearest part, any would do); at the ramp, the frame between */
			for (int v = 0; v < 2; v++) {
				const tr_frame_in_t *f = &in;

				if (v == 1) {
					shaken = in;
					if (ramp) {
						shaken.flags |= TR_FLAG_PHASE;
						shaken.phase = 0x8000u;
					} else {
						shaken.flags |= TR_FLAG_CRASH | TR_FLAG_SHAKE;
						shaken.crash_tick = (uint8_t)(t % 40u); /* the jolt's phase, all of it */
						for (uint8_t i = 0; i < TR_MAX_ENTITIES; i++) {
							if (in.ents[i].kind != TR_ENT_FREE &&
							    (in.ents[shaken.crash_ent].kind == TR_ENT_FREE ||
							     in.ents[i].y > in.ents[shaken.crash_ent].y)) {
								shaken.crash_ent = i;
							}
						}
						shaken.crash_lane = in.ents[shaken.crash_ent].lane;
						shaken.shake      = 255u;
						shaken.pace_q8    = 0u; /* the HE's crash frames */
					}
					f = &shaken;
				}
				tr_scene_build(&s, f, &cam, &dl);
				tr_bin_build(&dl, setup, bins, counts, &ov);
				for (int b = 0; b < TR_BANDS; b++) {
					w = counts[b] > w ? counts[b] : w;
				}
				if (ov) {
					printf("load: %s step %u zone %s%s: %u triangle-bands dropped\n", ramp ? "ramp" : attract ? "attract" : "play",
					       (unsigned)t, tr_zone_name(z.zone), v ? (ramp ? " (phase 1/2)" : " (shaken crash)") : "", (unsigned)ov);
				}
				assert(ov == 0 && tr_dl_dropped == 0);
			}
			worst[z.zone] = w > worst[z.zone] ? w : worst[z.zone];
			frames++;
		}
		printf("load (%s, %u steps x2 frames, %u with a gate): fullest bin per zone",
		       ramp ? "play at 1.5x, + phase 1/2" : attract ? "attract, + shaken crash" : "play, + shaken crash", (unsigned)frames,
		       (unsigned)gates);
		for (int zn = 0; zn < TR_ZONES; zn++) {
			printf(" %u", (unsigned)worst[zn]);
			worst_all = worst[zn] > worst_all ? worst[zn] : worst_all;
			assert(worst[zn] > 0); /* every zone was run */
		}
		printf(" of %u\n", (unsigned)TR_BIN_MAX);
	}
	/* The mesh -> billboard switch (tr_scene_wall_bb_z): the parts one tile
	 * past it drawn either way, the rest of the frame the same -- the step a
	 * part makes the frame it crosses. Every zone, a few scroll phases: at
	 * most TR_BB_STEP_PX pixels of the frame change by >= 60 (a contrasting
	 * pixel), per part switched. */
	for (uint8_t zn = 0; zn < TR_ZONES; zn++) {
		int most = 0;

		for (uint32_t t = 0; t < 40; t += 5) {
			tr_frame_in_t in = tr_scene_golden_in(3000 + t, 1);
			tr_scene_t    s;
			int           n = 0;

			memset(in.ents, 0, sizeof(in.ents));
			in.flags |= TR_FLAG_ZONE;
			in.zone   = zn;
			in.gate_y = TR_ZONE_NO_GATE;
			tr_scene_init(&s);
			tr_scene_step(&s, &in);
			tr_scene_wall_bb_z = TR_WALL_BB_Z + (float)TR_TILE_LEN;
			render(&s, &in, fa);
			tr_scene_wall_bb_z = TR_WALL_BB_Z;
			render(&s, &in, fbuf);
			for (int i = 0; i < TR_R3D_W * TR_R3D_H; i++) {
				n += contrast(fa[i], fbuf[i]) >= 60;
			}
			most = n > most ? n : most;
		}
		printf("billboard switch at %.0f deep, %-13s: at most %d px of the frame step\n", (double)TR_WALL_BB_Z,
		       tr_zone_name(zn), most);
		assert(most <= TR_BB_STEP_PX);
	}

	/* headroom for what the load drive does not do (a crash's sparks, a
	 * gate and a pickup burst at once) */
	assert(worst_all <= TR_BIN_MAX * 7u / 8u);
	return 0;
}
