/* tests/host/test_r3d_cam_pip.c -- src/render/cam_pip.c: the GREY8->RGB565
 * row expansion, the half/half video area's rotation to upright (the scalar
 * reference against an independent rotate, the NEON 8x8 transpose kernel
 * bit-exact against it, the raw column strip a band reads) and the
 * keypoint-to-screen mapping (score gate, corners, the letterbox padding).
 * Named test_r3d_*.c so tests/host/runner.sh's "A32 qemu" stage cross-
 * compiles and qemu-runs this file too -- the only place the NEON kernel
 * runs off silicon. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/render/cam_pip.h"

int main(void)
{
	/* 1. Grey->RGB565: a true grey pixel has r=g=b, so every truncated
	 * channel is EXACTLY what the 5/6/5 packing computes from the same
	 * source byte -- pin the packing for known values, including the
	 * extremes and a value that exercises non-zero low bits. */
	{
		uint8_t  src[4] = { 0x00, 0xFF, 0x80, 0x18 };
		uint16_t dst[4];

		tr_cam_pip_row_grey_to_rgb565(src, 4, dst, 4); /* 1:1, no decimation */
		assert(dst[0] == 0x0000u);                     /* black */
		assert(dst[1] == 0xFFFFu);                     /* white: 0x1F<<11 | 0x3F<<5 | 0x1F */
		assert(dst[2] == ((0x80 >> 3) << 11 | (0x80 >> 2) << 5 | (0x80 >> 3))); /* mid grey */
		assert(dst[3] == ((0x18 >> 3) << 11 | (0x18 >> 2) << 5 | (0x18 >> 3)));
	}

	/* 2. Decimation: nearest-sample, matches x*src_w/dst_w exactly (the
	 * SAME formula tr_cam_pip_map_kp's inverse must agree with). */
	{
		uint8_t  src[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
		uint16_t dst[4];

		tr_cam_pip_row_grey_to_rgb565(src, 8, dst, 4); /* dst[x] <- src[x*8/4] = src[x*2] */
		for (int x = 0; x < 4; x++) {
			uint8_t  g    = src[x * 8 / 4];
			uint16_t want = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));

			assert(dst[x] == want);
		}
	}

	/* 3. Upscaling (dst_w > src_w, e.g. a narrow crop stretched into the
	 * PiP) never reads past src_w-1 -- sx clamps via integer floor, the
	 * last dst column's sx is src_w-1 exactly when dst_w divides evenly. */
	{
		uint8_t  src[2] = { 1, 254 };
		uint16_t dst[4];

		tr_cam_pip_row_grey_to_rgb565(src, 2, dst, 4);
		assert(dst[0] == dst[1]); /* both sample src[0] */
		assert(dst[2] == dst[3]); /* both sample src[1] */
	}

	/* 4. Rotation geometry: tr_cam_rot_src() against the definition
	 * (cam_rot.h), and tr_cam_rot_rows() against an independent rotate of a
	 * whole synthetic frame. 270 turns the raw frame counter-clockwise: a
	 * mark on the raw RIGHT edge (a player's head with the camera body
	 * turned clockwise, cam_rot.h's derivation) lands on the upright TOP. */
	static uint8_t  raw[TR_CAM_SRC_W * TR_CAM_SRC_H];
	static uint16_t want[TR_CAM_SRC_W * TR_VID_W], got[TR_CAM_SRC_W * TR_VID_W];
	const int       W = TR_CAM_SRC_W, H = TR_CAM_SRC_H;

	for (int i = 0; i < W * H; i++) {
		raw[i] = (uint8_t)(i * 2654435761u >> 24);
	}
	{
		int sx, sy;

		assert(TR_CAM_UP_W(90) == 400 && TR_CAM_UP_H(90) == 640 && TR_CAM_UP_W(0) == 640 &&
		       TR_CAM_UP_H(0) == 400);
		tr_cam_rot_src(90, W, H, 0, 0, &sx, &sy);
		assert(sx == 0 && sy == H - 1); /* upright top-left <- raw bottom-left */
		tr_cam_rot_src(90, W, H, H - 1, W - 1, &sx, &sy);
		assert(sx == W - 1 && sy == 0); /* upright bottom-right <- raw top-right */
		tr_cam_rot_src(270, W, H, 0, 0, &sx, &sy);
		assert(sx == W - 1 && sy == 0); /* upright top-left <- raw top-right */
		tr_cam_rot_src(270, W, H, H - 1, W - 1, &sx, &sy);
		assert(sx == 0 && sy == H - 1); /* upright bottom-right <- raw bottom-left */
		tr_cam_rot_src(0, W, H, 17, 33, &sx, &sy);
		assert(sx == 17 && sy == 33);

		static uint8_t mark[TR_CAM_SRC_W * TR_CAM_SRC_H];

		mark[(H / 2) * W + (W - 1)] = 255; /* raw right edge, mid-height */
		tr_cam_rot_rows(mark, W, H, 270, 0, 1, got, TR_VID_W);
		assert(got[H / 2] == 0xFFFFu); /* upright row 0, x = raw row H/2 */
	}
	for (int rot = 90; rot <= 270; rot += 180) {
		for (int uy = 0; uy < W; uy++) {
			for (int ux = 0; ux < H; ux++) {
				/* the definition, written out, not through tr_cam_rot_src() */
				uint8_t g = rot == 90 ? raw[(H - 1 - ux) * W + uy] : raw[ux * W + (W - 1 - uy)];

				want[uy * TR_VID_W + ux] =
				    (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
			}
		}
		memset(got, 0, sizeof(got));
		tr_cam_rot_rows(raw, W, H, rot, 0, W, got, TR_VID_W);
		for (int uy = 0; uy < W; uy++) {
			assert(memcmp(&got[uy * TR_VID_W], &want[uy * TR_VID_W], (size_t)H * 2u) == 0);
		}
		/* a band's raw column strip covers every byte its rows read */
		for (int uy0 = 0; uy0 < W; uy0 += TR_BAND_H) {
			int c0, c1, sx, sy;

			tr_cam_rot_src_cols(rot, uy0, TR_BAND_H, &c0, &c1);
			assert(c0 >= 0 && c1 <= W && c1 - c0 == TR_BAND_H);
			for (int r = 0; r < TR_BAND_H; r++) {
				for (int ux = 0; ux < H; ux++) {
					tr_cam_rot_src(rot, W, H, ux, uy0 + r, &sx, &sy);
					assert(sx >= c0 && sx < c1);
				}
			}
		}
	}
	printf("rotate: 90/270 scalar == the definition, column strips cover every read\n");

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	/* 5. The NEON kernel: bit-exact with the scalar reference for both
	 * rotations, every 32-row band (the renderer's call) and a whole frame
	 * at once, a hash pattern plus a 0x00/0xFF checker (block-seam edges),
	 * written at the video area's x offset with guard words either side
	 * that must survive. */
	for (int pat = 0; pat < 2; pat++) {
		if (pat == 1) {
			for (int i = 0; i < W * H; i++) {
				raw[i] = (uint8_t)(((i % W) ^ (i / W)) & 1 ? 0xFF : 0x00);
			}
		}
		for (int rot = 90; rot <= 270; rot += 180) {
			int x0 = tr_cam_img_x0(rot);

			memset(want, 0xA5, sizeof(want));
			memset(got, 0xA5, sizeof(got));
			tr_cam_rot_rows(raw, W, H, rot, 0, W, want + x0, TR_VID_W);
			for (int uy0 = 0; uy0 < W; uy0 += TR_BAND_H) {
				tr_cam_rot_rows_neon(raw, rot, uy0, TR_BAND_H, got + uy0 * TR_VID_W + x0, TR_VID_W);
			}
			assert(memcmp(want, got, sizeof(want)) == 0);
			memset(got, 0xA5, sizeof(got));
			tr_cam_rot_rows_neon(raw, rot, 0, W, got + x0, TR_VID_W);
			assert(memcmp(want, got, sizeof(want)) == 0);
		}
	}
	printf("NEON rotate: bit-exact with scalar, 90 + 270, banded + whole frame, guards intact\n");
#endif

	/* 6. Keypoints -> screen: native 1:1, so an upright keypoint is the
	 * image pixel it names -- the corners of the portrait image land on the
	 * corners of x 200..599 x rows 0..639 of the video area; the landscape
	 * comparison path centres 640x400 at x 80, row 120. */
	{
		int16_t px = -1, py = -1;
		tr_kp_t k;

		assert(tr_cam_img_x0(90) == 200 && tr_cam_img_y0(90) == 0 && tr_cam_img_x0(270) == 200);
		assert(tr_cam_img_x0(0) == 80 && tr_cam_img_y0(0) == 120);
		k = (tr_kp_t){ 0, 0, 255 };
		assert(tr_cam_pip_map_kp(&k, 90, &px, &py) && px == 200 && py == 0);
		k = (tr_kp_t){ 399, 639, 255 };
		assert(tr_cam_pip_map_kp(&k, 270, &px, &py) && px == 599 && py == 639);
		k = (tr_kp_t){ 399, 0, 255 };
		assert(tr_cam_pip_map_kp(&k, 90, &px, &py) && px == 599 && py == 0);
		k = (tr_kp_t){ 0, 639, 255 };
		assert(tr_cam_pip_map_kp(&k, 90, &px, &py) && px == 200 && py == TR_VID_H - 1);
		k = (tr_kp_t){ 639, 399, 255 };
		assert(tr_cam_pip_map_kp(&k, 0, &px, &py) && px == 719 && py == 519);
		/* Landscape (rotation 0), the arm controls' layout: 640x400 (16:10) at
		 * native 1:1 -- never stretched -- centred in the 800x640 video area
		 * with 80-px side margins and 120-row letterbox bands, and every
		 * keypoint lands on the image pixel it names. */
		_Static_assert(TR_CAM_UP_W(0) == 640 && TR_CAM_UP_H(0) == 400,
		               "landscape is the raw frame");
		_Static_assert(TR_VID_W >= TR_CAM_UP_W(0) && TR_VID_H >= TR_CAM_UP_H(0),
		               "the landscape image fits the video area");
		k = (tr_kp_t){ 0, 0, 255 };
		assert(tr_cam_pip_map_kp(&k, 0, &px, &py) && px == 80 && py == 120);
		k = (tr_kp_t){ 320, 200, 255 };
		assert(tr_cam_pip_map_kp(&k, 0, &px, &py) && px == TR_VID_W / 2 && py == TR_VID_H / 2);
		k = (tr_kp_t){ 640, 10, 255 };
		assert(!tr_cam_pip_map_kp(&k, 0, &px, &py));
		k = (tr_kp_t){ 10, 400, 255 };
		assert(!tr_cam_pip_map_kp(&k, 0, &px, &py)); /* 400 is a PORTRAIT-only row */
		/* outside the upright frame (the letterbox padding) or unsure: not drawn */
		px = py = -7;
		k       = (tr_kp_t){ -1, 10, 255 };
		assert(!tr_cam_pip_map_kp(&k, 90, &px, &py));
		k = (tr_kp_t){ 400, 10, 255 };
		assert(!tr_cam_pip_map_kp(&k, 90, &px, &py));
		k = (tr_kp_t){ 10, 640, 255 };
		assert(!tr_cam_pip_map_kp(&k, 90, &px, &py));
		k = (tr_kp_t){ 200, 300, TR_POSE_KP_MIN - 1 };
		assert(!tr_cam_pip_map_kp(&k, 90, &px, &py));
		assert(px == -7 && py == -7); /* untouched on a reject */
		k.score = TR_POSE_KP_MIN;
		assert(tr_cam_pip_map_kp(&k, 90, &px, &py) && px == 400 && py == 300);
		printf("map_kp: corners 1:1, padding and low score rejected\n");
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
