/* src/render/cam_pip.h -- pure, portable pieces of the A32 renderer's live
 * camera video area (maintainer ruling "Half / half"): the camera at NATIVE
 * 1:1, turned upright (src/vision/cam_rot.h), centred in rows [TR_VID_Y0,
 * TR_R3D_H) on a dark background, no border; the skeleton mapped onto it.
 * With the sensor on its side the upright image is 400x640 portrait at
 * x 160..559, leaving a 160-px strip each side (intent lamps left, the
 * "CAMERA / NPU Hz" label right -- a32/renderer/render.c). Upright
 * (rotation 0, the arm controls' release) it is 640x400 LANDSCAPE at
 * x 40..679, rows 120..519 of the area: 1:1 (fit width less a 40-px margin,
 * never stretched), letterboxed 120 rows above (the lamps) and below (the
 * label). Host-tested
 * (tests/host/test_r3d_cam_pip.c, the NEON kernel under qemu-arm too). No
 * Zephyr; a32/renderer/render.c calls these same functions.
 */
#ifndef TR_CAM_PIP_HEADER_H
#define TR_CAM_PIP_HEADER_H

#include <stdbool.h>
#include <stdint.h>

#include "../vision/cam_rot.h" /* TR_CAM_SENSOR_W/H, TR_CAM_UP_W/H, tr_cam_rot_src */
#include "../vision/pose.h"    /* tr_kp_t, TR_POSE_KP_MIN */
#include "r3d.h"               /* TR_R3D_W/H, TR_VIEW_H */

#define TR_CAM_SRC_W TR_CAM_SENSOR_W /* the raw frame the HP publishes (tr_cam_view.h) */
#define TR_CAM_SRC_H TR_CAM_SENSOR_H

/* The video area: every row below the game viewport, full width. */
#define TR_VID_Y0 TR_VIEW_H
#define TR_VID_W  TR_R3D_W
#define TR_VID_H  (TR_R3D_H - TR_VIEW_H)
_Static_assert(TR_VID_H >= TR_CAM_SENSOR_W && TR_VID_W >= TR_CAM_SENSOR_W,
               "the video area must hold the camera at native 1:1 in either orientation");

/* Where the upright image of rotation rot sits: centred, native 1:1. x in
 * screen columns, y in video-area rows (add TR_VID_Y0 for the screen row). */
static inline int tr_cam_img_x0(int rot)
{
	return (TR_VID_W - TR_CAM_UP_W(rot)) / 2;
}

static inline int tr_cam_img_y0(int rot)
{
	return (TR_VID_H - TR_CAM_UP_H(rot)) / 2;
}

/* One GREY8 row -> RGB565, nearest-sample horizontal resample of src_w
 * pixels into dst_w (r = g = b = grey, 5/6/5 truncation -- a true grey's R,
 * G and B always agree, so this is exact for every representable level).
 * The landscape (rotation 0) path calls it 1:1 (src_w == dst_w). */
void tr_cam_pip_row_grey_to_rgb565(const uint8_t *src_row, int src_w, uint16_t *dst_row, int dst_w);

/* Scalar reference: UPRIGHT rows [uy0, uy0 + rows) of the src_w x src_h raw
 * frame turned by rot (cam_rot.h), TR_CAM_UP_W(rot) px each, as RGB565 into
 * dst (row r at dst + r * dst_stride). */
void tr_cam_rot_rows(const uint8_t *src,
                     int            src_w,
                     int            src_h,
                     int            rot,
                     int            uy0,
                     int            rows,
                     uint16_t      *dst,
                     int            dst_stride);

/* The raw columns [*c0, *c1) that upright rows [uy0, uy0 + rows) read, over
 * every raw row, for rot 90/270 (a column strip: what the A32 invalidates
 * before a band reads CAM_POOL). */
static inline void tr_cam_rot_src_cols(int rot, int uy0, int rows, int *c0, int *c1)
{
	*c0 = rot == 90 ? uy0 : TR_CAM_SRC_W - uy0 - rows;
	*c1 = *c0 + rows;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* tr_cam_rot_rows() for the real frame (TR_CAM_SRC_W x TR_CAM_SRC_H) at rot
 * 90 or 270, uy0 and rows multiples of 8: 8x8 byte blocks, eight 8-byte raw
 * row loads transposed in registers (vtrn.8/.16/.32), a lane reverse for 90
 * (vrev64.8), then GREY8 -> RGB565 (two vsli) and one 16-byte store per
 * output row of the block. Bit-exact against tr_cam_rot_rows()
 * (tests/host/test_r3d_cam_pip.c, under qemu-arm: a plain x86 host never
 * defines __ARM_NEON). */
void tr_cam_rot_rows_neon(const uint8_t *src,
                          int            rot,
                          int            uy0,
                          int            rows,
                          uint16_t      *dst,
                          int            dst_stride);
#endif

/* One pose keypoint (UPRIGHT frame px, pose.h tr_kp_t -- the frame the HP
 * decodes into) -> screen column *px and video-area row *py of the image of
 * rotation rot. false (outputs untouched) below TR_POSE_KP_MIN -- the same
 * gate track.c/pose.c use, so the skeleton never draws a bone the game would
 * not trust -- or outside the upright frame (a keypoint decoded into the
 * letterbox padding). */
bool tr_cam_pip_map_kp(const tr_kp_t *kp, int rot, int16_t *px, int16_t *py);

/* Format loop_hz_x10 (Hz * 10, src/ipc/tr_hp_dbg.h hp_dbg_t.loop_hz_x10) as
 * "NN.NHz" into out (>= 10 B). Returns the length (excluding '\0'). No
 * snprintf: there is no libc formatting on the release A32 image. */
int tr_cam_pip_format_hz(uint32_t loop_hz_x10, char *out);

#endif /* TR_CAM_PIP_HEADER_H */
