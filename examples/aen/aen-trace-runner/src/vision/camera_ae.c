/* src/vision/camera_ae.c -- see camera_ae.h. */
#include "camera_ae.h"

const uint8_t tr_ae_gain_reg_map[TR_AE_GAIN_IDX_MAX + 1u] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
	0x40, 0x42, 0x44, 0x46, 0x48, 0x4a, 0x4c, 0x4e, 0x50, 0x52, 0x54, 0x56, 0x58, 0x5a, 0x5c, 0x5e,
	0x60, 0x62, 0x64, 0x66, 0x68, 0x6a, 0x6c, 0x6e, 0x70, 0x72, 0x74, 0x76, 0x78, 0x7a, 0x7c, 0x7e,
	0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xb4, 0xb8, 0xbc,
	0xc0, 0xc4, 0xc8, 0xcc, 0xd0, 0xd4, 0xd8, 0xdc, 0xe0, 0xe4, 0xe8, 0xec, 0xf0, 0xf4, 0xf8, 0xfc,
};

#define TR_AE_GAIN_UNITY 0x10u /* tr_ae_gain_reg_map[0], 1x */

void tr_ae_init(tr_ae_t *ae, uint16_t exposure0, uint8_t gain_idx0)
{
	*ae = (tr_ae_t){ .exposure = exposure0, .gain_idx = gain_idx0 };
}

uint8_t tr_ae_meter(const uint8_t *grey, int w, int h)
{
	uint32_t sum = 0, n = 0;

	for (int y = 0; y < h; y += TR_AE_METER_STEP) {
		bool cy = y >= h / 4 && y < h - h / 4;

		for (int x = 0; x < w; x += TR_AE_METER_STEP) {
			uint32_t p = grey[y * w + x];
			uint32_t k = cy && x >= w / 4 && x < w - w / 4 ? TR_AE_CENTRE_WEIGHT : 1u;

			sum += (p < TR_AE_CLIP ? p : TR_AE_CLIP) * k;
			n += k;
		}
	}
	return n != 0u ? (uint8_t)(sum / n) : 0u;
}

/* The (exposure, gain) pair for a total (lines x gain register): exposure
 * alone up to its ceiling at 1x, then the largest gain not above the rest. */
static void split_total(uint32_t total, uint16_t exp_min, uint16_t exp_max, uint8_t gain_idx_max, uint16_t *exp,
			uint8_t *gain_idx)
{
	*gain_idx = 0;
	if (total <= (uint32_t)exp_max * TR_AE_GAIN_UNITY) {
		uint32_t e = (total + TR_AE_GAIN_UNITY / 2u) / TR_AE_GAIN_UNITY;

		*exp = (uint16_t)(e < exp_min ? exp_min : e > exp_max ? exp_max : e);
		return;
	}
	*exp = exp_max;
	while (*gain_idx < gain_idx_max && tr_ae_gain_reg_map[*gain_idx + 1u] <= total / exp_max) {
		(*gain_idx)++;
	}
}

bool tr_ae_step(tr_ae_t *ae, uint8_t mean, uint16_t exposure_min, uint16_t exposure_max, uint8_t gain_idx_max)
{
	if (ae->settle != 0u) {
		ae->settle--; /* this frame was exposed (partly) before the last write */
		return false;
	}

	int32_t err  = (int32_t)mean - TR_AE_TARGET_MEAN; /* >0: too bright */
	int32_t aerr = err < 0 ? -err : err;

	if (aerr <= (ae->hunting ? TR_AE_BAND_IN : TR_AE_BAND_OUT)) {
		ae->hunting = false;
		return false;
	}
	ae->hunting = true;

	uint8_t  gi    = ae->gain_idx <= gain_idx_max ? ae->gain_idx : gain_idx_max;
	uint32_t total = (uint32_t)ae->exposure * tr_ae_gain_reg_map[gi];
	uint32_t lo    = total * TR_AE_STEP_DEN / TR_AE_STEP_NUM;
	uint32_t hi    = total * TR_AE_STEP_NUM / TR_AE_STEP_DEN;
	/* Log-domain step: exact for a linear scene, limited to x/÷ 1.25. */
	uint32_t want = mean != 0u ? total * TR_AE_TARGET_MEAN / mean : hi;

	want = want < lo ? lo : want > hi ? hi : want;

	uint16_t e;
	uint8_t  g;

	split_total(want, exposure_min, exposure_max, gain_idx_max, &e, &g);
	if (e == ae->exposure && g == gi) {
		/* The step rounds away (short exposures): one quantum the right way. */
		if (err < 0) {
			if (e < exposure_max) {
				e++;
			} else if (g < gain_idx_max) {
				g++;
			}
		} else if (g > 0u) {
			g--;
		} else if (e > exposure_min) {
			e--;
		}
	}
	if (e == ae->exposure && g == ae->gain_idx) {
		return false; /* saturated in the needed direction */
	}
	ae->exposure = e;
	ae->gain_idx = g;
	ae->settle   = TR_AE_SETTLE_FRAMES;
	return true;
}

uint32_t tr_ae_exposure_max_from_vts(uint16_t vts, uint16_t margin)
{
	/* fix round 13 (maintainer correction of the round-11/12 story): NO
	 * <<4 here -- OV9281_FETCH_EXP_H/M/L (chips/ov9281/zephyr/drivers/
	 * video/ov9281.c) already take a WHOLE-LINE value and do the <<4
	 * themselves when they split it across the three registers (H gets
	 * bits [15:12] of the line count, M bits [11:4], L's own upper
	 * nibble bits [3:0] -- together exactly `lines << 4` once the bytes
	 * are read back as one 20-bit field). OV9281_EXPOSURE_DEFAULT (0x2a9
	 * = 681) IS 681 WHOLE LINES -- proven by the sensor's own reset-
	 * default register bytes (00/2a/90), which are exactly what
	 * FETCH_H/M/L(681) produce, not FETCH_H/M/L(681*16). Round 11 shifted
	 * this ceiling left 4 believing the fetch macros expected 1/16-line
	 * units already; round 12 kept that shift when it started writing
	 * these registers directly, so a ceiling of 1071 lines became a
	 * register write of 1071*16 = 17136 -- 17136 lines of exposure
	 * against a 1096-line frame, silicon-confirmed DARKER (mean 8 vs
	 * 16.5) since it is nowhere near a valid line count. tr_ae_exposure_
	 * regs()'s own test (tests/host/test_camera_ae.c) now pins the
	 * fetch macros' real behaviour directly: 681 lines -> 00/2a/90, 1071
	 * lines -> 00/42/f0. The driver's own range.max = mode->vts -
	 * OV9281_EXP_MAX_OFFSET was ALWAYS correct, whole lines, never
	 * needing this fix at all -- finding A's real bug (round 12) was
	 * genuinely the driver's clamp firing on an over-shifted value this
	 * function was handing it, not the clamp's own units. */
	return vts > margin ? (uint32_t)(vts - margin) : 0u;
}

void tr_ae_exposure_regs(uint16_t val, uint8_t *h, uint8_t *m, uint8_t *l)
{
	*h = (uint8_t)((val >> 12) & 0xFu);
	*m = (uint8_t)((val >> 4) & 0xFFu);
	*l = (uint8_t)((val & 0xFu) << 4);
}
