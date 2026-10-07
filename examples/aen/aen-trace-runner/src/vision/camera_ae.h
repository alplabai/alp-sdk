/* src/vision/camera_ae.h -- software auto-exposure for the OV9281: it has no
 * usable on-chip AEC (design sec 2, bench-probed: exposure and gain never
 * move on their own). Pure integer C, no Zephyr headers -- the HP app calls
 * tr_ae_step() on a measured frame mean and writes the result to the
 * sensor's VIDEO_CID_EXPOSURE / VIDEO_CID_ANALOGUE_GAIN controls
 * (chips/ov9281/zephyr/drivers/video/ov9281.c) itself; this file never
 * touches I2C.
 *
 * DELIBERATE DEVIATION FROM THE DESIGN DOC: section 2 says "a P-controller
 * on the letterboxed frame's mean". Measuring the LETTERBOXED 192x192 buffer
 * would bias the mean toward mid-grey: at 640x400 the letterbox pads 72 of
 * 192 rows (37.5 %) to q=0 (grey 128), which is not scene content. This
 * controller is fed the mean of the RAW captured frame instead (excludes the
 * padding entirely) -- tr_ae_meter() below, over the real 640x400
 * pixels, matches what the AE loop should actually be measuring.
 *
 * No notion of time: tr_ae_step() counts frames (it settles one frame
 * after each write), the HP main loop calls it on every frame.
 */
#ifndef TR_CAMERA_AE_H
#define TR_CAMERA_AE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The controller (polish round, silicon finding: the old P-controller,
 * TR_AE_KP = 4 lines per unit of mean error, bang-banged between 429 and 5
 * lines on a daylight window scene -- a saturated frame's error of -127
 * swung it to the floor, the dark frame's error swung it back). Now:
 *
 * - It steers the TOTAL exposure, lines x analogue gain (gain register
 *   units, 0x10 = 1x), in the LOG domain: the wanted total is
 *   total * target / mean, limited to x/÷ TR_AE_STEP_NUM/TR_AE_STEP_DEN
 *   (1.25) per update. For a scene whose brightness is proportional to
 *   exposure this lands on the target; saturation or the highlight clip
 *   only make the response concave, which makes the step UNDER-shoot, never
 *   overshoot -- the convergence is monotonic by construction.
 * - Exposure first (1x gain, least noise), gain only once exposure is at
 *   the ceiling.
 * - Deadband with hysteresis: it starts hunting only once the mean is more
 *   than TR_AE_BAND_OUT from the target and stops as soon as it is within
 *   TR_AE_BAND_IN, so sensor noise at the band edge cannot toggle it.
 * - After every change it skips TR_AE_SETTLE_FRAMES frames: the frame
 *   after a register write was already exposing with the old value.
 *
 * ponytail: fixed constants, not per-scene calibrated; retune here if the
 * booth lighting needs it. */
#define TR_AE_TARGET_MEAN   128
#define TR_AE_BAND_IN       8
#define TR_AE_BAND_OUT      24
#define TR_AE_STEP_NUM      5
#define TR_AE_STEP_DEN      4
#define TR_AE_SETTLE_FRAMES 1

/* Metering (tr_ae_meter): the centre half of the frame (in both axes) counts
 * TR_AE_CENTRE_WEIGHT times, the player stands there; every sample is
 * clipped at TR_AE_CLIP so a blown-out window cannot drag the mean down on
 * its own; every TR_AE_METER_STEP-th pixel and row (1/16 of a 640x400 frame
 * is plenty for a mean). */
#define TR_AE_CENTRE_WEIGHT 3
#define TR_AE_CLIP          (TR_AE_TARGET_MEAN * 3 / 2)
#define TR_AE_METER_STEP    4

/* chips/ov9281's ov9281_gain_reg_map[] (index -> reg 0x3509, 0x10 = 1x):
 * `static` in that driver, so the raw-register AE carries its own copy. */
#define TR_AE_GAIN_IDX_MAX 111u
extern const uint8_t tr_ae_gain_reg_map[TR_AE_GAIN_IDX_MAX + 1u];

typedef struct {
	uint16_t exposure; /* whole lines, what apply_ae() writes */
	uint8_t  gain_idx; /* index into tr_ae_gain_reg_map[] */
	uint8_t  settle;   /* frames still to skip after the last change */
	bool     hunting;  /* outside the inner band, still converging */
} tr_ae_t;

void tr_ae_init(tr_ae_t *ae, uint16_t exposure0, uint8_t gain_idx0);

/* Centre-weighted, highlight-clipped mean of a GREY8 w x h frame (the raw
 * frame, not the letterboxed tensor -- see the header note). */
uint8_t tr_ae_meter(const uint8_t *grey, int w, int h);

/*
 * One AE step on this frame's metered mean; call it every frame. Returns
 * true when *ae changed (the caller writes it to the sensor); false while
 * settling, inside the deadband, or saturated in the needed direction.
 *
 * exposure_min/exposure_max: the sensor mode's live range (OV9281: min
 * 0x05 fixed, max = mode->vts - OV9281_EXP_MAX_OFFSET, so the caller passes
 * it rather than this file hard-coding a mode-specific number).
 */
bool tr_ae_step(tr_ae_t *ae,
                uint8_t  mean,
                uint16_t exposure_min,
                uint16_t exposure_max,
                uint8_t  gain_idx_max);

/* fix round 11 (silicon finding: raw frame mean 16.5/255, "far too dark"),
 * story corrected in fix round 13 (maintainer): TR_AE_EXP_MAX (0x2a9 = 681,
 * the driver's own OV9281_EXPOSURE_DEFAULT) WAS already the sensor's real
 * ceiling in whole lines -- round 11 misread the OV9281_FETCH_EXP_H/M/L
 * split (chips/ov9281/zephyr/drivers/video/ov9281.c) as expecting a pre-
 * scaled 1/16-line value, when the macros themselves already do that
 * scaling internally (H/M/L together encode `lines << 4` once assembled
 * into the physical 20-bit register -- see tr_ae_exposure_regs()'s own
 * header comment for the derivation, proven against the sensor's own
 * reset-default bytes 00/2a/90 for exactly 681 LINES, not 681*16). The
 * real dark-frame bug was elsewhere: hp_vision's ceiling was fixed but the
 * frame really was that dark (a genuinely dim room, per the maintainer),
 * and a SEPARATE round-12 bug (writing lines<<4 into this already-<<4-
 * internally split) made it briefly WORSE before this round reverted it.
 * The one real, still-standing fix from round 11 is making the ceiling
 * LIVE (VTS read over I2C, AEC registers 0x380E/0x380F, hp_vision/src/
 * main.c) instead of a fixed constant, since VTS is genuinely per-mode
 * (100/50/100 fps, three different VTS values, chips/ov9281's own mode
 * table); margin is the sensor's minimum VTS-to-exposure gap for its own
 * readout (the driver's OV9281_EXP_MAX_OFFSET, 25 lines -- the same
 * datasheet-derived margin, not re-derived here). Pure integer math, no
 * Zephyr: tests/host/test_camera_ae.c pins it against known VTS values. */
uint32_t tr_ae_exposure_max_from_vts(uint16_t vts, uint16_t margin);

/* fix round 12 finding A (silicon: exposure/gain writes appeared to have
 * no effect on the captured frame), story corrected in fix round 13
 * (maintainer): `val` here is WHOLE LINES, not 1/16-line units -- OV9281_
 * FETCH_EXP_H/M/L (chips/ov9281/zephyr/drivers/video/ov9281.c) already
 * shift it left 4 THEMSELVES when splitting it across the three physical
 * registers (H gets bits [15:12] of the line count, M bits [11:4], L's own
 * upper nibble bits [3:0] -- together exactly `val << 4` once the bytes
 * are reassembled into the sensor's real 20-bit AEC_EXPO field, whose own
 * low 4 bits are its 1/16-line fraction). Proven against the sensor's own
 * reset-default register bytes: OV9281_EXPOSURE_DEFAULT (0x2a9 = 681)
 * writes 0x3500/01/02 = 00/2a/90, exactly what this split's own formula
 * gives for 681 WHOLE lines (not 681*16) -- tests/host/test_camera_ae.c
 * pins this exact byte triple, plus 1071 lines -> 00/42/f0 (the 640x400 @
 * 100 fps mode's own real ceiling, VTS 1096 - 25). Round 11 misread this
 * split as needing a pre-shifted input; round 12, having started writing
 * these registers directly (bypassing chips/ov9281's own ov9281_set_ctrl(),
 * which clamps VIDEO_CID_EXPOSURE to ctrls->exposure.range.max -- the
 * driver's OWN clamp, whole lines, ALWAYS correct, never the bug), carried
 * that same over-shift into the raw write: a 1071-line ceiling became a
 * register write of 1071*16 = 17136 lines against a 1096-line frame,
 * silicon-confirmed DARKER (mean 8 vs 16.5), not the "no effect" the
 * original finding reported (a separate, since-resolved question). This is
 * the register split math alone, reproduced here so it is a pure, host-
 * tested unit (not inlined at the one I2C call site, hp_vision/src/main.c,
 * which stays Zephyr-only there -- this file stays Zephyr-free). */
void tr_ae_exposure_regs(uint16_t val, uint8_t *h, uint8_t *m, uint8_t *l);

#endif /* TR_CAMERA_AE_H */
