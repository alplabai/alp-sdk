/* src/render/cam_pip.c -- see cam_pip.h. */
#include "cam_pip.h"

static inline uint16_t grey_to_rgb565(uint8_t g)
{
	return (uint16_t)(((uint32_t)(g >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (g >> 3));
}

/* A source column's two taps and the weight (0..63) of the right one, for output column x. */
static inline int cover_col(int x, int *tap_l, int *tap_r)
{
	int v = tr_cam_cover_sx64(x);

	*tap_l = v >> 6;
	*tap_r = (v >> 6) + 1;
	return v & 63;
}

/* The same lerp as the NEON kernel: (a (64 - w) + b w + 32) >> 6, 8 bits out. */
static inline uint8_t lerp64(uint8_t a, uint8_t b, int w)
{
	return (uint8_t)(((int)a * (64 - w) + (int)b * w + 32) >> 6);
}

/* One source row filtered horizontally to the area's width. */
static void cover_hrow(const uint8_t *src_row, uint8_t *out)
{
	for (int x = 0; x < TR_VID_W; x++) {
		int l, r, w = cover_col(x, &l, &r);

		out[x] = lerp64(src_row[l], src_row[r], w);
	}
}

void tr_cam_cover_rows(const uint8_t *src, int r0, int rows, uint16_t *dst, int dst_stride)
{
	uint8_t h0[TR_VID_W], h1[TR_VID_W];

	for (int i = 0; i < rows; i++) {
		int v = tr_cam_cover_sy64(r0 + i), j = v >> 6, w = v & 63;
		int a = j < 0 ? 0 : j, b = j + 1 > TR_CAM_SRC_H - 1 ? TR_CAM_SRC_H - 1 : j + 1;

		if (j < 0) {
			b = 0; /* above the first row: that row, whatever the weight */
		}
		cover_hrow(src + (uint32_t)a * TR_CAM_SRC_W, h0);
		cover_hrow(src + (uint32_t)b * TR_CAM_SRC_W, h1);
		for (int x = 0; x < TR_VID_W; x++) {
			dst[i * dst_stride + x] = grey_to_rgb565(lerp64(h0[x], h1[x], w));
		}
	}
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* r5 << 11 | g6 << 5 | b5, b5 == r5 for a grey: two shift-and-inserts
 * (vsli keeps the destination's low bits) instead of three shifts + two ORs. */
static inline uint16x8_t grey8_to_rgb565(uint8x8_t g)
{
	uint16x8_t r5 = vmovl_u8(vshr_n_u8(g, 3));
	uint16x8_t g6 = vmovl_u8(vshr_n_u8(g, 2));

	return vsliq_n_u16(vsliq_n_u16(r5, g6, 5), r5, 11);
}

/* The 32-column pattern's taps and weights, from the same grid the scalar reference walks: the
 * compiler folds them to constants. COVER_LIST(F) applies F to 0..31. */
#define COVER_LIST(F) \
	F(0) \
	F(1) \
	F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13) F(14) F(15) F(16) F(17) F(18) \
	    F(19) F(20) F(21) F(22) F(23) F(24) F(25) F(26) F(27) F(28) F(29) F(30) F(31)
#define COVER_TAP_L(x) (uint8_t)(TR_CAM_COVER_SX64(x) >> 6),
#define COVER_TAP_R(x) (uint8_t)((TR_CAM_COVER_SX64(x) >> 6) + 1),
#define COVER_W_R(x)   (uint8_t)(TR_CAM_COVER_SX64(x) & 63),
#define COVER_W_L(x)   (uint8_t)(64 - (TR_CAM_COVER_SX64(x) & 63)),

static const uint8_t cover_tap_l[32] __attribute__((aligned(8))) = { COVER_LIST(COVER_TAP_L) };
static const uint8_t cover_tap_r[32] __attribute__((aligned(8))) = { COVER_LIST(COVER_TAP_R) };
static const uint8_t cover_w_l[32] __attribute__((aligned(8)))   = { COVER_LIST(COVER_W_L) };
static const uint8_t cover_w_r[32] __attribute__((aligned(8)))   = { COVER_LIST(COVER_W_R) };

/* One source row -> the area's width, horizontally filtered. Per 32 output columns: the 32
 * source bytes from column 25 k as a vtbl4 table, the left taps by vtbl4 and the right taps by
 * vtbx4 (tap 32, only output column 31's right neighbour, is not in the table: vtbx leaves the
 * lane at the byte loaded for it), then (a (64 - w) + b w + 32) >> 6. */
static void cover_hrow_neon(const uint8_t *src_row, uint8_t *out)
{
	for (int k = 0; k < TR_VID_W / 32; k++) {
		const uint8_t *p   = src_row + 25 * k;
		uint8x8x4_t    tab = { { vld1_u8(p), vld1_u8(p + 8), vld1_u8(p + 16), vld1_u8(p + 24) } };
		uint8x8_t      nxt = vdup_n_u8(p[32]);

		for (int g = 0; g < 4; g++) {
			uint8x8_t  l   = vtbl4_u8(tab, vld1_u8(cover_tap_l + 8 * g));
			uint8x8_t  r   = vtbx4_u8(nxt, tab, vld1_u8(cover_tap_r + 8 * g));
			uint16x8_t acc = vmull_u8(l, vld1_u8(cover_w_l + 8 * g));

			acc = vmlal_u8(acc, r, vld1_u8(cover_w_r + 8 * g));
			vst1_u8(out + 32 * k + 8 * g, vrshrn_n_u16(acc, 6));
		}
	}
}

void tr_cam_cover_rows_neon(const uint8_t *src, int r0, int rows, uint16_t *dst, int dst_stride)
{
	/* The two most recent source rows, by parity of the row index (consecutive rows differ). */
	uint8_t h[2][TR_VID_W] __attribute__((aligned(16)));
	int     tag[2] = { -1, -1 };

	for (int i = 0; i < rows; i++) {
		int v = tr_cam_cover_sy64(r0 + i), j = v >> 6, w = v & 63;
		int a = j < 0 ? 0 : j,
		    b = j < 0 ? 0 : (j + 1 > TR_CAM_SRC_H - 1 ? TR_CAM_SRC_H - 1 : j + 1);

		if (tag[a & 1] != a) {
			cover_hrow_neon(src + (uint32_t)a * TR_CAM_SRC_W, h[a & 1]);
			tag[a & 1] = a;
		}
		if (tag[b & 1] != b) {
			cover_hrow_neon(src + (uint32_t)b * TR_CAM_SRC_W, h[b & 1]);
			tag[b & 1] = b;
		}

		const uint8_t *ha = h[a & 1], *hb = h[b & 1];
		uint8x8_t      wa = vdup_n_u8((uint8_t)(64 - w)), wb = vdup_n_u8((uint8_t)w);
		uint16_t      *d = dst + (uint32_t)i * (uint32_t)dst_stride;

		for (int x = 0; x < TR_VID_W; x += 16) {
			uint8x16_t va = vld1q_u8(ha + x), vb = vld1q_u8(hb + x);
			uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(va), wa), vget_low_u8(vb), wb);
			uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(va), wa), vget_high_u8(vb), wb);

			vst1q_u16(d + x, grey8_to_rgb565(vrshrn_n_u16(lo, 6)));
			vst1q_u16(d + x + 8, grey8_to_rgb565(vrshrn_n_u16(hi, 6)));
		}
	}
}
#endif

int tr_cam_pip_format_hz(uint32_t loop_hz_x10, char *out)
{
	uint32_t whole = loop_hz_x10 / 10u;
	uint32_t frac  = loop_hz_x10 % 10u;
	char     tmp[10];
	int      wn = 0;
	uint32_t v  = whole;

	do {
		tmp[wn++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u && wn < 10);

	int n = 0;

	for (int i = 0; i < wn; i++) {
		out[n++] = tmp[wn - 1 - i];
	}
	out[n++] = '.';
	out[n++] = (char)('0' + frac);
	out[n++] = 'H';
	out[n++] = 'z';
	out[n]   = '\0';
	return n;
}

/* floor((num + 25) / 50): num / 50 rounded to nearest, ties up, for either sign. */
static inline int round_div50(int num)
{
	int n = num + 25;

	return n >= 0 ? n / 50 : -((-n + 49) / 50);
}

bool tr_cam_pip_map_kp(const tr_kp_t *kp, int16_t *px, int16_t *py)
{
	if (kp->score < TR_POSE_KP_MIN || kp->x < 0 || kp->x >= TR_CAM_SENSOR_W || kp->y < 0 ||
	    kp->y >= TR_CAM_SENSOR_H) {
		return false;
	}
	/* The inverse of tr_cam_cover_sx64()/sy64(): 64 k - offset, over the step. */
	int x = round_div50(64 * kp->x - TR_CAM_COVER_X0);
	int y = round_div50(64 * kp->y - TR_CAM_COVER_Y0);

	if (x < 0 || x >= TR_VID_W || y < 0 || y >= TR_VID_H) {
		return false; /* the crop cut it off */
	}
	*px = (int16_t)x;
	*py = (int16_t)y;
	return true;
}
