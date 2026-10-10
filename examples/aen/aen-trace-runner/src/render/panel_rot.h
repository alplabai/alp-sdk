/* src/render/panel_rot.h -- the portrait game on a panel mounted turned.
 *
 * The game is TR_R3D_W x 1280 portrait everywhere (r3d.h, the walls, the
 * sprites), and a panel narrower than the render (the RK055, 720) shows its
 * centre `pw` columns (tr_frame_in_t.fw): the producer crops when it writes a
 * scan-out buffer, never scales. A panel whose controller scans the picture
 * turned from the way it is mounted (the Riverdi RVT121, 1280 x 800 scanned
 * landscape, mounted on its side) reports a `rotation` through
 * alp_display_caps_t (devicetree mount-rotation), and the producer turns the
 * portrait picture by that many degrees clockwise at the last step, as well:
 *
 *   layer 1: a 1280 x pw landscape window of the panel (the Riverdi's is the
 *            whole 1280 x 800); 1280 * pw * 2 B is the pw x 1280 * 2 B the
 *            framebuffer is, so no address in tr_mbox.h / tr_memmap.h moves
 *            (a slot is 800 x 1280 x 2 B, TR_FB_SLOT_SIZE);
 *   layer 2: the HUD's 720 x 352 becomes a 352 x 720 window, which the HE
 *            centres in the layer-1 window (platform/hud_l2.c).
 *
 * Rotation 90 turns the picture clockwise: portrait top -> landscape right,
 * portrait pixel (x, y) -> landscape (X, Y) = (1279 - y, x). Rotation 270
 * turns it anticlockwise: (X, Y) = (y, pw - 1 - x). The mapping is defined
 * once, here; both cores' writers and the layer-2 window placement use it.
 * Rotation 0 is the plain portrait buffer, y * pw + x.
 *
 * A rotated surface is `wl` px wide (the portrait rows it covers: 1280 for
 * layer 1, 352 for the HUD) and pw px tall, row pitch wl. Writing a column
 * run (+1 in y) is therefore a contiguous run in memory, which is what the
 * blits below walk: an uncached framebuffer takes whole 16-byte stores.
 */
#ifndef TR_PANEL_ROT_H
#define TR_PANEL_ROT_H

#include <stdint.h>

#include "r3d.h" /* TR_R3D_W: the widest frame the renderer draws */

#define TR_ROT_PORTRAIT_H 1280 /* the game's height (== TR_R3D_H) */

/* A rotation the renderer and HUD can produce: 0 (as scanned), 90 or 270. */
static inline int tr_rot_valid(int rot)
{
	return rot == 0 || rot == 90 || rot == 270;
}

/* The renderer refuses a frame over its rotation only if the frame will be drawn
 * (it has a valid framebuffer): a frame it would drop anyway, such as power-on
 * garbage on a cold mailbox, is no ABI violation. */
static inline int tr_rot_refuse(int drawn, unsigned rotation)
{
	return drawn && !tr_rot_valid((int)rotation);
}

/* The renderer refuses a frame whose panel width is not one it can write: the framebuffer is
 * fw px wide (the centre of the TR_R3D_W render), the band copy moves 16 px at a time, and a
 * panel wider than the render has nothing to fill it with. fw 0 (an HE that never set it) is
 * refused too. Like the rotation, only a frame that will be drawn is held to it. */
static inline int tr_fw_refuse(int drawn, unsigned fw)
{
	return drawn && !(fw % 16u == 0u && fw >= 16u && fw <= (unsigned)TR_R3D_W);
}

/* The HUD layer (layer 2): TR_HUD_H portrait rows (hud.h, 352) -> that many
 * landscape columns, at the layer-1 window's right edge for 90, left for 270. */
#define TR_ROT_HUD_W 352

/* Where the HUD's layer-2 window sits inside the layer-1 window (l1_w x l1_h, panel px; the
 * HUD window is win_w x win_h, hud_l2.c): flush with the edge the portrait top lands on (right
 * for 90, left for 270, the top for 0) and centred along it, so the 720-wide HUD sits over the
 * middle 720 columns of a wider picture. Offsets from the layer-1 window's top-left. */
static inline void tr_hud_window_off(int       rot,
                                     uint32_t  l1_w,
                                     uint32_t  l1_h,
                                     uint32_t  win_w,
                                     uint32_t  win_h,
                                     uint32_t *dx,
                                     uint32_t *dy)
{
	*dx = rot == 0 ? (l1_w - win_w) / 2u : rot == 90 ? l1_w - win_w : 0u;
	*dy = rot != 0 ? (l1_h - win_h) / 2u : 0u;
}

/* Index (in px) of portrait pixel (x, y) of a pw-wide picture in a surface `wl`
 * px wide rotated by `rot` (0, 90, 270; 0 ignores wl and is the pw-wide portrait
 * buffer). */
static inline uint32_t tr_rot_idx(int rot, uint32_t wl, uint32_t pw, int x, int y)
{
	if (rot == 90) {
		return (wl - 1u - (uint32_t)y) + (uint32_t)x * wl;
	}
	if (rot == 270) {
		return (uint32_t)y + (pw - 1u - (uint32_t)x) * wl;
	}
	return (uint32_t)y * pw + (uint32_t)x;
}

/* Copy a w x h block of portrait pixels (src: row-major, `pitch` px per row)
 * to portrait position (x0, y0) of the pw-wide picture in the rotated surface dst. Column by column
 * so the stores run along the surface's rows. Any w, h, x0, y0. */
static inline __attribute__((always_inline)) void tr_rot_blit(int             rot,
                                                              uint16_t       *dst,
                                                              uint32_t        wl,
                                                              uint32_t        pw,
                                                              const uint16_t *src,
                                                              uint32_t        pitch,
                                                              int             x0,
                                                              int             y0,
                                                              int             w,
                                                              int             h)
{
	/* Walk the surface by strides: +1 in x and +1 in y, in px. */
	int32_t   dx  = rot == 90 ? (int32_t)wl : rot == 270 ? -(int32_t)wl : 1;
	int32_t   dy  = rot == 90 ? -1 : rot == 270 ? 1 : (int32_t)pw;
	uint16_t *col = dst + tr_rot_idx(rot, wl, pw, x0, y0);

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
 * pitch (the band copy: fw x 32 at a 32-row boundary); anything else goes
 * through tr_rot_blit(). Per 8 columns it transposes the band's 8-row
 * groups, then writes each column's whole h-px run in one go, in ASCENDING
 * address order (a 32-row band: four back-to-back 16 B stores = one 64 B
 * aligned run for the Normal-NC framebuffer's write buffer to merge; the old
 * rot-90 form stored the groups in descending order). Same bytes either way:
 * tests/host/test_r3d_panel_rot.c and the qemu NEON stage hold it to the
 * scalar tr_rot_blit(); a32/payload-isa (stage_rot) times old vs ascending
 * vs VST1 {d0-d3} x2 vs VSTM. */
static inline __attribute__((always_inline)) void tr_rot_blit_neon(int             rot,
                                                                   uint16_t       *dst,
                                                                   uint32_t        wl,
                                                                   uint32_t        pw,
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
				uint16_t  *d = dst + tr_rot_idx(rot, wl, pw, x0 + x + k, y0 + h - 1);
				uint16x8_t v[4];

				for (int g = 0; g < groups; g++) {
					uint16x8_t t = out[g][k];

					v[groups - 1 - g] =
					    vcombine_u16(vrev64_u16(vget_high_u16(t)), vrev64_u16(vget_low_u16(t)));
				}
				for (int g = 0; g < groups; g++) {
					vst1q_u16(d + g * 8, v[g]);
				}
			} else {
				uint16_t *d = dst + tr_rot_idx(rot, wl, pw, x0 + x + k, y0);

				for (int g = 0; g < groups; g++) {
					vst1q_u16(d + g * 8, out[g][k]);
				}
			}
		}
	}
}
#endif /* __ARM_NEON */

#endif /* TR_PANEL_ROT_H */
