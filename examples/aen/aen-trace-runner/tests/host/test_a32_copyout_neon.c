/* tests/host/test_a32_copyout_neon.c -- render.c's band copy-out with the A32 build's NEON rot-90 /
 * rot-270 path (RENDER_A32=1: tr_rot_blit_neon inlined into copy_rows_turned()), against the
 * per-pixel mapping tr_rot_idx(), for the 800-wide render and the 720 centre crop and for 32, 24, 16
 * and 8-row bands. Only the NEON build runs it: tests/host/runner.sh's qemu-arm stage compiles it at the
 * renderer image's -O3 (a32/renderer/Makefile HOT_OPT); render.c's CP15 code is built but never run
 * here (qemu user mode has no CP15), the rest of the renderer is not called. A plain x86 host skips. */
#include <stdio.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <assert.h>
#include <string.h>

#define RENDER_A32       1
#define RENDER_DL_GOLDEN 0
#include "../../a32/common/crc32.c"
#include "../../a32/renderer/render.c"

static uint16_t cband[TR_R3D_W * TR_BAND_H] __attribute__((aligned(64)));
static uint16_t got[TR_R3D_H * TR_R3D_W] __attribute__((aligned(64)));
static uint16_t want[TR_R3D_H * TR_R3D_W] __attribute__((aligned(64)));

static void one(int rot, uint32_t fw, int x0c, int y_lo, int rows)
{
	frame_fw  = fw;
	frame_x0c = x0c;
	for (uint32_t i = 0; i < TR_R3D_W * TR_BAND_H; i++) {
		cband[i] = (uint16_t)(i * 2654435761u >> 11);
	}
	memset(got, 0xA5, sizeof(got));
	memset(want, 0xA5, sizeof(want));
	for (int r = 0; r < rows; r++) {
		for (uint32_t x = 0; x < fw; x++) {
			want[tr_rot_idx(rot, TR_R3D_H, fw, (int)x, y_lo + r)] =
			    cband[(uint32_t)r * TR_R3D_W + (uint32_t)x0c + x];
		}
	}
	if (rot == 90) {
		copy_rows_turned(90, got, cband, y_lo, rows);
	} else {
		copy_rows_turned(270, got, cband, y_lo, rows);
	}
	assert(memcmp(got, want, (size_t)TR_R3D_H * fw * 2u) == 0);
}

int main(void)
{
	static const int rows[] = { 32, 24, 16, 8 };

	for (int rot = 90; rot <= 270; rot += 180) {
		for (unsigned k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
			one(rot, TR_R3D_W, 0, 0, rows[k]);
			one(rot, TR_R3D_W, 0, 640, rows[k]);
			one(rot, 720, 40, 1248, rows[k]);
		}
	}
	printf("a32 copy-out NEON: rot 90/270, 800/720 wide, 32/24/16/8-row bands ok\n");
	return 0;
}
#else
int main(void)
{
	printf("SKIP: a32 copy-out NEON not compiled (host build)\n");
	return 0;
}
#endif
