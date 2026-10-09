/* src/vision/cam_rot.h -- the OV9281's mounting rotation. The sensor always
 * streams 640x400 GREY8, and everything downstream of the capture works on
 * the UPRIGHT image: 640x400 landscape when the camera sits upright, 400x640
 * portrait when it is mounted on its side and the software turns it.
 *
 * TR_CAM_ROTATE: the degrees the SOFTWARE turns the raw sensor image
 * CLOCKWISE (as it would be displayed, unrotated) to make it upright -- 0,
 * 90 or 270. All three are first-class:
 *   0       the camera is mounted upright (the E1M-EVK's RPi CSI connector,
 *           E1M-EVK): 640x400 LANDSCAPE, 16:10, shown at native 1:1 and
 *           letterboxed in the portrait game's camera half. This is the
 *           release for the arm-raise controls: arms reach sideways, and the
 *           wider field of view keeps both in frame;
 *   90, 270 the camera is mounted on its side (other rigs): 400x640 portrait.
 * Whichever it is, set it the same on the HE and the HP build.
 *
 * Default 90 (the first reference rig), BENCH-VERIFIED (2026W36-0009, 2026-09-25): with the camera body
 * turned 90 deg clockwise as seen from its LENS side, 270 showed the player
 * upside down and 90 upright -- the maintainer confirmed it by eye. So a
 * standing player's head lands at the LEFT edge of the raw frame (the
 * content appears turned 90 deg counter-clockwise), and undoing that is 90
 * deg clockwise (tr_cam_rot_src() below: upright row 0 reads raw column 0,
 * the left edge). An earlier paper derivation put the head at the RIGHT
 * edge and chose 270: it had to assume which way the sensor's row 0 faces
 * on the module, and got it backwards. Trust the bench, not the paper.
 * TR_CAM_MIRROR (below) does not interact: at 90/270 it reverses raw ROWS,
 * a left/right mirror of the upright view, never up/down.
 *
 * Re-check it on any new mount from the first frame: the head must be at
 * the TOP of the video area. If it is at the bottom, flip 90 <-> 270 on the HP
 * build only -- the one image that applies the rotation (the A32 renderer
 * draws whatever rotation the HP publishes in tr_cam_view_t, and the HE only
 * needs the upright frame's dimensions, the same for 90 and 270). 0 <->
 * 90/270 changes those dimensions: set it identically on the HE and HP.
 *
 * Pure C, no Zephyr; host-tested (tests/host/test_movenet_input.c,
 * test_r3d_cam_pip.c).
 */
#ifndef TR_CAM_ROT_H
#define TR_CAM_ROT_H

#include <stdint.h>

#ifndef TR_CAM_ROTATE
#define TR_CAM_ROTATE 90
#endif
#if TR_CAM_ROTATE != 0 && TR_CAM_ROTATE != 90 && TR_CAM_ROTATE != 270
#error "TR_CAM_ROTATE must be 0, 90 or 270"
#endif

/* The sensor mode (OV9281_MODE_640X400): the raw frame, row stride == width. */
#define TR_CAM_SENSOR_W 640
#define TR_CAM_SENSOR_H 400

/* The upright frame -- the coordinate space of every pose keypoint (pose.h
 * tr_kp_t) and of the HE's tracker. */
#define TR_CAM_UP_W(rot) ((rot) != 0 ? TR_CAM_SENSOR_H : TR_CAM_SENSOR_W)
#define TR_CAM_UP_H(rot) ((rot) != 0 ? TR_CAM_SENSOR_W : TR_CAM_SENSOR_H)

/* Upright pixel (ux, uy) -> raw sensor pixel (*sx, *sy), for a raw frame of
 * src_w x src_h:
 *   0:   (ux, uy)
 *   90:  (uy, src_h - 1 - ux)   -- the raw image turned clockwise
 *   270: (src_w - 1 - uy, ux)   -- turned counter-clockwise */
static inline void tr_cam_rot_src(int rot, int src_w, int src_h, int ux, int uy, int *sx, int *sy)
{
	if (rot == 90) {
		*sx = uy, *sy = src_h - 1 - ux;
	} else if (rot == 270) {
		*sx = src_w - 1 - uy, *sy = ux;
	} else {
		*sx = ux, *sy = uy;
	}
}

/* TR_CAM_MIRROR (default 1, a CMake option of both the hp_vision and the HE
 * image, which must agree): the upright view is mirrored left/right like a
 * selfie -- the player raises their right arm, the figure's arm on the RIGHT
 * of the screen goes up. The arm controls depend on it: with the mirror the
 * arm on the screen's left is the player's LEFT arm, without it their RIGHT
 * (src/vision/pose.c). The SENSOR does it (one register
 * bit, free), so the NPU input, the keypoints and the A32 video all see the
 * same mirrored raw frame and stay consistent with no pixel-path change.
 *
 * Which bit, derived: mirroring the upright view is U'(ux, uy) = U(UW-1-ux,
 * uy). At 90, U(ux, uy) = R(uy, H-1-ux) (tr_cam_rot_src()), so U' = R(uy,
 * ux) -- the raw frame with its ROWS reversed (R(sx, H-1-sy) under the same
 * map), i.e. the sensor's VFLIP, not its HMIRROR; 270 works out the same.
 * At 0 the upright IS the raw frame, so it is HMIRROR. Proven on every pixel
 * by tests/host/test_cam_mirror.c. Both are bit 2 of TIMING_FORMAT1 0x3820
 * (VFLIP) / TIMING_FORMAT2 0x3821 (HMIRROR) -- Linux ov9282.c
 * OV9282_FLIP_BIT, set read-modify-write over the mode table's 640x400
 * values 0x60 / 0x01 (binning bits kept). GREY8 sensor: no Bayer phase to
 * shift. */
#ifndef TR_CAM_MIRROR
#define TR_CAM_MIRROR 1
#endif

#define TR_OV9281_REG_TIMING_FORMAT1 0x3820u /* bit 2: VFLIP, raw rows reversed */
#define TR_OV9281_REG_TIMING_FORMAT2 0x3821u /* bit 2: HMIRROR, raw columns reversed */
#define TR_OV9281_FLIP_BIT           0x04u

/* The sensor register whose TR_OV9281_FLIP_BIT mirrors the upright view. */
static inline uint16_t tr_cam_mirror_reg(int rot)
{
	return rot != 0 ? TR_OV9281_REG_TIMING_FORMAT1 : TR_OV9281_REG_TIMING_FORMAT2;
}

/* BENCH TRAP: the sense of the HMIRROR bit (0x3821 bit 2, the rot-0 mirror).
 * The Linux ov9282 driver has had its hflip control inverted against the
 * silicon, and nothing in this repo has yet put a rot-0 picture on a glass to
 * see which way the bit really mirrors (the 90/270 VFLIP was). Set this to 1
 * (the HP's TR_OV9281_HMIRROR_ACTIVE_LOW CMake option) if, on the bench, the
 * physical LEFT arm raised makes the figure's arm go up on the screen's RIGHT
 * (README "Controls", FLASH-RECIPE "Bench acceptance"): the HP then writes
 * the bit the other way round and still reports the TRUTH -- whether the view
 * is mirrored -- in tr_cam_view_t.mirror, which is what the HE reads, so only
 * the HP image changes. Applies to the rot-0 register only. */
#ifndef TR_OV9281_HMIRROR_ACTIVE_LOW
#define TR_OV9281_HMIRROR_ACTIVE_LOW 0
#endif

/* The flip-bit value that makes the upright view mirrored (want != 0) or not. */
static inline uint8_t tr_cam_mirror_bit(int rot, int want)
{
	int inverted = rot == 0 && TR_OV9281_HMIRROR_ACTIVE_LOW;

	return ((want != 0) != inverted) ? TR_OV9281_FLIP_BIT : 0u;
}

/* Whether the view is mirrored, from the register value read back. */
static inline int tr_cam_mirrored_from_reg(int rot, uint8_t reg_value)
{
	int inverted = rot == 0 && TR_OV9281_HMIRROR_ACTIVE_LOW;

	return ((reg_value & TR_OV9281_FLIP_BIT) != 0u) != inverted;
}

#endif /* TR_CAM_ROT_H */
