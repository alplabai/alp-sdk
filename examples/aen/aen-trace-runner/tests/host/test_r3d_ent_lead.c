/* tests/host/test_r3d_ent_lead.c -- the visibility lead, in screen terms
 * (maintainer on glass, three times: "the road obstacles start rendering
 * when they get close" / "You need to start earlier" / "obstacles still too
 * close. You should start drawing earlier"). In every zone, every obstacle,
 * pickup and live wire, alone in the runner's lane, rendered the way the A32
 * does it:
 *   - the road: visible (>= CONTRAST off the fog colour) from the skyline's
 *     foot down, so a far part stands ON the road, not in a haze above it;
 *   - no pop: at its spawn step it adds no contrasting pixel, and it is
 *     there within 0.3 s (the fade-in);
 *   - it first appears far up the road: its base within TR_APPEAR_PX of the
 *     horizon, at or below the skyline's foot (on the visible road);
 *   - lead: from TR_LEAD_S seconds before impact (real time, play pace) it
 *     is recognisable -- >= TR_REC_PX across one way, >= TR_REC_MIN the other;
 *   - the curves: entity fog 1 at spawn, a bounded fade-in, never rising as
 *     the part comes in; screen size (scale / z) only ever growing. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/game/state.h"
#include "../../src/game/zone.h"
#include "../../src/render/proj.h"
#include "../../src/render/r3d_scene.h"
#include "tr_scene_golden.h"

#define W TR_R3D_W
#define H TR_R3D_H

#define TR_LEAD_S 8 /* seconds of warning the player gets: recognisable by then */
/* Screen-size targets tuned at TR_VIEW_TUNED_H and rescaled to the game
 * viewport (r3d.h TR_VIEW_PX): the whole picture scales with TR_VIEW_H. */
#define TR_REC_PX    TR_VIEW_PX(16) /* px across (the larger way) that make a part recognisable */
#define TR_REC_MIN   TR_VIEW_PX(6)  /* ... and px the other way: a solid thing, not a line */
#define TR_APPEAR_PX TR_VIEW_PX(60) /* a new part's base at most this far under the horizon */
#define TR_FADE_S10  3              /* the fade-in, tenths of a second */
#define TR_ROAD_EDGES \
	6 /* colour edges across the 3 lanes in a far road row: its lines and rails (the flat haze of 737f968: 0) */
#define CONTRAST 60 /* 8-bit |dR| + |dG| + |dB| of a pixel that reads against its surround */
#define ROAD_CONTRAST \
	40 /* ... of the road against the haze: neon city's dark board under its dark purple glow is 52
			  * at the far end (the board's own colour, not the fog: 0.2 of it was 8,800 deep) */

/* Game steps per second at the play pace (state.h: 0.5x = 20). */
#define STEPS_S (TR_GAME_PACE_Q8 * 40u / 256u)

static uint16_t       fb[W * H], fb0[W * H];
static uint16_t       zb[W * TR_BAND_H], cb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl;
static tr_cam_t       cam;
static tr_bg_t        bg;

static void render(const tr_scene_t *s, const tr_frame_in_t *in, uint16_t *out)
{
	uint32_t overflow = 0;

	tr_scene_build(s, in, &cam, &dl);
	tr_scene_bg(in, &cam, &bg);
	tr_bin_build(&dl, setup, bins, counts, &overflow);
	for (int b = 0; b < TR_BANDS; b++) {
		tr_raster_band(out,
		               W,
		               b * TR_BAND_H,
		               (b + 1) * TR_BAND_H,
		               zb,
		               cb,
		               &bg,
		               &dl,
		               setup,
		               bins[b],
		               counts[b]);
	}
	assert(overflow == 0);
}

static int contrast(uint16_t a, uint16_t b)
{
	return abs((a >> 11) - (b >> 11)) * 8 + abs(((a >> 5) & 63) - ((b >> 5) & 63)) * 4 +
	       abs((a & 31) - (b & 31)) * 8;
}

/* Bounding box of the pixels that contrast with the entity-free frame;
 * returns their count. */
static int part_box(int *x0, int *y0, int *x1, int *y1)
{
	int n = 0;

	*x0 = W, *y0 = H, *x1 = -1, *y1 = -1;
	for (int i = 0; i < W * H; i++) {
		if (contrast(fb[i], fb0[i]) >= CONTRAST) {
			int r = i / W, c = i % W;

			n++;
			*x0 = c < *x0 ? c : *x0, *x1 = c > *x1 ? c : *x1;
			*y0 = r < *y0 ? r : *y0, *y1 = r > *y1 ? r : *y1;
		}
	}
	return n;
}

int main(void)
{
	static const struct {
		uint8_t     kind, low;
		const char *name;
	} k[]         = { { 1, 1, "low obstacle" },
		              { 1, 0, "high obstacle" },
		              { 2, 0, "pickup" },
		              { 3, 1, "low wire" },
		              { 3, 0, "high wire" } };
	const int  ry = tr_runner_ground_y(TR_R3D_H);
	tr_scene_t s;

	setvbuf(stdout, NULL, _IONBF, 0); /* the numbers reach the log before an assert fails */

	/* 1. Rendered, per zone. */
	for (uint8_t zn = 0; zn < TR_ZONES; zn++) {
		tr_frame_in_t none = tr_scene_golden_in(3000, 1);
		tr_sv_t       sv;
		float         vz;

		memset(none.ents, 0, sizeof(none.ents));
		none.flags |= TR_FLAG_ZONE;
		none.zone   = zn;
		none.gate_y = TR_ZONE_NO_GATE;
		tr_scene_init(&s);
		tr_scene_step(&s, &none);
		render(&s, &none, fb0);

		/* The far road: every row from the skyline's foot down to
		 * TR_APPEAR_PX under the horizon shows the board over the lanes
		 * (x 300..420, a majority >= ROAD_CONTRAST off the fog colour) -- the
		 * haze no longer eats it (nearer, the zones' own ground art sets
		 * the contrast, not the fog). */
		assert(tr_r3d_project(
		    &cam,
		    (tr_v3_t){ 0.0f, 0.0f, (float)TR_PROJ_Z_RUNNER - TR_CAM_BACK + TR_SCENE_SKY_DZ },
		    &sv,
		    &vz));
		const int hz = bg.horizon, foot = sv.y >> TR_R3D_SUB;

		for (int y = foot + 1; y <= hz + TR_APPEAR_PX; y++) {
			int n = 0;

			for (int x = 300; x <= 420; x++) {
				n += contrast(fb0[y * W + x], bg.ground) >= ROAD_CONTRAST;
			}
			if (n <= 60) {
				printf("zone %u: the road fades into the haze at row %d (%d of 121 px; horizon %d, "
				       "skyline foot %d)\n",
				       zn,
				       y,
				       n,
				       hz,
				       foot);
			}
			assert(n > 60);
		}
		printf("%-13s horizon %d, skyline foot %d: the road visible from row %d\n",
		       tr_zone_name(zn),
		       hz,
		       foot,
		       foot + 1);

		/* The far road is rendered, not a haze-coloured hole: in every
		 * row from the skyline's foot to where the near ground's texture
		 * starts, each lane's edge line (its texture's column 0: the
		 * dashes, as the minified texture shows them) stands out from the
		 * lane's own middle, and the side scenery covers some of the
		 * shoulder row beyond the lanes. */
		{
			int bad = 0, empty = 0, minl = 1 << 30;

			for (int y = foot + 2; y <= hz + 90;
			     y++) { /* to ~3,100 deep: the near ground's own dashes (and gaps) from there */
				float zlo = (float)TR_PROJ_Z_RUNNER,
				      zhi = (float)TR_PROJ_Z_RUNNER - TR_CAM_BACK + TR_SCENE_SKY_DZ;

				for (int it = 0; it < 40; it++) { /* the board depth that projects to row y */
					float zm = 0.5f * (zlo + zhi);

					tr_r3d_project(&cam, (tr_v3_t){ 0.0f, 0.0f, zm }, &sv, &vz);
					*((sv.y >> TR_R3D_SUB) > y ? &zlo : &zhi) = zm;
				}
				int line = 0, side = 0;

				if (zlo < 3100.0f) {
					break;
				}

				/* texture: colour edges along the row across the lanes -- the
				 * lane lines and rails, each an edge in and out */
				int lx0, lx1;

				tr_r3d_project(&cam, (tr_v3_t){ -360.0f, 0.0f, zlo }, &sv, &vz);
				lx0 = sv.x >> TR_R3D_SUB;
				tr_r3d_project(&cam, (tr_v3_t){ 360.0f, 0.0f, zlo }, &sv, &vz);
				lx1 = sv.x >> TR_R3D_SUB;
				for (int x = lx0 - 1; x <= lx1; x++) {
					line += contrast(fb0[y * W + x], fb0[y * W + x + 1]) >= ROAD_CONTRAST;
				}
				minl = line < minl ? line : minl;
				for (int sd = -1; sd <= 1;
				     sd += 2) { /* scenery: 400..1400 out, any px off the board's colour */
					int x0s, x1s, n = 0;

					tr_r3d_project(&cam, (tr_v3_t){ 400.0f * (float)sd, 0.0f, zlo }, &sv, &vz);
					x0s = sv.x >> TR_R3D_SUB;
					tr_r3d_project(&cam, (tr_v3_t){ 1400.0f * (float)sd, 0.0f, zlo }, &sv, &vz);
					x1s = sv.x >> TR_R3D_SUB;
					for (int x = x0s < x1s ? x0s : x1s; x <= (x0s < x1s ? x1s : x0s); x++) {
						n += x >= 0 && x < W &&
						     contrast(fb0[y * W + x], fb0[y * W + (x0s < x1s ? x0s : x1s)]) >= 30;
					}
					side += n > 0;
				}
				bad += line < TR_ROAD_EDGES;
				empty += side < 2;
				if (line < TR_ROAD_EDGES || side < 2) {
					printf("  row %d (depth %.0f): %d colour edges across the lanes, scenery on %d "
					       "sides\n",
					       y,
					       (double)zlo,
					       line,
					       side);
				}
			}
			printf("%-13s far road rows from %d: fewest colour edges across the lanes %d; %d rows "
			       "under %d, %d without scenery both sides\n",
			       tr_zone_name(zn),
			       foot + 2,
			       minl,
			       bad,
			       TR_ROAD_EDGES,
			       empty);
			assert(bad == 0 && empty == 0);
		}

		for (unsigned j = 0; j < sizeof(k) / sizeof(k[0]); j++) {
			tr_frame_in_t in = none;
			int           x0, y0, x1, y1, n = 0, steps;

			/* no pop: nothing at spawn, in any lane */
			for (uint8_t lane = 0; lane < (zn == 0 ? TR_LANES : 1); lane++) {
				in.ents[5] = (tr_pkt_ent_t){ k[j].kind,  (uint8_t)(zn == 0 ? lane : 1),
					                         k[j].low,   0,
					                         TR_SPAWN_Y, 0 };
				render(&s, &in, fb);
				assert(part_box(&x0, &y0, &x1, &y1) == 0);
			}
			/* there within the fade-in, far up the road */
			in.ents[5].lane = 1; /* the runner's */
			for (steps = 1; n == 0; steps++) {
				assert(steps * 10 <= TR_FADE_S10 * (int)STEPS_S);
				in.ents[5].y = (int16_t)(TR_SPAWN_Y + steps * TR_SCROLL_PX);
				render(&s, &in, fb);
				n = part_box(&x0, &y0, &x1, &y1);
			}
			/* where it stands: its base on the board, projected (its first
			 * contrasting pixels may be its top, against the skyline) */
			assert(tr_r3d_project(&cam, (tr_v3_t){ 0.0f, 0.0f, tr_scene_ent_z(&in, 5) }, &sv, &vz));
			int base = sv.y >> TR_R3D_SUB;

			printf("  %-13s spawn %.2f s out; appears %.2f s later, %d x %d px, standing at row %d "
			       "(%d under the horizon); ",
			       k[j].name,
			       (double)(ry - TR_SPAWN_Y) / TR_SCROLL_PX / STEPS_S,
			       (double)(steps - 1) / STEPS_S,
			       x1 - x0 + 1,
			       y1 - y0 + 1,
			       base,
			       base - hz);
			assert(base - hz <= TR_APPEAR_PX && base >= foot);
			/* recognisable TR_LEAD_S out */
			in.ents[5].y = (int16_t)(ry - TR_LEAD_S * (int)STEPS_S * TR_SCROLL_PX);
			render(&s, &in, fb);
			part_box(&x0, &y0, &x1, &y1);
			int w = x1 - x0 + 1, h = y1 - y0 + 1;

			assert(tr_r3d_project(&cam, (tr_v3_t){ 0.0f, 0.0f, tr_scene_ent_z(&in, 5) }, &sv, &vz));
			printf("%d s out %d x %d px at row %d\n", TR_LEAD_S, w, h, (int)(sv.y >> TR_R3D_SUB));
			assert((w >= TR_REC_PX && h >= TR_REC_MIN) || (h >= TR_REC_PX && w >= TR_REC_MIN));
			assert((sv.y >> TR_R3D_SUB) > foot); /* on the visible road */
		}
	}

	/* 3. A run's worth of parts: one spawned every TR_SPAWN_TICKS steps
	 * (step.c spawn()'s draw: a quarter pickups, a fifth of obstacles
	 * live wires, two thirds low, any lane), from the spawn row to past the
	 * runner -- every live one past its grow-in (0.25 s) shows (removing it
	 * changes >= 3 pixels), the far ones between the others too. */
	for (int frame = 0; frame < 2; frame++) {
		tr_frame_in_t in   = tr_scene_golden_in(3000, 1), minus;
		uint32_t      rng  = 12345u + (uint32_t)frame * 77u;
		int           live = 0, grown = 0, shown = 0;
		uint16_t      full[W * H];

		memset(in.ents, 0, sizeof(in.ents));
		for (int i = 0; i < 16; i++) {
			int y = TR_SPAWN_Y + (i * TR_SPAWN_TICKS + frame * 9) * TR_SCROLL_PX;

			rng        = rng * 1664525u + 1013904223u;
			uint32_t r = rng >> 8;

			if (y > ry) {
				break;
			}
			in.ents[i] = (tr_pkt_ent_t){ (uint8_t)(!(r & 3u)                ? 2
				                                   : ((r >> 16) % 5u == 0u) ? 3
				                                                            : 1),
				                         (uint8_t)((r >> 3) % 3u),
				                         (uint8_t)(((r >> 7) % 3u) != 0u),
				                         0,
				                         (int16_t)y,
				                         0 };
			live++;
		}
		tr_scene_init(&s);
		tr_scene_step(&s, &in);
		render(&s, &in, full);
		memcpy(fb0, full, sizeof(full));
		for (int i = 0; i < 16; i++) {
			int x0, y0, x1, y1, n;

			if (in.ents[i].kind == 0 || in.ents[i].y <= TR_SPAWN_Y + 5 * TR_SCROLL_PX) {
				continue; /* not there, or still growing in */
			}
			grown++;
			minus              = in;
			minus.ents[i].kind = 0;
			render(&s, &minus, fb);
			n = part_box(&x0, &y0, &x1, &y1);
			shown += n >= 3;
			if (n < 3) {
				printf("part %d (kind %u lane %u, %.1f s out) does not show\n",
				       i,
				       in.ents[i].kind,
				       in.ents[i].lane,
				       (double)(ry - in.ents[i].y) / TR_SCROLL_PX / STEPS_S);
			}
		}
		printf(
		    "a run's frame %d: %d live parts, %d grown in, %d show\n", frame, live, grown, shown);
		assert(live >= 12 && grown >= live - 1 && shown == grown);
	}

	/* 2. The curves. Fade-in: whole at spawn for every part (emissive
	 * too), the haze alone (<= 25 %) and the part fully grown from 0.3 s
	 * real time after it; from there in, neither the fog rises nor the
	 * screen size (scale / z) falls, step by step down to the runner. */
	for (uint8_t kind = 1; kind <= 3; kind++) {
		const float zs  = (float)tr_proj_depth_of_model_y(TR_SPAWN_Y);
		const float dz  = (float)((TR_PROJ_Z_FAR - TR_PROJ_Z_RUNNER) * TR_SCROLL_PX) / (float)ry;
		const float z3  = zs - 0.1f * TR_FADE_S10 * (float)STEPS_S * dz;
		float       fog = 2.0f, size = -1.0f;

		assert(tr_scene_ent_fog(zs, 1.0f) == 1.0f && tr_scene_ent_fog(zs, 0.3f) == 1.0f);
		assert(tr_scene_ent_scale(zs, kind) == 0.0f); /* grows in from nothing */
		assert(tr_scene_ent_fog(z3, 1.0f) <= 0.25f && tr_scene_ent_scale(z3, kind) > 1.0f);
		for (float z = zs; z >= (float)TR_PROJ_Z_RUNNER; z -= 4.0f) {
			float f  = tr_scene_ent_fog(z, 1.0f),
			      sz = tr_scene_ent_scale(z, kind) / (z - (float)TR_PROJ_Z_RUNNER + TR_CAM_BACK);

			assert(f <= fog && sz > size);
			assert(z > z3 || f <= 0.25f);
			assert(tr_scene_ent_fog(z, 0.4f) <= f);
			fog = f, size = sz;
		}
		assert(tr_scene_ent_scale(1800.0f, kind) == 1.0f &&
		       tr_scene_ent_scale((float)TR_PROJ_Z_RUNNER, kind) == 1.0f);
		printf("kind %u: fog at spawn + 0.3 s %.3f, far size x%.2f\n",
		       kind,
		       (double)tr_scene_ent_fog(z3, 1.0f),
		       (double)tr_scene_ent_scale(z3, kind));
	}
	return 0;
}
