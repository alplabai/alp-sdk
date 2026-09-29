/* tools/sharp_preview.c -- how smooth the ground art reads, per zone, for art
 * review (tools/genzone.py --preset): the golden run's frame (running
 * straight, tr_scene_golden_in(), the shimmer test's drive) rendered the A32
 * way and as a 16x supersampled ground truth (the DL shifted by 4 x 4
 * quarter-pixel offsets, averaged -- test_scene_shimmer.c's). Not a test.
 *
 *   cc -std=c11 -O2 -ffp-contract=off -Isrc -Itests/host -o /tmp/sharp_preview tools/sharp_preview.c \
 *      <every src .c the host tests link: tests/host/runner.sh> -lm
 *   /tmp/sharp_preview OUTDIR    OUTDIR/sharp-<z>.ppm (render), OUTDIR/ssaa-<z>.ppm (truth) and a line per zone:
 *     aliased  % of the frame's pixels whose luminance is >= ALIAS off the 16x truth (jaggies, blocky texels)
 *     ground   the same over the ground only (near + mid: 250..2200 deep)
 *     contrast mean local contrast of that ground: |dL| to the right and below neighbours, averaged (0..255)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/state.h"
#include "game/zone.h"
#include "render/r3d_scene.h"
#include "tr_scene_golden.h"

#define W     TR_R3D_W
#define H     TR_R3D_H
#define NF    13 /* frames run in; the last is measured */
#define ALIAS 16.0f

static uint16_t       fb[W * H], idb[W * H];
static uint16_t       zb[W * TR_BAND_H], cb[W * TR_BAND_H];
static uint16_t       bins[TR_BANDS][TR_BIN_MAX];
static uint32_t       counts[TR_BANDS];
static tr_tri_setup_t setup[TR_DL_MAX_TRIS];
static tr_dl_t        dl, dls;
static float          truth[W * H][3];
static uint8_t        ground[W * H];

static void raster(uint16_t *f, const tr_dl_t *d, const tr_bg_t *bg)
{
	uint32_t ov = 0;

	tr_bin_build(d, setup, bins, counts, &ov);
	for (int b = 0; b < TR_BANDS; b++) {
		tr_raster_band(f, W, b * TR_BAND_H, (b + 1) * TR_BAND_H, zb, cb, bg, d, setup, bins[b], counts[b]);
	}
}

static void rgb(uint16_t p, float c[3])
{
	c[0] = (float)((p >> 11) << 3), c[1] = (float)(((p >> 5) & 63) << 2), c[2] = (float)((p & 31) << 3);
}

static float lum3(const float c[3])
{
	return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

static float lum(uint16_t p)
{
	float c[3];

	rgb(p, c);
	return lum3(c);
}

static void write_ppm(const char *path, int ssaa)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL) {
		perror(path);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		float c[3];

		if (ssaa) {
			memcpy(c, truth[i], sizeof(c));
		} else {
			rgb(fb[i], c);
		}
		for (int k = 0; k < 3; k++) {
			fputc((int)(c[k] + 0.5f), f);
		}
	}
	fclose(f);
}

static void zone(uint8_t zn, const char *dir)
{
	tr_scene_t s;
	tr_cam_t   cam;
	tr_bg_t    bg, b0 = {0};
	char       path[512];

	tr_scene_init(&s);
	for (int n = 0; n < NF; n++) { /* test_scene_shimmer.c run(), straight */
		tr_frame_in_t in = tr_scene_golden_in(3000, 1);
		double        g  = n * 20.0 / 30.0 + 1e-9;
		uint32_t      el = (uint32_t)g;

		in.hz      = 30;
		in.pace_q8 = (uint8_t)((((TR_GAME_PACE_Q8 << 8) * 40u + 15u) / 30u) >> 8);
		in.tick    = 3000 + el;
		in.phase   = (uint16_t)((g - (double)el) * 65536.0);
		in.flags   = TR_FLAG_ALIVE | TR_FLAG_ZONE | (in.phase ? TR_FLAG_PHASE : 0u);
		in.zone    = zn;
		in.gate_y  = TR_ZONE_NO_GATE;
		tr_scene_step(&s, &in);
		if (n == NF - 1) {
			tr_scene_build(&s, &in, &cam, &dl);
			tr_scene_bg(&in, &cam, &bg);
		}
	}
	memset(truth, 0, sizeof(truth));
	for (int j = 0; j < 16; j++) {
		dls = dl;
		for (int t = 0; t < dls.n; t++) {
			for (int k = 0; k < 3; k++) {
				dls.tri[t].v[k].x += (j & 3) * 4 - 6; /* 28.4: +-1/8, +-3/8 px */
				dls.tri[t].v[k].y += (j >> 2) * 4 - 6;
			}
		}
		raster(fb, &dls, &bg);
		for (int i = 0; i < W * H; i++) {
			float c[3];

			rgb(fb[i], c);
			for (int k = 0; k < 3; k++) {
				truth[i][k] += c[k] * (1.0f / 16.0f);
			}
		}
	}
	dls = dl; /* ground: every z-tested tri flat 1, NOZ 0 (the shimmer test's mask) */
	for (int t = 0; t < dls.n; t++) {
		dls.tri[t].c = (dls.tri[t].flags & TR_TRI_NOZ) ? 0 : 1;
		dls.tri[t].flags &= TR_TRI_NOZ;
	}
	raster(idb, &dls, &b0);
	raster(fb, &dl, &bg);

	const float (*m)[4] = cam.view.m;
	float eye[3];

	for (int i = 0; i < 3; i++) {
		eye[i] = -(m[0][i] * m[0][3] + m[1][i] * m[1][3] + m[2][i] * m[2][3]);
	}
	for (int i = 0; i < W * H; i++) {
		float d[3] = {((float)(i % W) + 0.5f - cam.cx) / cam.f_px, -((float)(i / W) + 0.5f - cam.cy) / cam.f_px, 1.0f};
		float dy   = m[0][1] * d[0] + m[1][1] * d[1] + m[2][1] * d[2];
		float t    = dy < 0.0f ? -eye[1] / dy : 0.0f;

		ground[i] = !idb[i] && t >= 250.0f && t < 2200.0f;
	}

	uint64_t al = 0, gal = 0, gn = 0;
	double   con = 0.0;

	for (int i = 0; i < W * H; i++) {
		int bad = fabsf(lum(fb[i]) - lum3(truth[i])) >= ALIAS;

		al += bad;
		if (ground[i] && i % W < W - 1 && i / W < H - 1 && ground[i + 1] && ground[i + W]) {
			gn++;
			gal += bad;
			con += 0.5 * (fabs(lum(fb[i]) - lum(fb[i + 1])) + fabs(lum(fb[i]) - lum(fb[i + W])));
		}
	}
	printf("sharp %-13s aliased %5.2f %% ground %5.2f %% contrast %5.2f\n", tr_zone_name(zn), 100.0 * (double)al / (W * H),
	       gn ? 100.0 * (double)gal / (double)gn : 0.0, gn ? con / (double)gn : 0.0);
	snprintf(path, sizeof(path), "%s/sharp-%u.ppm", dir, zn);
	write_ppm(path, 0);
	snprintf(path, sizeof(path), "%s/ssaa-%u.ppm", dir, zn);
	write_ppm(path, 1);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
		return 2;
	}
	for (uint8_t z = 0; z < TR_ZONES; z++) {
		zone(z, argv[1]);
	}
	return 0;
}
