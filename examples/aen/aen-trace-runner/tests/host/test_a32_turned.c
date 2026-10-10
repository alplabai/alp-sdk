/* tests/host/test_a32_turned.c -- the A32 renderer's frame for a panel mounted
 * turned (a32/renderer/render.c, frame_rot from tr_frame_in_t.rotation): a
 * frame rendered with rotation 90 / 270 is, pixel for pixel, the rotation 0
 * frame (the portrait golden path every RK055 CRC in test_a32_scene.c /
 * test_a32_render.c pins) put through the mapping of render/panel_rot.h. The same holds for a
 * narrower panel (in.fw 720, the RK055): its frame is the CENTRE 720 columns of the full
 * 800-column render, cropped, never rescaled -- at rotation 0 and turned.
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
static uint16_t fbn[W * H] __attribute__((aligned(16))); /* the narrower panel's frame */

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
	tr_cam_view_write(
	    &host_cam_view_mem, TR_MEM_CAM_POOL, 42u, TR_CAM_SRC_W, TR_CAM_SRC_H, 0u, 1u, pip_barrier);
	{
		tr_pose_t pose = { 0 };

		pose.kp[TR_KP_NOSE] = (tr_kp_t){ 320, 200, 255 };
		pose.kp[TR_KP_LSHO] = (tr_kp_t){ 250, 250, 255 };
		pose.kp[TR_KP_RSHO] = (tr_kp_t){ 390, 250, 255 };
		pose.kp[TR_KP_LELB] = (tr_kp_t){ 200, 330, 255 };
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
		in[f].fw       = W;
		draw(&in[f], fb0, 0x5A);
		/* the skeleton overlay really drew (nose dot on its image pixel) */
		assert(fb0[(TR_VID_Y0 + 256) * W + 400] == COLOR_KP);
		for (unsigned r = 0; r < sizeof(rots) / sizeof(rots[0]); r++) {
			in[f].rotation = (uint16_t)rots[r];
			draw(&in[f], fbr, 0xA5);
			for (int y = 0; y < H; y++) {
				for (int x = 0; x < W; x++) {
					assert(fbr[tr_rot_idx(rots[r], TR_R3D_H, W, x, y)] == fb0[y * W + x]);
				}
			}
			printf("a32 turned frame %d rotation %d: matches the portrait frame\n", f, rots[r]);
		}
		/* The intent lamps hold a lane step lit for LAMP_HOLD_TICKS draws (render.c
		 * video_frame_state) and count DRAWS, so frames drawn in between would differ from the
		 * reference by a lamp going dark: drain them, then redraw the reference. */
		in[f].rotation = 0;
		in[f].fw       = W;
		for (int i = 0; i <= LAMP_HOLD_TICKS; i++) {
			draw(&in[f], fb0, 0x5A);
		}
		/* The 720-wide panel: the centre columns [40, 760) of the 800-wide frame, pitch 720,
		 * and turned the same way. Poisoned beyond the bytes it owns: nothing may touch them. */
		for (int rot = 0; rot <= 270; rot = rot == 0 ? 90 : rot + 180) {
			enum { FW = 720, X0 = (W - FW) / 2 };

			in[f].rotation = (uint16_t)rot;
			in[f].fw       = FW;
			draw(&in[f], fbn, 0xA5);
			/* The crash flash's border frames the PANEL (r3d_scene.c flash_border): at the crop's
			 * edge, not the render's, so a flash frame differs from the crop of the full-width
			 * one exactly in the game view's outer 16 px (and the border really is there). */
			for (int y = 0; y < H; y++) {
				for (int x = 0; x < FW; x++) {
					int edge = (in[f].flags & TR_FLAG_CRASH) && y < TR_VIEW_H &&
					           (x < 16 || x >= FW - 16 || y < 16 || y >= TR_VIEW_H - 16);

					/* the plate (lamps, label) is laid out over the VISIBLE columns, so it
					 * differs by design from the crop of the full-width one */
					int plate = y >= TR_VID_Y0 + PLATE_Y;
					/* ... and the A32 sprite score is drawn at the PANEL's left edge, not the
					 * render's: checked on its own below */
					int score = render_hud && !(in[f].flags & TR_FLAG_HUD_L2) && y < 64 && x < 160;

					if (!edge && !plate && !score) {
						assert(fbn[tr_rot_idx(rot, TR_R3D_H, FW, x, y)] == fb0[y * W + X0 + x]);
					}
				}
			}
			if (in[f].flags & TR_FLAG_CRASH) {
				uint16_t l = fbn[tr_rot_idx(rot, TR_R3D_H, FW, 0, TR_VIEW_H / 2)];
				uint16_t r = fbn[tr_rot_idx(rot, TR_R3D_H, FW, FW - 1, TR_VIEW_H / 2)];

				assert(l == r &&
				       l != fb0[(TR_VIEW_H / 2) * W + X0]); /* the border at the crop edge */
			}
			if (render_hud && !(in[f].flags & TR_FLAG_HUD_L2)) {
				/* the score digits start HUD_X0 in from the PANEL's left edge: the pixels the sprite
				 * HUD adds (a frame with it, less one without) are, on the crop, where the full-width
				 * frame has them HUD_X0 in from the render's edge -- and there are some */
				static uint16_t wide_off[W * H], narrow_off[W * H];
				int             shown = 0;

				render_hud     = 0;
				in[f].fw       = W;
				in[f].rotation = 0;
				draw(&in[f], wide_off, 0x5A);
				in[f].fw       = FW;
				in[f].rotation = (uint16_t)rot;
				draw(&in[f], narrow_off, 0xA5);
				render_hud = 1;
				for (int y = 0; y < 64; y++) {
					for (int x = 0; x < 160; x++) {
						int digit_w = fb0[y * W + x] != wide_off[y * W + x];
						int digit_n = fbn[tr_rot_idx(rot, TR_R3D_H, FW, x, y)] !=
						              narrow_off[tr_rot_idx(rot, TR_R3D_H, FW, x, y)];

						assert(digit_w == digit_n);
						shown += digit_n;
					}
				}
				assert(shown > 20); /* the digits really are on the crop, not under it */
			}
			for (size_t i = (size_t)FW * H; i < (size_t)W * H; i++) {
				assert(fbn[i] == 0xA5A5u);
			}
			printf("a32 turned frame %d fw %d rotation %d: the centre crop\n", f, FW, rot);
		}
	}
	puts("a32 turned ok");
	return 0;
}
