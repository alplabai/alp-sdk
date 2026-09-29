/* src/vision/movenet.h */
#ifndef TR_MOVENET_H
#define TR_MOVENET_H

#include <stdint.h>

#include "cam_rot.h"
#include "pose.h"

/*
 * MoveNet SinglePose Lightning int8's post-processing, in integer C.
 *
 * The NPU runs the model cut at its four int8 output maps
 * (tools/movenet_cut.py: 100 % NPU, 9.22 ms on the HP's Ethos-U55-256 per
 * Vela); this is the CPU tail the published model carried as 29 float ops
 * (argmax, GatherNd, SQRT, DIV). Same algorithm:
 *   1. the person centre = argmax of the weighted centre map;
 *   2. per keypoint, a first guess = centre + that cell's regression;
 *   3. per keypoint, the cell maximising heat / (distance to the guess + 1.8);
 *   4. that cell + its sub-cell offset is the keypoint; its heat the score.
 * Host-checked against the LiteRT reference of the FULL model
 * (tests/host/test_movenet.c).
 */

#define TR_MN_GRID  48 /* output maps are 48x48 */
#define TR_MN_CELLS (TR_MN_GRID * TR_MN_GRID)
#define TR_MN_IN    192 /* input square; 4 input px per cell */

/* Output quantisation of MoveNet Lightning int8 v4 (Vela keeps I/O quant):
 * real = (q - zp) * scale; scales in Q16 cell units. The heatmaps are a
 * sigmoid at zp -128, scale 1/256, so (q + 128) is the score 0..255. */
#define TR_MN_OFF_ZP       (-9)
#define TR_MN_OFF_SCALE    13093 /* 0.19978105 cells */
#define TR_MN_REG_ZP       (-21)
#define TR_MN_REG_SCALE    45118 /* 0.68843985 cells */

typedef struct {
	const int8_t *centre;  /* [TR_MN_CELLS] */
	const int8_t *heat;    /* [TR_MN_CELLS][17] */
	const int8_t *offset;  /* [TR_MN_CELLS][34], (y, x) per keypoint */
	const int8_t *regress; /* [TR_MN_CELLS][34], (y, x) per keypoint */
} tr_movenet_out_t;

/*
 * Pre-processing: a GREY8 camera frame (row stride == frame_w, frame_w >=
 * frame_h) into the model's input tensor [192][192][3] int8 (q = grey - 128,
 * the grey replicated into R, G, B): scaled by 192/frame_w with a 2x2 mean at
 * each sample (a cheap anti-alias for the ~3.3x shrink), centred vertically,
 * the bands above and below mid-grey (q = 0). tr_movenet_input_rot(.., 0, ..).
 */
void tr_movenet_input(const uint8_t *grey, int16_t frame_w, int16_t frame_h, int8_t *in);

/*
 * The same, of the UPRIGHT image (cam_rot.h): the raw src_w x src_h frame
 * turned by rot (0, 90, 270) inside this one pass -- no rotated copy. The
 * upright frame's long side fills the square, the short side is centred and
 * padded both ways (portrait 400x640: 120 image columns, 36 pad columns each
 * side). Decode with the upright dimensions (cam_rot.h TR_CAM_UP_W/H).
 */
void tr_movenet_input_rot(const uint8_t *grey, int16_t src_w, int16_t src_h, int rot, int8_t *in);

/*
 * Decode into frame pixels of a frame_w x frame_h (upright) image
 * letterboxed into the 192 square as tr_movenet_input_rot() does it: the
 * long side scaled to 192, the short side centred.
 */
void tr_movenet_decode(const tr_movenet_out_t *o, int16_t frame_w, int16_t frame_h, tr_pose_t *out);

#endif /* TR_MOVENET_H */
