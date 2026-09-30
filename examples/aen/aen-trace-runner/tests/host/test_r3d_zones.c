/* tests/host/test_r3d_zones.c -- world zones in the scene (P15): the packet
 * (an old HE's frames are the board, bit for bit), the gate and the look's
 * blend (monotonic, no pop, done before the gate is dropped), every zone's
 * budget (DL, bins, nothing under the board), its parts (on the board, far
 * LODs cheaper, cull spheres that bound them, the cull never changes a
 * pixel), a golden frame per zone and one mid-blend (also under A32 qemu:
 * the same bits on the target), and -- built with -DTR_RASTER_PROF=1
 * (runner.sh) -- each zone's raster work against the board's, at the
 * silicon unit costs. TR_DUMP=/dir/prefix- writes the frames as PPM. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/game/zone.h"
#include "../../src/render/proj.h"
#include "../../src/render/r3d_scene.h"
#include "../../src/render/zones.h"
#include "tr_scene_golden.h"

#define W TR_R3D_W
#define H TR_R3D_H

static uint16_t       fb[W * H], ref[W * H];
static uint16_t       zb[W * TR_BAND_H], cb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl;

#ifdef TR_RASTER_PROF
static tr_prof_t prof[TR_PROF_N];

tr_prof_t *tr_prof_core(void)
{
	return prof;
}

uint32_t tr_prof_now(void)
{
	return 0u;
}
#endif

static uint32_t crc32(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t       c = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		c ^= b[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
		}
	}
	return ~c;
}

/* Bin + raster the built DL the A32 way; returns the fullest band's bin. */
static uint32_t render(const tr_cam_t *cam, const tr_frame_in_t *in, tr_bg_t *bgo)
{
	uint32_t overflow = 0, worst = 0;
	tr_bg_t  bg;

	tr_scene_bg(in, cam, &bg);
	tr_scene_bg_flash(in, &bg);
	tr_bin_build(&dl, setup, bins, counts, &overflow);
	for (int b = 0; b < TR_BANDS; b++) {
		worst = counts[b] > worst ? counts[b] : worst;
		/* fix round 8: clip the last band to TR_VIEW_H (REQUIRED: bins[]
		 * is sized to TR_BANDS now), matching render_band() (r3d.h). */
		int y_hi = (b + 1) * TR_BAND_H < TR_VIEW_H ? (b + 1) * TR_BAND_H : TR_VIEW_H;

		tr_raster_band(fb, W, b * TR_BAND_H, y_hi, zb, cb, &bg, &dl, setup, bins[b], counts[b]);
	}
	/* fix round 8: zero the ragged last band's stale reused-buffer tail so
	 * repeated render() calls never depend on band-buffer call history. */
	memset(&fb[TR_VIEW_H * W], 0, (size_t)(H - TR_VIEW_H) * W * sizeof(fb[0]));
	assert(overflow == 0);
	if (bgo) {
		*bgo = bg;
	}
	return worst;
}

static void dump(const char *name)
{
	const char *pre = getenv("TR_DUMP");
	char        path[256];

	if (pre == NULL || pre[0] != '/') {
		return;
	}
	snprintf(path, sizeof(path), "%s%s.ppm", pre, name);
	FILE *f = fopen(path, "wb");

	assert(f);
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint8_t rgb[3] = { (uint8_t)((fb[i] >> 11) << 3),
			               (uint8_t)(((fb[i] >> 5) & 63) << 2),
			               (uint8_t)((fb[i] & 31) << 3) };

		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* The golden run in `zone`, gate at model y gy (TR_ZONE_NO_GATE: none). */
static tr_frame_in_t zone_in(uint32_t tick, uint8_t zone, int16_t gy)
{
	tr_frame_in_t in = tr_scene_golden_in(tick, 1);

	in.flags |= TR_FLAG_ZONE;
	in.zone   = zone;
	in.gate_y = gy;
	return in;
}

/* Lowest world y of any DL vertex (inverse projection needs the camera;
 * the scene checks this per zone the way test_r3d_scene.c does: through
 * every emitted mesh instance's own data instead, see 4). */
static int16_t mesh_min_y(const tr_mesh_t *m)
{
	int16_t lo = 32767;

	for (int i = 0; i < m->nv; i++) {
		lo = m->v[i * 3 + 1] < lo ? m->v[i * 3 + 1] : lo;
	}
	return lo;
}

static float delta565(uint16_t a, uint16_t b)
{
	int d[3] = { (int)(a >> 11) - (int)(b >> 11),
		         (int)((a >> 5) & 63) - (int)((b >> 5) & 63),
		         (int)(a & 31) - (int)(b & 31) };
	int m    = 0;

	for (int k = 0; k < 3; k++) {
		m = abs(d[k]) > m ? abs(d[k]) : m;
	}
	return (float)m;
}

int main(void)
{
	static tr_scene_t s;
	tr_cam_t          cam;
	const int16_t     line = tr_runner_ground_y(H);

	/* 1. The packet. An old HE never sets TR_FLAG_ZONE: whatever its pad
	 * bytes hold, the frame is the board -- the scene golden, bit for bit;
	 * and so is the new HE's zone 0 with no gate on the track. */
	{
		tr_frame_in_t old = tr_scene_golden_in(1234, 1), z0 = zone_in(1234, 0, TR_ZONE_NO_GATE);

		old.zone   = 3;
		old.gate_y = 500;
		for (int k = 0; k < 2; k++) {
			tr_frame_in_t *in = k ? &z0 : &old;

			tr_scene_init(&s);
			tr_scene_step(&s, in);
			tr_scene_build(&s, in, &cam, &dl);
			render(&cam, in, NULL);
			assert(crc32(fb, sizeof(fb)) == TR_SCENE_GOLDEN_CRC);
		}
		printf("zones: no TR_FLAG_ZONE, and zone 0 without a gate == the scene golden %08x\n",
		       TR_SCENE_GOLDEN_CRC);
	}

	/* 2. The gate and the blend, as the HE ships them (tr_zone_t stepped
	 * with the game at the 30 Hz play pace, the sub-tick phase between):
	 * near / far follow the gate, the blend rises monotonically from 0 to 1
	 * across the gate's pass with no step bigger than a smoothstep over
	 * ~1 s allows, the sky / glow / halo never jump, it is done while the
	 * gate is still reported, and dropping the gate changes nothing. */
	for (uint8_t z0 = 0; z0 < TR_ZONES; z0++) {
		tr_zone_t z;
		uint32_t  ph = 0, frames = 0, blend = 0;
		float     t_prev = 0.0f;
		tr_bg_t   b_prev = { 0 };
		int       have = 0, entered = 0;

		tr_zone_reset(&z);
		z.zone  = z0;
		z.steps = TR_ZONE_STEPS_PLAY - tr_zone_gate_lead(H) - 2u;
		tr_scene_init(&s);
		for (int f = 0; f < 400; f++) {
			ph += 43691u; /* TR_PLAY_SPEED_Q16 at 30 Hz */
			if (ph >= 65536u) {
				ph -= 65536u;
				entered += tr_zone_step(&z, false, H);
			}
			tr_frame_in_t in = zone_in(3000u + (uint32_t)f, z.zone, z.gate_y);
			uint8_t       nz, fz;
			float         gz, t;

			in.flags |= TR_FLAG_PHASE;
			in.phase = (uint16_t)ph;
			in.hz    = 30;
			t        = tr_scene_zone(&in, &nz, &fz, &gz);
			if (z.gate_y == TR_ZONE_NO_GATE) {
				assert(nz == fz && t == 0.0f && nz == z.zone);
			} else if (z.gate_y < line) {
				assert(nz == z.zone && fz == (z.zone + 1u) % TR_ZONES);
			} else {
				assert(fz == z.zone && nz == (z.zone + TR_ZONES - 1u) % TR_ZONES);
			}
			/* the look, as blended toward the zone now ahead */
			float tl = z.gate_y == TR_ZONE_NO_GATE ? (entered ? 1.0f : 0.0f) : t;

			assert(tl >= t_prev - 1e-6f && tl - t_prev <= 0.06f);
			blend += tl > 0.0f && tl < 1.0f;
			t_prev = tl;
			tr_scene_step(&s, &in);
			tr_scene_build(&s, &in, &cam, &dl);

			tr_bg_t bg;

			tr_scene_bg(&in, &cam, &bg);
			if (have) {
				float d = delta565(bg.top, b_prev.top);

				d = delta565(bg.mid, b_prev.mid) > d ? delta565(bg.mid, b_prev.mid) : d;
				d = delta565(bg.bot, b_prev.bot) > d ? delta565(bg.bot, b_prev.bot) : d;
				d = delta565(bg.halo, b_prev.halo) > d ? delta565(bg.halo, b_prev.halo) : d;
				assert(d <= 4.0f); /* <= 4 levels of a 5/6-bit channel a frame */
			}
			b_prev = bg, have = 1;
			frames++;
			if (entered && z.gate_y == TR_ZONE_NO_GATE) {
				break;
			}
		}
		assert(entered == 1 && z.gate_y == TR_ZONE_NO_GATE && t_prev == 1.0f);
		assert(blend >= 24u && blend <= 40u); /* ~1 s at 30 Hz */
		printf("zones: %u -> %u blend over %u of %u frames at 30 Hz, no step\n",
		       (unsigned)z0,
		       (unsigned)z.zone,
		       (unsigned)blend,
		       (unsigned)frames);
	}

	/* 3. Every zone's parts: on the board, far LODs cheaper, the right-hand
	 * copy the left mirrored, and the cull sphere bounds every vertex with
	 * the 10 % the projection's understatement needs. */
	for (int i = 0; i < TR_ZPART_N; i++) {
		const tr_zpart_t *p = &tr_zpart[i];

		for (int side = 0; side < 2; side++) {
			for (int lo = 0; lo < 2; lo++) {
				const tr_mesh_t *m = p->m[side][lo];

				if (m == NULL) {
					continue;
				}
				assert(mesh_min_y(m) >= 0);
				for (int v = 0; v < m->nv; v++) {
					float x = m->v[v * 3], y = (float)m->v[v * 3 + 1] - p->cy, zz = m->v[v * 3 + 2];

					assert((x * x + y * y + zz * zz) * 1.21f <= p->r * p->r);
					assert(m->v[v * 3] == -p->m[!side][lo]->v[v * 3]);
				}
			}
		}
		assert(p->m[0][1] == NULL || p->m[0][1]->nt < p->m[0][0]->nt);
	}
	assert(mesh_min_y(&tr_zmesh_gate) >= 0 && mesh_min_y(&tr_zmesh_gate_glow) >= 0);
	printf("zones: %d shoulder parts on the board, bounded, LODs cheaper\n", TR_ZPART_N);

	/* 4. Budget per zone: a stretch of scrolling with the golden run, the
	 * worst entity load, and the gate at every depth: inside the DL, no
	 * drop, every band's bin fits; and culled == unculled. */
	{
		uint32_t worst_all = 0;

		for (uint8_t zn = 0; zn < TR_ZONES; zn++) {
			uint32_t most = 0, worst = 0, sum = 0, nf = 0;

			tr_scene_init(&s);
			for (uint32_t t = 0; t < 600; t += 7) {
				int16_t       gy = (t / 7) % 3 == 0
				                       ? (int16_t)(TR_ZONE_GATE_Y + (int32_t)(t * 3u % 2470u))
				                       : TR_ZONE_NO_GATE;
				tr_frame_in_t in = zone_in(t, zn, gy);

				if ((t / 7) % 5 == 4) { /* all 16 slots live obstacles, near */
					for (int i = 0; i < 16; i++) {
						in.ents[i] = (tr_pkt_ent_t){ (uint8_t)(1 + (i & 1) * 2), (uint8_t)(i % 3),
							                         (uint8_t)(i & 1),           0,
							                         (int16_t)(700 + 25 * i),    0 };
					}
				}
				in.lane = (uint8_t)((t / 50) % 3);
				tr_scene_step(&s, &in);
				tr_scene_build(&s, &in, &cam, &dl);
				assert(dl.n < TR_DL_MAX_TRIS && tr_dl_dropped == 0);
				uint32_t b = render(&cam, &in, NULL);

				worst = b > worst ? b : worst;
				most  = dl.n > most ? dl.n : most;
				sum += dl.n, nf++;
			}
			/* the cull never changes a pixel (tr_scene_wall_r huge = off) */
			for (int f = 0; f < 6; f++) {
				tr_frame_in_t in = zone_in(200u + 37u * (uint32_t)f,
				                           zn,
				                           f & 1 ? (int16_t)(300 + 150 * f) : TR_ZONE_NO_GATE);

				tr_scene_init(&s);
				tr_scene_step(&s, &in);
				in.lane = (uint8_t)(f % 3);
				tr_scene_step(&s, &in);
				tr_scene_wall_r = 1e30f;
				tr_scene_build(&s, &in, &cam, &dl);
				render(&cam, &in, NULL);
				memcpy(ref, fb, sizeof(fb));
				tr_scene_wall_r = TR_WALL_R;
				tr_scene_build(&s, &in, &cam, &dl);
				render(&cam, &in, NULL);
				assert(memcmp(ref, fb, sizeof(fb)) == 0);
			}
			printf("zones: %-13s %u tris mean, %u max, max band bin %u of %u; culled == unculled\n",
			       tr_zone_name(zn),
			       (unsigned)(sum / nf),
			       (unsigned)most,
			       (unsigned)worst,
			       TR_BIN_MAX);
			assert(worst < TR_BIN_MAX);
			worst_all = worst > worst_all ? worst : worst_all;
		}
		(void)worst_all;
	}

	/* 5. Golden frames: each zone mid-zone, and the die city's gate mid-way
	 * through its blend (both zones' ground, walls, far layers on screen). */
	{
		static const uint32_t gold[TR_ZONES] = TR_ZONE_GOLDEN_CRC;
		uint32_t              crc;
		char                  name[32];

		for (uint8_t zn = 0; zn < TR_ZONES; zn++) {
			tr_frame_in_t in = zone_in(1234, zn, TR_ZONE_NO_GATE);

			tr_scene_init(&s);
			tr_scene_step(&s, &in);
			tr_scene_build(&s, &in, &cam, &dl);
			render(&cam, &in, NULL);
			crc = crc32(fb, sizeof(fb));
			snprintf(name, sizeof(name), "zone-%u", (unsigned)zn);
			dump(name);
			printf("zones: golden %-13s %08x (%u tris)\n", tr_zone_name(zn), (unsigned)crc, dl.n);
#ifndef TR_ZONES_PRINT_ONLY /* tools: print the goldens only */
			assert(crc == gold[zn]);
#else
			(void)gold;
#endif
		}
		tr_frame_in_t in = zone_in(1234, TR_ZONE_DIE, 1080); /* just before the line: mid-blend */
		uint8_t       nz, fz;
		float         gz;

		in.flags |= TR_FLAG_PHASE, in.phase = 0x8000;
		assert(tr_scene_zone(&in, &nz, &fz, &gz) > 0.3f && nz == TR_ZONE_DIE && fz == TR_ZONE_MEM);
		tr_scene_init(&s);
		tr_scene_step(&s, &in);
		tr_scene_build(&s, &in, &cam, &dl);
		render(&cam, &in, NULL);
		crc = crc32(fb, sizeof(fb));
		dump("zone-blend");
		printf("zones: golden die -> canyon mid-blend %08x (%u tris)\n", (unsigned)crc, dl.n);
#ifndef TR_ZONES_PRINT_ONLY
		assert(crc == TR_ZONE_BLEND_GOLDEN_CRC);
#endif
	}

#ifdef TR_RASTER_PROF
	/* 6. Raster work per zone against the board's, from the TR_PROF_*
	 * counters the silicon prof build reads (a32/renderer `make prof`,
	 * decode.py --prof), at its unit costs (docs/2026-09-22-measurements.md,
	 * 800 MHz, both cores share the frame): NOZ_TEX 21.4 cyc/px; TRI 134
	 * cyc a (triangle, row) over its spans; SETUP 1.53 us a triangle; the
	 * z-tested GOURAUD / FLAT and the NOZ_FILL spans 4.5 cyc/px in their
	 * 8-px NEON chunks plus a per-span cost fitted so the board's Gouraud
	 * comes out at its measured 3.06 ms/core (P12, 28 cyc/px averaged over
	 * short spans). Over a stretch of play with the golden run, averaged. */
	{
		double base = 0.0, span_cyc = 0.0;

		/* zn < TR_ZONES: mid-zone; zn - TR_ZONES: the gate into zone zn + 1
		 * at every depth from the haze to past the runner */
		for (uint8_t zn = 0; zn < 2 * TR_ZONES; zn++) {
			uint64_t px_tex = 0, px_s = 0, spans = 0, rows = 0, tris = 0, nf = 0;

			tr_scene_init(&s);
			for (uint32_t t = 0; t < 300; t += 10) {
				tr_frame_in_t in = zn < TR_ZONES
				                       ? zone_in(t, zn, TR_ZONE_NO_GATE)
				                       : zone_in(t,
				                                 (uint8_t)(zn - TR_ZONES),
				                                 (int16_t)(TR_ZONE_GATE_Y + (int32_t)t * 8));

				memset(prof, 0, sizeof(prof));
				tr_scene_step(&s, &in);
				tr_scene_build(&s, &in, &cam, &dl);
				render(&cam, &in, NULL);
				px_tex += prof[TR_PROF_NOZ_TEX].px;
				px_s +=
				    prof[TR_PROF_GOURAUD].px + prof[TR_PROF_FLAT].px + prof[TR_PROF_NOZ_FILL].px;
				spans += prof[TR_PROF_GOURAUD].n + prof[TR_PROF_FLAT].n + prof[TR_PROF_NOZ_FILL].n;
				rows += prof[TR_PROF_TRI].px;
				tris += dl.n, nf++;
			}
			if (zn == 0) { /* calibrate: the board's spans cost its measured 3.06 ms/core */
				span_cyc = (3.06 * 800e3 * 2.0 * (double)nf - (double)px_s * 4.5) / (double)spans;
			}
			double cyc = (double)px_tex * 21.4 + (double)px_s * 4.5 + (double)spans * span_cyc +
			             (double)rows * 134.0 + (double)tris * 1.53 * 800.0;
			double ms  = cyc / 800e3 / (double)nf / 2.0;

			base = zn == 0 ? ms : base;
			printf("zones: cost %-13s%s tex %6.0f px, spans %6.0f px in %5.0f, %5.0f rows, %4.0f "
			       "tris a frame: "
			       "%.2f ms/core est (%+.2f vs the board; span %.0f cyc)\n",
			       tr_zone_name(zn % TR_ZONES),
			       zn < TR_ZONES ? "      " : " +gate",
			       (double)px_tex / (double)nf,
			       (double)px_s / (double)nf,
			       (double)spans / (double)nf,
			       (double)rows / (double)nf,
			       (double)tris / (double)nf,
			       ms,
			       ms - base,
			       span_cyc);
			assert(ms - base < 4.0); /* the P15 budget: ~+4 ms/core over the board */
		}
	}
#endif
	return 0;
}
