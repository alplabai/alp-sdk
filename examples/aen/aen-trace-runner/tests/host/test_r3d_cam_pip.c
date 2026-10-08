/* tests/host/test_r3d_cam_pip.c -- src/render/cam_pip.c: the camera's cover
 * resample (the 640x400 landscape frame scaled by 32/25 into the 800x512 video
 * area, centre-cropped), the NEON kernel bit-exact against the scalar
 * reference, the source rows a band reads, and the keypoint-to-screen
 * mapping (score gate, the crop, the round trip with the sampling grid).
 * Named test_r3d_*.c so tests/host/runner.sh's "A32 qemu" stage cross-
 * compiles and qemu-runs this file too -- the only place the NEON kernel
 * runs off silicon (a plain x86 host never defines __ARM_NEON; without qemu
 * the runner prints a SKIP for that stage, and the kernel is NOT checked). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/render/cam_pip.h"

#define W  TR_CAM_SRC_W
#define H  TR_CAM_SRC_H
#define VW TR_VID_W
#define VH TR_VID_H

static uint8_t                          src[W * H];
static uint16_t                         want[VH * VW];
__attribute__((unused)) static uint16_t got[VH * VW + 64]; /* NEON stage only */

static uint8_t grey_of(uint16_t px) /* the green channel (6 bits) back to 8, low bits dropped */
{
	return (uint8_t)(((px >> 5) & 63) << 2);
}

static uint16_t rgb565(uint8_t g)
{
	return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
}

static void fill_hash(void)
{
	for (int i = 0; i < W * H; i++) {
		src[i] = (uint8_t)(i * 2654435761u >> 24);
	}
}

__attribute__((unused)) static void fill_checker(void) /* NEON stage only */
{
	for (int i = 0; i < W * H; i++) {
		src[i] = (uint8_t)(((i % W) ^ (i / W)) & 1 ? 0xFF : 0x00);
	}
}

static void fill_edge_v(void) /* a vertical edge: 0 left of column 320, 255 from it */
{
	for (int i = 0; i < W * H; i++) {
		src[i] = i % W < W / 2 ? 0 : 255;
	}
}

static void fill_edge_h(void) /* a horizontal edge: 0 above row 200, 255 from it */
{
	for (int i = 0; i < W * H; i++) {
		src[i] = i / W < H / 2 ? 0 : 255;
	}
}

/* The sampling grid written out independently of cam_pip.c: output (x, r) reads source position
 * (50 x + 473, 50 r - 7) / 64. Bilinear in doubles, so the integer kernels are checked against the
 * maths, not against themselves; clamped at the frame edge. */
static double ref_sample(int x, int r)
{
	double fx = (50.0 * x + 473.0) / 64.0, fy = (50.0 * r - 7.0) / 64.0;
	int    x0 = (int)(fx < 0 ? -1 : fx), y0 = (int)(fy < 0 ? -1 : fy);
	double ax = fx - x0, ay = fy - y0;
	int    xa = x0 < 0 ? 0 : x0, xb = x0 + 1 > W - 1 ? W - 1 : x0 + 1;
	int    ya = y0 < 0 ? 0 : y0, yb = y0 + 1 > H - 1 ? H - 1 : y0 + 1;
	double top = src[ya * W + xa] * (1 - ax) + src[ya * W + xb] * ax;
	double bot = src[yb * W + xa] * (1 - ax) + src[yb * W + xb] * ax;

	return top * (1 - ay) + bot * ay;
}

int main(void)
{
	/* 1. The geometry the maintainer ruled, pinned as numbers (the header's static asserts say the
	 * same in types): 800 x 512 area, scale 1.28 = 32/25 height-bound, 819.2 px wide so 19.2 px
	 * (9.6 a side) are cropped, every one of the 400 source rows used. */
	assert(VW == 800 && VH == 512 && TR_VID_Y0 == 768);
	assert(TR_CAM_COVER_NUM == 32 && TR_CAM_COVER_DEN == 25);
	assert(W * 32 / 25 == 819 && W * 32 >= VW * 25 && H * 32 == VH * 25);
	assert(tr_cam_cover_sx64(0) == 473 && tr_cam_cover_sx64(1) == 523 &&
	       tr_cam_cover_sx64(799) == 40423);
	assert(tr_cam_cover_sy64(0) == -7 && tr_cam_cover_sy64(1) == 43 &&
	       tr_cam_cover_sy64(511) == 25543);
	/* 32 output columns are exactly 25 source columns (the NEON pattern) */
	assert(tr_cam_cover_sx64(32) - tr_cam_cover_sx64(0) == 25 * 64);
	/* the last right-hand tap of every column and row is inside the frame (columns), or clamped (rows) */
	assert((tr_cam_cover_sx64(VW - 1) >> 6) + 1 <= W - 1);
	assert((tr_cam_cover_sx64(0) >> 6) >= 0);

	/* 2. The scalar kernel against the maths: a constant frame stays constant, the tolerance covers
	 * the two 8-bit roundings and the 5/6-bit RGB565 truncation (the green channel keeps 6 bits). */
	fill_hash();
	tr_cam_cover_rows(src, 0, VH, want, VW);
	{
		int worst = 0;

		for (int r = 0; r < VH; r += 3) {
			for (int x = 0; x < VW; x += 3) {
				double ref = ref_sample(x, r);
				int    d   = abs((int)grey_of(want[r * VW + x]) - (int)ref);

				worst = d > worst ? d : worst;
			}
		}
		assert(worst <= 4); /* 6-bit green truncation loses up to 3, rounding up to 1 */
		printf("cover: scalar within %d grey levels of the real-valued bilinear sample\n", worst);
	}
	memset(src, 0x77, sizeof(src));
	tr_cam_cover_rows(src, 0, VH, want, VW);
	for (int i = 0; i < VH * VW; i++) {
		assert(want[i] == rgb565(0x77));
	}

	/* 3. Where the picture lands: a vertical edge at source column 320 crosses 127.5 at output
	 * column 399.5 -- the area's exact centre, so the crop is symmetric -- and a horizontal edge at
	 * source row 200 crosses at output row 255.5. A wrong offset (the 473 / -7) moves these. */
	fill_edge_v();
	tr_cam_cover_rows(src, 0, 1, want, VW);
	assert(grey_of(want[398]) < 128 && grey_of(want[399]) < 128 && grey_of(want[400]) >= 128 &&
	       grey_of(want[401]) >= 128);
	assert(want[0] == rgb565(0) && want[VW - 1] == rgb565(255)); /* flat out to both edges */
	for (int i = 0; i < 8; i++) { /* mirror-symmetric about the centre, to within a rounding */
		int s = (grey_of(want[399 - i]) >> 2) + (grey_of(want[400 + i]) >> 2);

		assert(s == 62 || s == 63);
	}
	fill_edge_h();
	tr_cam_cover_rows(src, 0, VH, want, VW);
	assert(grey_of(want[254 * VW + 10]) < 128 && grey_of(want[255 * VW + 10]) < 128 &&
	       grey_of(want[256 * VW + 10]) >= 128 && grey_of(want[257 * VW + 10]) >= 128);
	assert(want[0] == rgb565(0) && want[(VH - 1) * VW] == rgb565(255)); /* top and bottom rows */
	printf("cover: edges land at the area's centre (399.5, 255.5), flat to the borders\n");

	/* 4. The source rows a band of output rows reads: inside the frame, never more than the band's
	 * 25.6 + 2 rows, and every row the brute-force taps touch (what the A32 invalidates). */
	for (int r0 = 0; r0 + TR_BAND_H <= VH; r0 += TR_BAND_H) {
		int j0, j1;

		tr_cam_cover_src_rows(r0, TR_BAND_H, &j0, &j1);
		assert(j0 >= 0 && j1 <= H - 1 && j1 - j0 + 1 <= 28);
		for (int r = r0; r < r0 + TR_BAND_H; r++) {
			int v = tr_cam_cover_sy64(r), a = v >> 6, b = a + 1;

			a = a < 0 ? 0 : a;
			b = b > H - 1 ? H - 1 : b;
			assert(a >= j0 && b <= j1);
		}
	}
	{
		int j0, j1;

		tr_cam_cover_src_rows(0, VH, &j0, &j1);
		assert(j0 == 0 && j1 == H - 1); /* the whole area reads the whole frame */
	}
	printf("cover: band source-row spans cover every tap\n");

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	/* 5. The NEON kernel: bit-exact with the scalar reference on a hash, a 0x00/0xFF checker (every
	 * tap pair at full contrast) and both edges, for every 32-row band (the renderer's call), the
	 * whole area at once and ragged windows, with guard words either side that must survive. */
	for (int pat = 0; pat < 4; pat++) {
		static const struct {
			int r0, rows;
		} win[] = { { 0, VH }, { 5, 37 }, { 1, 1 }, { 480, 32 }, { 511, 1 }, { 100, 300 } };

		pat == 0   ? fill_hash()
		: pat == 1 ? fill_checker()
		: pat == 2 ? fill_edge_v()
		           : fill_edge_h();
		memset(want, 0xA5, sizeof(want));
		tr_cam_cover_rows(src, 0, VH, want, VW);
		for (int r0 = 0; r0 < VH; r0 += TR_BAND_H) {
			memset(got, 0xA5, sizeof(got));
			tr_cam_cover_rows_neon(src, r0, TR_BAND_H, got + 8, VW);
			assert(memcmp(got + 8, want + (size_t)r0 * VW, (size_t)TR_BAND_H * VW * 2u) == 0);
			assert(got[0] == 0xA5A5u && got[7] == 0xA5A5u);     /* before the first row */
			assert(got[8 + (size_t)TR_BAND_H * VW] == 0xA5A5u); /* after the last */
		}
		for (unsigned k = 0; k < sizeof(win) / sizeof(win[0]); k++) {
			memset(got, 0xA5, sizeof(got));
			tr_cam_cover_rows_neon(src, win[k].r0, win[k].rows, got + 8, VW);
			assert(memcmp(got + 8, want + (size_t)win[k].r0 * VW, (size_t)win[k].rows * VW * 2u) ==
			       0);
		}
	}
	printf(
	    "cover NEON: bit-exact with scalar, bands + whole area + ragged windows, guards intact\n");
#else
	printf("!!!!! SKIP: cover NEON kernel not compiled (host build) -- bit-exactness NOT checked "
	       "!!!!!\n");
#endif

	/* 6. Keypoints -> screen: the inverse of the grid, x = (64 kx - 473) / 50 and y = (64 ky + 7) / 50,
	 * to the nearest output pixel. */
	{
		int16_t px = -1, py = -1;
		tr_kp_t k;

		k = (tr_kp_t){ 7, 0, 255 }; /* the first column the 9.6 px crop leaves */
		assert(tr_cam_pip_map_kp(&k, &px, &py) && px == 0 && py == 0);
		k = (tr_kp_t){ 6, 0, 255 }; /* cropped away on the left */
		assert(!tr_cam_pip_map_kp(&k, &px, &py));
		k = (tr_kp_t){ 631, 399, 255 }; /* the last one on the right */
		assert(tr_cam_pip_map_kp(&k, &px, &py) && px == 798 && py == VH - 1);
		k = (tr_kp_t){ 632, 399, 255 }; /* cropped away on the right */
		assert(!tr_cam_pip_map_kp(&k, &px, &py));
		k = (tr_kp_t){ 320,
			           200,
			           255 }; /* the frame's centre: 639.5 / 2 -> the area's, within a pixel */
		assert(tr_cam_pip_map_kp(&k, &px, &py) && px == 400 && py == 256);
		assert(tr_cam_pip_map_kp(&(tr_kp_t){ 100, 50, 255 }, &px, &py) && px == 119 && py == 64);

		/* round trip, every keypoint of the frame: the output pixel it maps to samples the source
		 * within half an output step (25/64 source px) of it, in both axes -- and a kp maps
		 * exactly when that pixel is inside the area. */
		int mapped = 0;

		for (int ky = 0; ky < H; ky++) {
			for (int kx = 0; kx < W; kx++) {
				k  = (tr_kp_t){ (int16_t)kx, (int16_t)ky, 255 };
				px = py = -7;
				if (tr_cam_pip_map_kp(&k, &px, &py)) {
					assert(px >= 0 && px < VW && py >= 0 && py < VH);
					assert(abs(tr_cam_cover_sx64(px) - 64 * kx) <= 25);
					assert(abs(tr_cam_cover_sy64(py) - 64 * ky) <= 25);
					mapped++;
				} else {
					assert(px == -7 && py == -7); /* untouched on a reject */
					assert(kx < 7 || kx > 631);   /* only the crop rejects an in-frame keypoint */
				}
			}
		}
		assert(mapped == (631 - 7 + 1) * H);

		/* and the forward direction: every output pixel's nearest source pixel maps back to within one */
		for (int x = 0; x < VW; x += 5) {
			int kx = (tr_cam_cover_sx64(x) + 32) >> 6;

			k = (tr_kp_t){ (int16_t)kx, 100, 255 };
			assert(tr_cam_pip_map_kp(&k, &px, &py) && abs(px - x) <= 1);
		}
		/* gate and bounds */
		px = py = -7;
		k       = (tr_kp_t){ 300, 200, TR_POSE_KP_MIN - 1 };
		assert(!tr_cam_pip_map_kp(&k, &px, &py) && px == -7 && py == -7);
		k = (tr_kp_t){ -1, 10, 255 };
		assert(!tr_cam_pip_map_kp(&k, &px, &py));
		k = (tr_kp_t){ 640, 10, 255 };
		assert(!tr_cam_pip_map_kp(&k, &px, &py));
		k = (tr_kp_t){ 10, 400, 255 }; /* 400 is past the frame */
		assert(!tr_cam_pip_map_kp(&k, &px, &py));
		k = (tr_kp_t){ 10, -1, 255 };
		assert(!tr_cam_pip_map_kp(&k, &px, &py));
		k.x = 300, k.y = 200, k.score = TR_POSE_KP_MIN;
		assert(tr_cam_pip_map_kp(&k, &px, &py) && px == 375 && py == 256);
		printf("map_kp: inverse of the grid within half a step, crop and low score rejected\n");
	}

	/* 7. fix round 11: tr_cam_pip_format_hz() -- pin the maintainer-reported
	 * exact range (251..266, "25.1..26.6 Hz") plus edge cases. A direct
	 * render of this exact range could not reproduce the reported
	 * "NPU 2.8Hz" (fix round 11 report: renders "26.6Hz" correctly); this
	 * locks the proven-correct formula in against any future regression. */
	{
		char buf[16];
		int  n;

		n = tr_cam_pip_format_hz(251u, buf);
		assert(n == 6 && memcmp(buf, "25.1Hz", 7) == 0); /* n excludes '\0', memcmp includes it */
		n = tr_cam_pip_format_hz(266u, buf);
		assert(n == 6 && memcmp(buf, "26.6Hz", 7) == 0);
		n = tr_cam_pip_format_hz(0u, buf);
		assert(n == 5 && memcmp(buf, "0.0Hz", 6) == 0);
		n = tr_cam_pip_format_hz(9u, buf);
		assert(n == 5 &&
		       memcmp(buf, "0.9Hz", 6) == 0); /* single-digit Hz*10 still gets a "0" whole part */
		n = tr_cam_pip_format_hz(1000u, buf);
		assert(n == 7 && memcmp(buf, "100.0Hz", 8) == 0); /* 3-digit whole part, no truncation */
		printf("format_hz: 251->25.1Hz 266->26.6Hz 0->0.0Hz 9->0.9Hz 1000->100.0Hz\n");
	}

	printf("PASS: tests/host/test_r3d_cam_pip.c\n");
	return 0;
}
