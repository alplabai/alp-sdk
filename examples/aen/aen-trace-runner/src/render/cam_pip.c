/* src/render/cam_pip.c -- see cam_pip.h. */
#include "cam_pip.h"

static inline uint16_t grey_to_rgb565(uint8_t g)
{
	return (uint16_t)(((uint32_t)(g >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (g >> 3));
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

void tr_cam_rot_rows_neon(const uint8_t *src,
                          int            rot,
                          int            uy0,
                          int            rows,
                          uint16_t      *dst,
                          int            dst_stride)
{
	enum { W = TR_CAM_SRC_W, H = TR_CAM_SRC_H }; /* upright: H wide, W tall */

	for (int by = 0; by < rows; by += 8) {
		int uy = uy0 + by; /* upright rows uy..uy+7 read raw columns c0..c0+7 */
		int c0 = rot == 90 ? uy : W - 8 - uy;
		/* 90: raw column c0+j is upright row by+j, stepping +1 row per j;
		 * 270: it is row by+7-j, stepping -1. */
		uint16_t *d0   = rot == 90 ? dst + by * dst_stride : dst + (by + 7) * dst_stride;
		int       step = rot == 90 ? dst_stride : -dst_stride;

		for (int ux0 = 0; ux0 < H; ux0 += 8) {
			/* 90: upright x = H-1-raw row, so raw rows H-8-ux0.. run
			 * backwards in x; 270: upright x = raw row. Eight named
			 * registers, not an array: an array spills through the stack. */
			const uint8_t *p  = src + (rot == 90 ? H - 8 - ux0 : ux0) * W + c0;
			uint8x8x2_t    a0 = vtrn_u8(vld1_u8(p), vld1_u8(p + W));
			uint8x8x2_t    a1 = vtrn_u8(vld1_u8(p + 2 * W), vld1_u8(p + 3 * W));
			uint8x8x2_t    a2 = vtrn_u8(vld1_u8(p + 4 * W), vld1_u8(p + 5 * W));
			uint8x8x2_t    a3 = vtrn_u8(vld1_u8(p + 6 * W), vld1_u8(p + 7 * W));
			/* 8x8 byte transpose, in registers: .8 then .16 then .32 */
			uint16x4x2_t b0 =
			    vtrn_u16(vreinterpret_u16_u8(a0.val[0]),
			             vreinterpret_u16_u8(a1.val[0])); /* cols 0|4 / 2|6, raw rows 0-3 */
			uint16x4x2_t b1 = vtrn_u16(vreinterpret_u16_u8(a0.val[1]),
			                           vreinterpret_u16_u8(a1.val[1])); /* cols 1|5 / 3|7 */
			uint16x4x2_t b2 = vtrn_u16(vreinterpret_u16_u8(a2.val[0]),
			                           vreinterpret_u16_u8(a3.val[0])); /* raw rows 4-7 */
			uint16x4x2_t b3 =
			    vtrn_u16(vreinterpret_u16_u8(a2.val[1]), vreinterpret_u16_u8(a3.val[1]));
			uint32x2x2_t c04 =
			    vtrn_u32(vreinterpret_u32_u16(b0.val[0]), vreinterpret_u32_u16(b2.val[0]));
			uint32x2x2_t c15 =
			    vtrn_u32(vreinterpret_u32_u16(b1.val[0]), vreinterpret_u32_u16(b3.val[0]));
			uint32x2x2_t c26 =
			    vtrn_u32(vreinterpret_u32_u16(b0.val[1]), vreinterpret_u32_u16(b2.val[1]));
			uint32x2x2_t c37 =
			    vtrn_u32(vreinterpret_u32_u16(b1.val[1]), vreinterpret_u32_u16(b3.val[1]));
			uint16_t *d = d0 + ux0;

			/* column j, lane i = raw row i: 90 wants lane l = x ux0+l = raw
			 * row 7-l (reverse the lanes), 270 lane l = raw row l. */
#define TR_ROT_ST(j, v) \
	vst1q_u16( \
	    d + (j) * step, \
	    grey8_to_rgb565(rot == 90 ? vrev64_u8(vreinterpret_u8_u32(v)) : vreinterpret_u8_u32(v)))
			TR_ROT_ST(0, c04.val[0]);
			TR_ROT_ST(1, c15.val[0]);
			TR_ROT_ST(2, c26.val[0]);
			TR_ROT_ST(3, c37.val[0]);
			TR_ROT_ST(4, c04.val[1]);
			TR_ROT_ST(5, c15.val[1]);
			TR_ROT_ST(6, c26.val[1]);
			TR_ROT_ST(7, c37.val[1]);
#undef TR_ROT_ST
		}
	}
}
#endif

void tr_cam_pip_row_grey_to_rgb565(const uint8_t *src_row, int src_w, uint16_t *dst_row, int dst_w)
{
	for (int x = 0; x < dst_w; x++) {
		dst_row[x] = grey_to_rgb565(src_row[x * src_w / dst_w]);
	}
}

void tr_cam_rot_rows(const uint8_t *src,
                     int            src_w,
                     int            src_h,
                     int            rot,
                     int            uy0,
                     int            rows,
                     uint16_t      *dst,
                     int            dst_stride)
{
	int uw = rot != 0 ? src_h : src_w;

	for (int r = 0; r < rows; r++) {
		for (int ux = 0; ux < uw; ux++) {
			int sx, sy;

			tr_cam_rot_src(rot, src_w, src_h, ux, uy0 + r, &sx, &sy);
			dst[r * dst_stride + ux] = grey_to_rgb565(src[sy * src_w + sx]);
		}
	}
}

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

bool tr_cam_pip_map_kp(const tr_kp_t *kp, int rot, int16_t *px, int16_t *py)
{
	if (kp->score < TR_POSE_KP_MIN || kp->x < 0 || kp->x >= TR_CAM_UP_W(rot) || kp->y < 0 ||
	    kp->y >= TR_CAM_UP_H(rot)) {
		return false;
	}
	/* Native 1:1: a keypoint is the image pixel it names. */
	*px = (int16_t)(tr_cam_img_x0(rot) + kp->x);
	*py = (int16_t)(tr_cam_img_y0(rot) + kp->y);
	return true;
}
