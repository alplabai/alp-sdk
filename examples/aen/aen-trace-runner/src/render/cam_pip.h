/* src/render/cam_pip.h -- pure, portable pieces of the A32 renderer's live
 * camera video area (maintainer ruling 2026-10-08, 3/5 game : 2/5 camera): the
 * UPRIGHT LANDSCAPE camera (TR_CAM_ROTATE 0, 640x400 GREY8) scaled UP, keeping
 * its aspect ratio, until it COVERS the whole video area -- 800 x 512 rows
 * [TR_VID_Y0, TR_R3D_H) -- and cropped where it overhangs (about 10 px a
 * side); the skeleton mapped onto it by the exact inverse. The scale is the
 * larger of the two ratios, max(800 / 640, 512 / 400) = 1.28 = 32 / 25, so the
 * picture is height-bound: all 400 source rows are shown (the image is
 * 819.2 px wide, 19.2 px of it cropped). Rotated cameras (TR_CAM_ROTATE
 * 90 / 270) are not drawn: the video area says so instead (a32/renderer/
 * render.c). Host-tested (tests/host/test_r3d_cam_pip.c, the NEON kernel
 * bit-exact against the scalar one under qemu-arm too). No Zephyr;
 * a32/renderer/render.c calls these same functions.
 *
 * The scale, as a bilinear resample on a 1/64-px grid. Output pixel (x, r)
 * of the area samples the source at
 *
 *     sx = (50 x + 473) / 64      sy = (50 r - 7) / 64        (source px)
 *
 * (50 / 64 = 25 / 32 = 1 / 1.28; both centres on the pixel-index convention,
 * pixel i at i, so 473 is the half-pixel and the 9.6 px crop folded in, -7 the
 * half-pixel alone). The integer part picks the two taps, the low 6 bits are
 * the weight of the right/lower one; the horizontal lerp rounds to 8 bits,
 * then the vertical one does. Every 32 output columns consume exactly 25
 * source columns, which is what lets the NEON kernel build one gather table
 * for the pattern and reuse it along the row.
 */
#ifndef TR_CAM_PIP_HEADER_H
#define TR_CAM_PIP_HEADER_H

#include <stdbool.h>
#include <stdint.h>

#include "../vision/cam_rot.h" /* TR_CAM_SENSOR_W/H */
#include "../vision/pose.h"    /* tr_kp_t, TR_POSE_KP_MIN */
#include "r3d.h"               /* TR_R3D_W/H, TR_VIEW_H */

#define TR_CAM_SRC_W TR_CAM_SENSOR_W /* the raw frame the HP publishes (tr_cam_view.h) */
#define TR_CAM_SRC_H TR_CAM_SENSOR_H

/* The video area: every row below the game viewport, full width. */
#define TR_VID_Y0 TR_VIEW_H
#define TR_VID_W  TR_R3D_W
#define TR_VID_H  (TR_R3D_H - TR_VIEW_H)

/* The cover scale, NUM / DEN, and its 1/64-px sampling grid. */
#define TR_CAM_COVER_NUM  32
#define TR_CAM_COVER_DEN  25
#define TR_CAM_COVER_STEP 50   /* source px per output px, in 1/64 (64 * DEN / NUM) */
#define TR_CAM_COVER_X0   473  /* sx at output column 0, in 1/64 */
#define TR_CAM_COVER_Y0   (-7) /* sy at output row 0, in 1/64 */
_Static_assert(TR_CAM_COVER_STEP *TR_CAM_COVER_NUM == 64 * TR_CAM_COVER_DEN,
               "the step is 64 / scale");
/* Cover: the scaled image reaches both edges of the area (>=) and is exactly as tall as it
 * (the scale is the larger of the two ratios, max(800 / 640, 512 / 400): the height-bound
 * 1.28 against 1.25). */
_Static_assert(TR_CAM_SENSOR_W *TR_CAM_COVER_NUM >= TR_VID_W * TR_CAM_COVER_DEN,
               "the camera does not cover the area's width");
_Static_assert(TR_CAM_SENSOR_H *TR_CAM_COVER_NUM == TR_VID_H * TR_CAM_COVER_DEN,
               "the scale is not the area's height over the camera's");
/* The offsets are the half-pixel of each grid plus the centring crop, derived: the crop is
 * (SENSOR_W * NUM / DEN - VID_W) / 2 output px (9.6), each output px is STEP / 64 source px. */
_Static_assert(TR_CAM_COVER_X0 ==
                   TR_CAM_COVER_STEP *
                           (TR_CAM_SENSOR_W * TR_CAM_COVER_NUM - TR_VID_W * TR_CAM_COVER_DEN) /
                           (2 * TR_CAM_COVER_DEN) +
                       TR_CAM_COVER_STEP / 2 - 32,
               "TR_CAM_COVER_X0: centring crop + half a pixel");
_Static_assert(TR_CAM_COVER_Y0 == TR_CAM_COVER_STEP / 2 - 32, "TR_CAM_COVER_Y0: half a pixel");
/* The NEON kernel's pattern: 32 output columns are 25 source columns, 800 is whole patterns. */
_Static_assert(32 * TR_CAM_COVER_STEP == 25 * 64 && TR_VID_W % 32 == 0,
               "32 output columns must be 25 source columns, and the width whole patterns");

/* Source position of output column x / row r, in 1/64 source px (floor with >> 6, & 63). Macros
 * as well, so the NEON kernel's gather tables are constant initialisers. */
#define TR_CAM_COVER_SX64(x) (TR_CAM_COVER_STEP * (x) + TR_CAM_COVER_X0)
#define TR_CAM_COVER_SY64(r) (TR_CAM_COVER_STEP * (r) + TR_CAM_COVER_Y0)

static inline int tr_cam_cover_sx64(int x)
{
	return TR_CAM_COVER_SX64(x);
}

static inline int tr_cam_cover_sy64(int r)
{
	return TR_CAM_COVER_SY64(r);
}

/* Source rows [*j0, *j1] (clamped to the frame) that output rows [r0, r0 + rows) read: what the
 * A32 invalidates (whole rows, 640-B aligned) before a band reads CAM_POOL. */
static inline void tr_cam_cover_src_rows(int r0, int rows, int *j0, int *j1)
{
	int lo = tr_cam_cover_sy64(r0) >> 6, hi = (tr_cam_cover_sy64(r0 + rows - 1) >> 6) + 1;

	*j0 = lo < 0 ? 0 : lo;
	*j1 = hi > TR_CAM_SRC_H - 1 ? TR_CAM_SRC_H - 1 : hi;
}

/* Scalar reference: output rows [r0, r0 + rows) of the area (TR_VID_W px each) as RGB565 into
 * dst (row i at dst + i * dst_stride), from the TR_CAM_SRC_W x TR_CAM_SRC_H GREY8 frame src
 * (r = g = b = grey, 5/6/5 truncation). The definition the NEON kernel must match bit for bit. */
void tr_cam_cover_rows(const uint8_t *src, int r0, int rows, uint16_t *dst, int dst_stride);

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* tr_cam_cover_rows() on NEON: each source row is filtered horizontally ONCE (a vtbl4 gather of
 * the left and right taps of the 32-column pattern, 6-bit weights, vrshrn #6), the last two kept
 * for the next output row, then the vertical lerp and GREY8 -> RGB565 (two vsli) 16 columns at a
 * time into 16-byte stores. dst and dst_stride * 2 must be 16-byte aligned. */
void tr_cam_cover_rows_neon(const uint8_t *src, int r0, int rows, uint16_t *dst, int dst_stride);
#endif

/* One pose keypoint (UPRIGHT frame px, pose.h tr_kp_t, the 640 x 400 frame the HP decodes into)
 * -> screen column *px and video-area row *py: the inverse of the sampling grid above,
 * x = (64 kx - 473) / 50 and y = (64 ky + 7) / 50, rounded to nearest, so a keypoint lands on the
 * output pixel that samples it (within a pixel of the forward map: the scale is 1.28). false
 * (outputs untouched) below TR_POSE_KP_MIN -- the same gate track.c/pose.c use, so the skeleton
 * never draws a bone the game would not trust -- outside the upright frame, or where the crop
 * cut it off (outside the area). */
bool tr_cam_pip_map_kp(const tr_kp_t *kp, int16_t *px, int16_t *py);

/* Format loop_hz_x10 (Hz * 10, src/ipc/tr_hp_dbg.h hp_dbg_t.loop_hz_x10) as
 * "NN.NHz" into out (>= 10 B). Returns the length (excluding '\0'). No
 * snprintf: there is no libc formatting on the release A32 image. */
int tr_cam_pip_format_hz(uint32_t loop_hz_x10, char *out);

#endif /* TR_CAM_PIP_HEADER_H */
