/* tests/host/test_movenet_input_rot.c -- src/vision/movenet.c's tr_movenet_input_rot() (the HP's
 * DTCM line-buffer + packed-word store path) is byte-identical to the straight per-pixel loads
 * it replaced, which this file keeps verbatim as ref_input_rot(): upright and turned frames,
 * word-aligned (the fast path) and not (the fallback), full-range random pixels and the
 * extremes. Its own bite check: a one-bit difference in the output is found. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/vision/movenet.h"

#define IN_BYTES (TR_MN_IN * TR_MN_IN * 3)

/* The pre-DTCM implementation, unchanged. */
static void ref_input_rot(const uint8_t *grey, int16_t src_w, int16_t src_h, int rot, int8_t *in)
{
	int uw = rot != 0 ? src_h : src_w, uh = rot != 0 ? src_w : src_h; /* the upright frame */
	int m    = uw > uh ? uw : uh; /* the long side fills the 192 square */
	int cols = uw * TR_MN_IN / m, rows = uh * TR_MN_IN / m;
	int padx = (TR_MN_IN - cols) / 2, pady = (TR_MN_IN - rows) / 2;
	int sx, sy;

	/* The rotation is affine, so a raw byte offset splits into an upright-x
	 * part plus an upright-y part: off(ux, uy) = off(ux, 0) + off(0, uy) -
	 * off(0, 0). Tabled once per column (fix round 5's reason: TR_MN_IN
	 * divisions, not one per pixel); each row adds its own part. The 2x2
	 * mean is taken over UPRIGHT neighbours, which are raw neighbours too
	 * (a rotation keeps adjacency), so rotating costs no extra pass and no
	 * copy of the frame. */
	tr_cam_rot_src(rot, src_w, src_h, 0, 0, &sx, &sy);
	int32_t base = sy * src_w + sx;
	int32_t cx0[TR_MN_IN], cx1[TR_MN_IN];

	for (int c = 0; c < TR_MN_IN; c++) {
		int ux = c < padx
		             ? 0
		             : (c - padx) * m / TR_MN_IN; /* padding columns: never read, kept in range */
		int u1 = ux + 1 < uw ? ux + 1 : ux;

		tr_cam_rot_src(rot, src_w, src_h, ux, 0, &sx, &sy);
		cx0[c] = sy * src_w + sx - base;
		tr_cam_rot_src(rot, src_w, src_h, u1, 0, &sx, &sy);
		cx1[c] = sy * src_w + sx - base;
	}

	for (int r = 0; r < TR_MN_IN; r++) {
		int8_t *o = in + r * TR_MN_IN * 3;

		if (r < pady || r >= pady + rows) {
			memset(o, 0, (size_t)TR_MN_IN * 3);
			continue;
		}
		int uy = (r - pady) * m / TR_MN_IN;
		int y1 = uy + 1 < uh ? uy + 1 : uy;

		tr_cam_rot_src(rot, src_w, src_h, 0, uy, &sx, &sy);
		const uint8_t *row0 = grey + (sy * src_w + sx);
		tr_cam_rot_src(rot, src_w, src_h, 0, y1, &sx, &sy);
		const uint8_t *row1 = grey + (sy * src_w + sx);

		for (int c = 0; c < TR_MN_IN; c++) {
			if (c < padx || c >= padx + cols) {
				o[3 * c] = o[3 * c + 1] = o[3 * c + 2] = 0;
				continue;
			}
			int32_t a = cx0[c], b = cx1[c];
			int     v = (row0[a] + row0[b] + row1[a] + row1[b] + 2) >> 2;
			int8_t  q = (int8_t)(v - 128);

			o[3 * c] = o[3 * c + 1] = o[3 * c + 2] = q;
		}
	}
}

static uint8_t raw_mem[644 * 400 + 64] __attribute__((aligned(64)));
static int8_t  want_mem[IN_BYTES + 64] __attribute__((aligned(64)));
static int8_t  got_mem[IN_BYTES + 64] __attribute__((aligned(64)));

static uint32_t rng = 0x12345678u;

static uint8_t rnd8(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (uint8_t)(rng >> 24);
}

/* One case; `ro` / `io` offset the frame / tensor pointers off their 4-byte alignment. */
static void run(int w, int h, int rot, int ro, int io, int mode)
{
	uint8_t *raw  = raw_mem + ro;
	int8_t  *want = want_mem + io, *got = got_mem + io;

	for (int i = 0; i < w * h; i++) {
		raw[i] = mode == 0   ? rnd8()
		         : mode == 1 ? (uint8_t)((i & 1) ? 255 : 0)
		                     : (uint8_t)(i % 7 == 0 ? 255 : 0);
	}
	memset(want, 0x55, IN_BYTES);
	memset(got, 0x55, IN_BYTES);
	ref_input_rot(raw, (int16_t)w, (int16_t)h, rot, want);
	tr_movenet_input_rot(raw, (int16_t)w, (int16_t)h, rot, got);
	if (memcmp(want, got, IN_BYTES) != 0) {
		printf("MISMATCH %dx%d rot %d ro %d io %d mode %d\n", w, h, rot, ro, io, mode);
		exit(1);
	}
}

int main(void)
{
	/* 644 x 400 is word-aligned but wider than the line buffers: the src_w <= TR_CAM_SENSOR_W guard. */
	static const struct {
		int w, h;
	} sz[] = {
		{ 640, 400 }, { 400, 640 }, { 320, 200 }, { 192, 192 },
		{ 600, 400 }, { 640, 360 }, { 100, 640 }, { 644, 400 },
	};

	for (unsigned s = 0; s < sizeof(sz) / sizeof(sz[0]); s++) {
		for (int rot = 0; rot <= 270; rot += 90) {
			if (rot == 180) continue;
			for (int mode = 0; mode < 3; mode++) {
				run(sz[s].w, sz[s].h, rot, 0, 0, mode); /* aligned: the fast path at rot 0 */
			}
		}
	}
	/* Off the word grid: the fallback. */
	run(640, 400, 0, 1, 0, 0);
	run(640, 400, 0, 0, 1, 0);
	run(637, 400, 0, 0, 0, 0);
	run(639, 399, 0, 2, 3, 0);
	run(641, 400, 0, 0, 0, 0); /* wider, and off the word grid */
	run(640, 400, 90, 1, 2, 0);

	/* Bite: the harness sees a single flipped output bit. */
	{
		for (int i = 0; i < 640 * 400; i++)
			raw_mem[i] = rnd8();
		ref_input_rot(raw_mem, 640, 400, 0, want_mem);
		tr_movenet_input_rot(raw_mem, 640, 400, 0, got_mem);
		assert(memcmp(want_mem, got_mem, IN_BYTES) == 0);
		got_mem[50 * TR_MN_IN * 3 + 7] ^= 1;
		assert(memcmp(want_mem, got_mem, IN_BYTES) != 0);
	}
	printf("movenet input rot: byte-identical to the per-pixel reference\n");
	return 0;
}
