/* src/vision/movenet.c */
#include <string.h>

#include "movenet.h"

static uint32_t isqrt(uint32_t v)
{
	uint32_t r = 0u, bit = 1u << 30;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit != 0u) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* The HP runs with the D-cache off (hp_vision/prj.conf), so every load from the camera pool
 * (SRAM1) or the NPU arena (SRAM0) is its own bus transaction. Both stages below therefore
 * pull what they read into the HP's DTCM (this file's statics live in .bss) in word bursts
 * first and work from there. Output bytes are unchanged: tests/host/test_movenet_input_rot.c
 * holds tr_movenet_input_rot() to the straight per-pixel loads it replaced. */
/* bytes in, words out: not a strict-aliasing bet */
typedef uint32_t __attribute__((may_alias)) word_t;

static void burst_copy(void *dst, const void *src, uint32_t n)
{
	word_t       *d = dst;
	const word_t *s = src;

	if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u) {
		for (; n >= 32u; n -= 32u, d += 8, s += 8) {
			uint32_t a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
			uint32_t a4 = s[4], a5 = s[5], a6 = s[6], a7 = s[7];

			d[0] = a0, d[1] = a1, d[2] = a2, d[3] = a3;
			d[4] = a4, d[5] = a5, d[6] = a6, d[7] = a7;
		}
	}
	memcpy(d, s, n);
}

/* The frame's two source rows of an output row, DTCM. */
static uint8_t g_line[2][TR_CAM_SENSOR_W] __attribute__((aligned(32)));

void tr_movenet_input(const uint8_t *grey, int16_t frame_w, int16_t frame_h, int8_t *in)
{
	tr_movenet_input_rot(grey, frame_w, frame_h, 0, in);
}

void tr_movenet_input_rot(const uint8_t *grey, int16_t src_w, int16_t src_h, int rot, int8_t *in)
{
	int uw = rot != 0 ? src_h : src_w, uh = rot != 0 ? src_w : src_h; /* the upright frame */
	int m    = uw > uh ? uw : uh; /* the long side fills the 192 square */
	int cols = uw * TR_MN_IN / m, rows = uh * TR_MN_IN / m;
	int padx = (TR_MN_IN - cols) / 2, pady = (TR_MN_IN - rows) / 2;
	int sx, sy;
	/* Upright camera, word-aligned rows that fit the line buffers (the release: 640 x 400,
	 * rot 0): burst each output row's two source rows into DTCM, then store packed words. */
	int fast = rot == 0 && src_w <= TR_CAM_SENSOR_W &&
	           (((uintptr_t)grey | (uintptr_t)in | (uintptr_t)src_w) & 3u) == 0u;

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

		if (fast) {
			word_t *o32 = (word_t *)(void *)o;

			if (r < pady || r >= pady + rows) {
				for (int i = 0; i < TR_MN_IN * 3 / 4; i++) {
					o32[i] = 0u;
				}
				continue;
			}
			int uy = (r - pady) * m / TR_MN_IN;
			int y1 = uy + 1 < uh ? uy + 1 : uy;

			burst_copy(g_line[0], grey + uy * src_w, (uint32_t)src_w);
			burst_copy(g_line[1], grey + y1 * src_w, (uint32_t)src_w);

			uint8_t q[TR_MN_IN];

			for (int c = 0; c < TR_MN_IN; c++) {
				if (c < padx || c >= padx + cols) {
					q[c] = 0;
					continue;
				}
				int32_t a = cx0[c], b = cx1[c];
				int     v = (g_line[0][a] + g_line[0][b] + g_line[1][a] + g_line[1][b] + 2) >> 2;

				q[c] = (uint8_t)(v - 128);
			}
			/* 4 pixels = 12 B = 3 words, each pixel's byte three times (little-endian) */
			for (int c = 0; c < TR_MN_IN; c += 4) {
				uint32_t q0 = q[c], q1 = q[c + 1], q2 = q[c + 2], q3 = q[c + 3];

				o32[0] = q0 * 0x010101u | q1 << 24;
				o32[1] = q1 * 0x0101u | q2 * 0x01010000u;
				o32[2] = q2 | q3 * 0x01010100u;
				o32 += 3;
			}
			continue;
		}

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

/* (q - zp) * scale_q16, in 1/16 cell, rounded to nearest. */
static int32_t q16th(int8_t q, int zp, int32_t scale)
{
	int32_t v = (q - zp) * scale; /* Q16 cells; |v| < 2^24 */

	return (v >= 0) ? (v + 2048) >> 12 : -((-v + 2048) >> 12);
}

/* heat / (distance to the guess + 1.8), Q16. */
static uint32_t score_at(const tr_movenet_out_t *o, int k, int j, int32_t ry, int32_t rx)
{
	uint32_t h  = (uint32_t)(o->heat[j * TR_POSE_KP + k] + 128);
	int32_t  dy = (j / TR_MN_GRID) * 16 - ry;
	int32_t  dx = (j % TR_MN_GRID) * 16 - rx;

	return (h << 16) / (isqrt((uint32_t)(dy * dy + dx * dx)) + 29u);
}

/* The two maps the argmax passes sweep (centre once, heat 17 times, 41,472 B): DTCM copies. */
static int8_t g_centre[TR_MN_CELLS] __attribute__((aligned(32)));
static int8_t g_heat[TR_MN_CELLS * TR_POSE_KP] __attribute__((aligned(32)));

void tr_movenet_decode(const tr_movenet_out_t *src,
                       int16_t                 frame_w,
                       int16_t                 frame_h,
                       tr_pose_t              *out)
{
	burst_copy(g_centre, src->centre, sizeof(g_centre));
	burst_copy(g_heat, src->heat, sizeof(g_heat));

	tr_movenet_out_t  local = *src;
	tr_movenet_out_t *o     = &local;

	local.centre = g_centre;
	local.heat   = g_heat;

	/* 1. The person centre: first maximum, as TFLite ARG_MAX picks. */
	int ci = 0;

	for (int j = 1; j < TR_MN_CELLS; j++) {
		if (o->centre[j] > o->centre[ci]) {
			ci = j;
		}
	}

	/* The letterbox tr_movenet_input_rot() applied: the long side fills the
	 * square, the short one is padded both sides (1/16 input px). */
	int32_t m      = frame_w > frame_h ? frame_w : frame_h;
	int32_t padx16 = (TR_MN_IN - (int32_t)frame_w * TR_MN_IN / m) * 16 / 2;
	int32_t pady16 = (TR_MN_IN - (int32_t)frame_h * TR_MN_IN / m) * 16 / 2;

	for (int k = 0; k < TR_POSE_KP; k++) {
		/* 2. First guess from the centre's regression, 1/16 cell. */
		int32_t ry = (ci / TR_MN_GRID) * 16 +
		             q16th(o->regress[ci * 34 + 2 * k], TR_MN_REG_ZP, TR_MN_REG_SCALE);
		int32_t rx = (ci % TR_MN_GRID) * 16 +
		             q16th(o->regress[ci * 34 + 2 * k + 1], TR_MN_REG_ZP, TR_MN_REG_SCALE);

		/* 3. argmax of heat / (distance + 1.8), distance in 1/16 cell
		 * (1.8 cells = 29/16), first maximum on ties. Seeded with the cell
		 * under the guess -- nearly always a strong candidate -- so the
		 * skip below (a heat that could not win even at distance 0) spares
		 * almost every cell its square root and divide. */
		int32_t  gy = (ry + 8) / 16, gx = (rx + 8) / 16;
		int      bj   = (gy < 0                ? 0
		                 : gy > TR_MN_GRID - 1 ? TR_MN_GRID - 1
		                                       : gy) *
		                    TR_MN_GRID +
		                (gx < 0                ? 0
		                 : gx > TR_MN_GRID - 1 ? TR_MN_GRID - 1
		                                       : gx);
		uint32_t best = score_at(o, k, bj, ry, rx);
		/* (h << 16) / 29u < best, algebraically, for non-negative integer
		 * division: a/29 < b  <=>  a < 29*b (a/29 floors, so a/29 <= b-1
		 * exactly when a <= 29*(b-1)+28 = 29b-1 < 29b, and the converse
		 * holds the same way) -- fix round 5, the design-vs-measured
		 * decode_us gap (~10ms measured vs an unstated <=1ms estimate):
		 * the ORIGINAL form ran this division on EVERY ONE of the
		 * TR_MN_CELLS * TR_POSE_KP (2304 * 17 = 39,168) cell/keypoint
		 * pairs every frame, unconditionally, for a check the design's
		 * own comment says should almost always short-circuit before the
		 * expensive isqrt below. Recomputed only when best actually
		 * changes (rare -- 126-614 of 39,168 cells per the design doc),
		 * not once per cell: a multiply is far cheaper than a divide on
		 * this core, and this removes ~39,168 divides/frame down to a
		 * handful. */
		uint32_t best_x29 = best * 29u;

		for (int j = 0; j < TR_MN_CELLS; j++) {
			uint32_t h = (uint32_t)(o->heat[j * TR_POSE_KP + k] + 128);

			if ((h << 16) < best_x29 || j == bj) {
				continue;
			}
			uint32_t score = score_at(o, k, j, ry, rx);

			if (score > best || (score == best && j < bj)) {
				best     = score;
				best_x29 = best * 29u;
				bj       = j;
			}
		}

		/* 4. Cell + sub-cell offset -> 192-square px (4 per cell) -> frame px. */
		int32_t y16 = (bj / TR_MN_GRID) * 16 +
		              q16th(o->offset[bj * 34 + 2 * k], TR_MN_OFF_ZP, TR_MN_OFF_SCALE);
		int32_t x16 = (bj % TR_MN_GRID) * 16 +
		              q16th(o->offset[bj * 34 + 2 * k + 1], TR_MN_OFF_ZP, TR_MN_OFF_SCALE);
		int32_t h   = o->heat[bj * TR_POSE_KP + k] + 128;

		/* x16 * 4 = 1/16 input px; (input px - pad) * m / 192 = frame px. */
		out->kp[k].x     = (int16_t)(((x16 * 4 - padx16) * m + 8 * TR_MN_IN) / (16 * TR_MN_IN));
		out->kp[k].y     = (int16_t)(((y16 * 4 - pady16) * m + 8 * TR_MN_IN) / (16 * TR_MN_IN));
		out->kp[k].score = (uint8_t)(h > 255 ? 255 : h);
	}
}
