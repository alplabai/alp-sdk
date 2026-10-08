/* tests/host/test_r3d_panel_rot.c -- src/render/panel_rot.h: the portrait
 * game on a panel mounted turned (the RVT121, mount-rotation 90). For rotation 90 and 270,
 * and for both panel widths (pw 800, the Riverdi's whole render, and 720, the RK055's centre
 * crop): the corners land where the direction says, the whole pw x 1280 frame is a
 * bijection onto the 1280 x pw layer-1 window (and the HUD's 720 x 352
 * onto its 352 x 720 layer-2 window), a blit equals the per-pixel mapping,
 * and -- on the A32 build -- the NEON transpose blit is bit-exact against the
 * scalar one. Named test_r3d_*.c so tests/host/runner.sh's "A32 qemu" stage
 * cross-compiles and qemu-runs it too: the only place the NEON path runs off
 * silicon. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/render/panel_rot.h"
#include "../../src/ipc/tr_mbox.h" /* TR_FB_SLOT_SIZE */

#define WMAX TR_R3D_W /* the widest picture: 800 */
#define H    TR_ROT_PORTRAIT_H
#define LW   TR_ROT_PORTRAIT_H /* the layer-1 window: 1280 x pw */
#define HW   720               /* the HUD's width (TR_HUD_W): its surface is 352 x 720 */

static uint16_t rot_px(int x, int y)
{
	return (uint16_t)(x * 31 + y * 7 + (x ^ y));
}

/* Landscape (X, Y) of the portrait pixel (x, y): the direction, stated
 * independently of tr_rot_idx(). */
static void land(int rot, int wl, int pw, int x, int y, int *X, int *Y)
{
	if (rot == 90) { /* clockwise: portrait top -> landscape right */
		*X = wl - 1 - y;
		*Y = x;
	} else { /* 270, anticlockwise: portrait top -> landscape left */
		*X = y;
		*Y = pw - 1 - x;
	}
}

static void check_rotation(int rot, int W)
{
	static uint8_t hit[LW * WMAX];
	int            LH = W; /* the layer-1 window is 1280 wide and pw tall */
	int            X, Y;

	/* Corners and the direction: 90 puts the portrait's top-left at the
	 * landscape top-right, 270 at the bottom-left. */
	land(rot, LW, W, 0, 0, &X, &Y);
	assert(tr_rot_idx(rot, LW, W, 0, 0) == (uint32_t)(Y * LW + X));
	assert(rot == 90 ? (X == LW - 1 && Y == 0) : (X == 0 && Y == LH - 1));
	land(rot, LW, W, W - 1, 0, &X, &Y);
	assert(tr_rot_idx(rot, LW, W, W - 1, 0) == (uint32_t)(Y * LW + X));
	assert(rot == 90 ? (X == LW - 1 && Y == LH - 1) : (X == 0 && Y == 0));
	land(rot, LW, W, 0, H - 1, &X, &Y);
	assert(rot == 90 ? (X == 0 && Y == 0) : (X == LW - 1 && Y == LH - 1));
	assert(tr_rot_idx(rot, LW, W, 0, H - 1) == (uint32_t)(Y * LW + X));
	land(rot, LW, W, W - 1, H - 1, &X, &Y);
	assert(rot == 90 ? (X == 0 && Y == LH - 1) : (X == LW - 1 && Y == 0));
	assert(tr_rot_idx(rot, LW, W, W - 1, H - 1) == (uint32_t)(Y * LW + X));

	/* Every portrait pixel maps inside the window, no two to one cell. */
	memset(hit, 0, sizeof(hit));
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t i = tr_rot_idx(rot, LW, W, x, y);

			land(rot, LW, W, x, y, &X, &Y);
			assert(i == (uint32_t)(Y * LW + X) && i < (uint32_t)(LW * LH));
			assert(!hit[i]);
			hit[i] = 1;
		}
	}
	for (int i = 0; i < LW * LH; i++) {
		assert(hit[i]); /* onto, too: the whole 1280 x pw window is covered */
	}

	/* The HUD: 720 x 352 portrait -> 352 x 720, a bijection too. */
	{
		static uint8_t hh[TR_ROT_HUD_W * HW];

		memset(hh, 0, sizeof(hh));
		for (int y = 0; y < TR_ROT_HUD_W; y++) {
			for (int x = 0; x < HW; x++) {
				uint32_t i = tr_rot_idx(rot, TR_ROT_HUD_W, HW, x, y);

				land(rot, TR_ROT_HUD_W, HW, x, y, &X, &Y);
				assert(i == (uint32_t)(Y * TR_ROT_HUD_W + X) && i < sizeof(hh));
				assert(!hh[i]);
				hh[i] = 1;
			}
		}
		for (unsigned i = 0; i < sizeof(hh); i++) {
			assert(hh[i]);
		}
	}

	/* The HUD window sits on the side the portrait top (rows 0..351) lands:
	 * the layer-1 X of those rows is exactly the window's columns. */
	{
		int x0 = rot == 90 ? LW - TR_ROT_HUD_W : 0, lo = LW, hi = -1;

		for (int y = 0; y < TR_ROT_HUD_W; y++) {
			land(rot, LW, W, 0, y, &X, &Y);
			lo = X < lo ? X : lo;
			hi = X > hi ? X : hi;
		}
		assert(lo == x0 && hi == x0 + TR_ROT_HUD_W - 1);
	}
}

/* tr_rot_blit() against the per-pixel mapping, for a band (pw x 32 at a
 * 32-row boundary, what render_band() copies) and an odd block. */
static void check_blit(int rot, int W)
{
	static uint16_t src[WMAX * 32], a[LW * WMAX], b[LW * WMAX];
	int             LH = W;
	struct {
		int x0, y0, w, h;
	} blk[] = { { 0, 0, W, 32 }, { 0, 640, W, 32 }, { 0, 1248, W, 32 }, { 3, 5, 100, 11 } };

	for (unsigned k = 0; k < sizeof(blk) / sizeof(blk[0]); k++) {
		memset(a, 0, sizeof(a));
		memset(b, 0, sizeof(b));
		for (int y = 0; y < blk[k].h; y++) {
			for (int x = 0; x < blk[k].w; x++) {
				src[y * W + x] = rot_px(blk[k].x0 + x, blk[k].y0 + y);
				b[tr_rot_idx(rot, LW, W, blk[k].x0 + x, blk[k].y0 + y)] = src[y * W + x];
			}
		}
		tr_rot_blit(rot, a, LW, W, src, W, blk[k].x0, blk[k].y0, blk[k].w, blk[k].h);
		assert(memcmp(a, b, (size_t)LW * (size_t)LH * 2u) == 0);
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		if (blk[k].w % 8 == 0 && blk[k].h % 8 == 0 && blk[k].x0 % 8 == 0 && blk[k].y0 % 8 == 0) {
			static uint16_t n[LW * WMAX] __attribute__((aligned(16)));
			static uint16_t s16[WMAX * 32] __attribute__((aligned(16)));

			memcpy(s16, src, sizeof(s16));
			memset(n, 0, sizeof(n));
			tr_rot_blit_neon(rot, n, LW, W, s16, W, blk[k].x0, blk[k].y0, blk[k].w, blk[k].h);
			assert(memcmp(n, b, (size_t)LW * (size_t)LH * 2u) == 0);
			printf("  neon block %u ok\n", k);
		}
#endif
	}

	/* The whole frame, band by band, round-trips: read each landscape cell
	 * back through the inverse mapping and get the portrait pixel. */
	{
		static uint16_t frame[LW * WMAX], band[WMAX * 32];

		memset(frame, 0, sizeof(frame));
		for (int b0 = 0; b0 < H; b0 += 32) {
			for (int y = 0; y < 32; y++) {
				for (int x = 0; x < W; x++) {
					band[y * W + x] = rot_px(x, b0 + y);
				}
			}
			tr_rot_blit(rot, frame, LW, W, band, W, 0, b0, W, 32);
		}
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				int X, Y;

				land(rot, LW, W, x, y, &X, &Y);
				assert(frame[Y * LW + X] == rot_px(x, y));
			}
		}
	}
}

int main(void)
{
	/* Rotation off: the plain portrait index. */
	assert(tr_rot_idx(0, 0, 720, 5, 7) == 7u * 720u + 5u);
	assert(tr_rot_idx(0, 0, 800, 5, 7) == 7u * 800u + 5u); /* the pitch is the picture's width */

	assert(LW - TR_ROT_HUD_W == 928); /* the 90 HUD window starts at X 928 */
	assert(tr_rot_valid(0) && tr_rot_valid(90) && tr_rot_valid(270));
	assert(!tr_rot_valid(180) && !tr_rot_valid(45) && !tr_rot_valid(-90));
	assert((uint32_t)LW * 800u * 2u ==
	       TR_FB_SLOT_SIZE); /* the widest picture IS the placement slot */
	assert((uint32_t)LW * 720u * 2u <= TR_FB_SLOT_SIZE); /* a narrower one fits it */
	assert((uint32_t)TR_ROT_HUD_W * HW * 2u == 506880u); /* tr_mbox.h TR_HUD_FB_SIZE */
	assert(WMAX == 800);

	for (int pw = 720; pw <= 800; pw += 80) {
		check_rotation(90, pw);
		check_rotation(270, pw);
		check_blit(90, pw);
		check_blit(270, pw);
	}
	puts("panel rotation ok");
	return 0;
}
