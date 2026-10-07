/* src/render/panel_rot.h -- the portrait game on a landscape panel.
 *
 * The game is 720 x 1280 portrait everywhere (TR_R3D_W x TR_R3D_H, the
 * walls, the sprites, the HUD). The Riverdi RVT121 scans out 1280 x 800
 * landscape and is mounted on its side, so with -DTR_PANEL_ROTATE=90|270
 * (CMake: TR_PANEL=rvt121) the portrait picture is rotated at the last
 * step, when it is written to a scan-out buffer:
 *
 *   layer 1: a 1280 x 720 landscape window, centred on the 800 rows
 *            (y 40..759, black bars above and below); 1280 * 720 * 2 B is the
 *            720 x 1280 * 2 B the framebuffers already are, so no address in
 *            tr_mbox.h / tr_memmap.h moves;
 *   layer 2: the HUD's 720 x 352 becomes a 352 x 720 window, at the panel
 *            side the portrait top edge lands on.
 *
 * ROTATE 90 turns the picture clockwise: portrait top -> landscape right,
 * portrait pixel (x, y) -> landscape (X, Y) = (1279 - y, x). ROTATE 270
 * turns it anticlockwise: (X, Y) = (y, 719 - x). The mapping is defined
 * once, here, and both cores' writers and the layer-2 window placement use
 * it; 0 (the default, RK055) is the plain portrait buffer, y * 720 + x.
 *
 * A rotated surface is `wl` px wide (the portrait rows it covers: 1280 for
 * layer 1, 352 for the HUD) and 720 px tall, row pitch wl. Writing a column
 * run (+1 in y) is therefore a contiguous run in memory, which is what the
 * blits below walk: an uncached framebuffer takes whole 16-byte stores.
 */
#ifndef TR_PANEL_ROT_H
#define TR_PANEL_ROT_H

#include <stdint.h>

#ifndef TR_PANEL_ROTATE
#define TR_PANEL_ROTATE 0
#endif
_Static_assert(TR_PANEL_ROTATE == 0 || TR_PANEL_ROTATE == 90 || TR_PANEL_ROTATE == 270,
               "TR_PANEL_ROTATE: 0 (portrait panel), 90 or 270");

#define TR_ROT_PORTRAIT_W 720  /* the game's width (== TR_R3D_W) */
#define TR_ROT_PORTRAIT_H 1280 /* the game's height (== TR_R3D_H) */

/* The scan-out window of layer 1 on the 1280 x 800 panel. */
#define TR_ROT_L1_W  1280
#define TR_ROT_L1_H  720
#define TR_ROT_L1_Y0 40 /* (800 - 720) / 2 */

/* The HUD layer (layer 2): TR_HUD_H portrait rows (hud.h, 352) -> that many
 * landscape columns. Its window is TR_ROT_HUD_W x 720 at (tr_rot_hud_x0, 40). */
#define TR_ROT_HUD_W       352
#define TR_ROT_HUD_X0(rot) ((rot) == 90 ? TR_ROT_L1_W - TR_ROT_HUD_W : 0)

/* Index (in px) of portrait pixel (x, y) in a surface `wl` px wide rotated by
 * `rot` (0, 90, 270; 0 ignores wl and is the 720-wide portrait buffer). */
static inline uint32_t tr_rot_idx(int rot, uint32_t wl, int x, int y)
{
	if (rot == 90) {
		return (wl - 1u - (uint32_t)y) + (uint32_t)x * wl;
	}
	if (rot == 270) {
		return (uint32_t)y + (uint32_t)(TR_ROT_PORTRAIT_W - 1 - x) * wl;
	}
	return (uint32_t)y * TR_ROT_PORTRAIT_W + (uint32_t)x;
}

/* Copy a w x h block of portrait pixels (src: row-major, `pitch` px per row)
 * to portrait position (x0, y0) of the rotated surface dst. Column by column
 * so the stores run along the surface's rows. Any w, h, x0, y0. */
static inline void tr_rot_blit(int             rot,
                               uint16_t       *dst,
                               uint32_t        wl,
                               const uint16_t *src,
                               uint32_t        pitch,
                               int             x0,
                               int             y0,
                               int             w,
                               int             h)
{
	/* Walk the surface by strides: +1 in x and +1 in y, in px. */
	int32_t   dx  = rot == 90 ? (int32_t)wl : rot == 270 ? -(int32_t)wl : 1;
	int32_t   dy  = rot == 90 ? -1 : rot == 270 ? 1 : (int32_t)TR_ROT_PORTRAIT_W;
	uint16_t *col = dst + tr_rot_idx(rot, wl, x0, y0);

	for (int x = 0; x < w; x++, col += dx) {
		uint16_t *d = col;

		for (int y = 0; y < h; y++, d += dy) {
			*d = src[(uint32_t)y * pitch + (uint32_t)x];
		}
	}
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* 8 x 8 u16 transpose: out[k] = column k of in[0..7]. */
static inline void tr_rot_transpose8(const uint16x8_t in[8], uint16x8_t out[8])
{
	uint16x8x2_t t0 = vtrnq_u16(in[0], in[1]), t1 = vtrnq_u16(in[2], in[3]);
	uint16x8x2_t t2 = vtrnq_u16(in[4], in[5]), t3 = vtrnq_u16(in[6], in[7]);
	uint32x4x2_t u0 = vtrnq_u32(vreinterpretq_u32_u16(t0.val[0]), vreinterpretq_u32_u16(t1.val[0]));
	uint32x4x2_t u1 = vtrnq_u32(vreinterpretq_u32_u16(t0.val[1]), vreinterpretq_u32_u16(t1.val[1]));
	uint32x4x2_t u2 = vtrnq_u32(vreinterpretq_u32_u16(t2.val[0]), vreinterpretq_u32_u16(t3.val[0]));
	uint32x4x2_t u3 = vtrnq_u32(vreinterpretq_u32_u16(t2.val[1]), vreinterpretq_u32_u16(t3.val[1]));

#define TR_ROT_CAT(lo_or_hi, a, b) \
	vcombine_u16(vreinterpret_u16_u32(vget_##lo_or_hi##_u32(a)), \
	             vreinterpret_u16_u32(vget_##lo_or_hi##_u32(b)))
	out[0] = TR_ROT_CAT(low, u0.val[0], u2.val[0]);
	out[1] = TR_ROT_CAT(low, u1.val[0], u3.val[0]);
	out[2] = TR_ROT_CAT(low, u0.val[1], u2.val[1]);
	out[3] = TR_ROT_CAT(low, u1.val[1], u3.val[1]);
	out[4] = TR_ROT_CAT(high, u0.val[0], u2.val[0]);
	out[5] = TR_ROT_CAT(high, u1.val[0], u3.val[0]);
	out[6] = TR_ROT_CAT(high, u0.val[1], u2.val[1]);
	out[7] = TR_ROT_CAT(high, u1.val[1], u3.val[1]);
#undef TR_ROT_CAT
}

/* tr_rot_blit() for the renderer: whole 16-byte NEON stores. Needs w % 8 ==
 * 0, h % 8 == 0, h <= 32, x0 and y0 % 8 == 0 and 16-byte aligned dst / src /
 * pitch (the band copy: 720 x 32 at a 32-row boundary); anything else goes
 * through tr_rot_blit(). Per 8 columns it transposes the band's 8-row
 * groups, then writes each column's whole h-px run in one go.
 * ponytail: a column run is h * 2 = 64 B; the cores' write buffers merge it, not
 * measured on silicon -- a wider run needs a taller band. */
static inline void tr_rot_blit_neon(int             rot,
                                    uint16_t       *dst,
                                    uint32_t        wl,
                                    const uint16_t *src,
                                    uint32_t        pitch,
                                    int             x0,
                                    int             y0,
                                    int             w,
                                    int             h)
{
	int groups = h / 8;

	for (int x = 0; x < w; x += 8) {
		uint16x8_t out[4][8];

		for (int g = 0; g < groups; g++) {
			uint16x8_t in[8];

			for (int r = 0; r < 8; r++) {
				in[r] = vld1q_u16(src + (uint32_t)(g * 8 + r) * pitch + (uint32_t)x);
			}
			tr_rot_transpose8(in, out[g]);
		}
		for (int k = 0; k < 8; k++) {
			if (rot == 90) {
				/* y runs the other way: reverse each 8-group and the group order. */
				uint16_t *d = dst + tr_rot_idx(rot, wl, x0 + x + k, y0 + h - 1);

				for (int g = 0; g < groups; g++) {
					uint16x8_t v = out[g][k];

					vst1q_u16(
					    d + (groups - 1 - g) * 8,
					    vcombine_u16(vrev64_u16(vget_high_u16(v)), vrev64_u16(vget_low_u16(v))));
				}
			} else {
				uint16_t *d = dst + tr_rot_idx(rot, wl, x0 + x + k, y0);

				for (int g = 0; g < groups; g++) {
					vst1q_u16(d + g * 8, out[g][k]);
				}
			}
		}
	}
}
#endif /* __ARM_NEON */

#endif /* TR_PANEL_ROT_H */
