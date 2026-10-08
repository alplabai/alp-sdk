/* tests/host/test_a32_turned.c -- the A32 renderer's frame for a panel mounted
 * turned (a32/renderer/render.c, frame_rot from tr_frame_in_t.rotation): a
 * frame rendered with rotation 90 / 270 is, pixel for pixel, the rotation 0
 * frame (the portrait golden path every RK055 CRC in test_a32_scene.c /
 * test_a32_render.c pins) put through the mapping of render/panel_rot.h.
 * Whole frames, 3D bands, HUD and the video half included -- with a running HP
 * slot (camera view, pose) seeded, so the camera image and the skeleton overlay
 * (cv_rect writing the turned framebuffer directly) are part of the comparison. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

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
	uint8_t      *pool   = mmap((void *)(uintptr_t)TR_MEM_CAM_POOL,
	                            TR_MEM_CAM_POOL_SIZE,
	                            PROT_READ | PROT_WRITE,
	                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
	                            -1,
	                            0);

	/* A published camera frame at the address CAM_POOL has on silicon (the renderer
	 * range-checks it), a running HP and a skeleton: test_a32_video.c's setup. */
	assert(pool == (uint8_t *)(uintptr_t)TR_MEM_CAM_POOL);
	for (int i = 0; i < TR_CAM_SRC_W * TR_CAM_SRC_H; i++) {
		pool[i] = (uint8_t)(i * 2654435761u >> 24);
	}
	host_hp_dbg_mem.magic       = TR_HP_DBG_MAGIC;
	host_hp_dbg_mem.loop_hz_x10 = 251u;
	tr_cam_view_write(&host_cam_view_mem,
	                  TR_MEM_CAM_POOL,
	                  42u,
	                  TR_CAM_SRC_W,
	                  TR_CAM_SRC_H,
	                  270u,
	                  1u,
	                  pip_barrier);
	{
		tr_pose_t pose = { 0 };

		pose.kp[TR_KP_NOSE] = (tr_kp_t){ 200, 300, 255 };
		pose.kp[TR_KP_LSHO] = (tr_kp_t){ 120, 380, 255 };
		pose.kp[TR_KP_RSHO] = (tr_kp_t){ 280, 380, 255 };
		pose.kp[TR_KP_LELB] = (tr_kp_t){ 90, 470, 255 };
		tr_pslot_write(&host_pslot_mem, &pose, 0u, 0u, TR_HP_STATE_RUNNING, NULL, 1u, pip_barrier);
	}

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
		/* the skeleton overlay really drew (nose dot on its image pixel) */
		assert(fb0[(TR_VID_Y0 + 300) * W + 160 + 200] == COLOR_KP);
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
