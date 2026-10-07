/* tests/host/test_a32_turned.c -- the A32 renderer's frame for a panel mounted
 * turned (a32/renderer/render.c, frame_rot from tr_frame_in_t.rotation): a
 * frame rendered with rotation 90 / 270 is, pixel for pixel, the rotation 0
 * frame (the portrait golden path every RK055 CRC in test_a32_scene.c /
 * test_a32_render.c pins) put through the mapping of render/panel_rot.h.
 * Whole frames, 3D bands, HUD and the video half included. */
#include <assert.h>
#include <stdio.h>

#define RENDER_DL_GOLDEN 0
#include "../../a32/common/crc32.c"
#include "../../a32/renderer/render.c"
#include "tr_scene_golden.h"

#define W TR_R3D_W
#define H TR_R3D_H

static uint16_t fb0[W * H] __attribute__((aligned(16)));
static uint16_t fbr[W * H] __attribute__((aligned(16)));

static void draw(const tr_frame_in_t *in, uint16_t *fb, uint8_t poison)
{
	memset(fb, poison, sizeof(uint16_t) * W * H);
	render_init(); /* the same scene state every time */
	render_frame(in, fb);
}

int main(void)
{
	tr_frame_in_t in[3];
	const int     rots[] = { 90, 270 };

	in[0] = tr_scene_golden_in(1234, 1);
	in[1] = tr_scene_golden_in(1235, 0);
	in[1].score += 10;
	in[2]            = in[1];
	in[2].flags      = TR_FLAG_CRASH;
	in[2].ents[4]    = (tr_pkt_ent_t){ 1, 0, 1, 0, 1110, 0 };
	in[2].crash_ent  = 4;
	in[2].crash_kind = TR_CRASH_KIND_LOW;

	for (int f = 0; f < 3; f++) {
		render_hud     = f != 0; /* HUD on the sprite path for frames 1, 2 */
		in[f].rotation = 0;
		draw(&in[f], fb0, 0x5A);
		for (unsigned r = 0; r < sizeof(rots) / sizeof(rots[0]); r++) {
			in[f].rotation = (uint16_t)rots[r];
			draw(&in[f], fbr, 0xA5);
			for (int y = 0; y < H; y++) {
				for (int x = 0; x < W; x++) {
					assert(fbr[tr_rot_idx(rots[r], TR_R3D_H, x, y)] == fb0[y * W + x]);
				}
			}
			printf("a32 turned frame %d rotation %d: matches the portrait frame\n", f, rots[r]);
		}
	}
	puts("a32 turned ok");
	return 0;
}
