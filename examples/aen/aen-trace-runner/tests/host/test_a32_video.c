/* tests/host/test_a32_video.c -- the A32 renderer's half/half video area
 * (a32/renderer/render.c render_video_band()/render_video_overlay()) end to
 * end on the host: a real frame published through tr_cam_view_t exactly as
 * hp_vision does, its pixels at the SAME address range CAM_POOL has on
 * silicon (mmap'd there, so the renderer's own range check passes
 * unmodified):
 *   - rows [0, TR_VIEW_H) are never touched by the video pass;
 *   - every video-area row is written (no stale framebuffer shows through);
 *   - the camera turned upright at native 1:1, centred at x 200..599 (270
 *     and 90), or 640x400 at x 80, row 120 (0);
 *   - the lamps (BOTH ARMS lit from air_ticks, LEFT ARM unlit) down the left
 *     strip of the portrait layout and along the top letterbox of the
 *     landscape one, the live Hz label, a skeleton keypoint on its image
 *     pixel;
 *   - bands in any order, on either core's scratch, give the same frame. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

#define RENDER_DL_GOLDEN 0
#include "../../a32/common/crc32.c"
#include "../../a32/renderer/render.c"

static uint16_t fb[TR_R3D_W * TR_R3D_H] __attribute__((aligned(16)));
static uint16_t fb2[TR_R3D_W * TR_R3D_H] __attribute__((aligned(16)));
static uint16_t ref[TR_CAM_SRC_W * TR_VID_W];

static uint16_t at(const uint16_t *f, int x, int vy) /* vy: video-area row */
{
	return f[(TR_VID_Y0 + vy) * TR_R3D_W + x];
}

static void frame(const tr_frame_in_t *in, uint16_t *f)
{
	for (int i = 0; i < TR_R3D_W * TR_R3D_H; i++) {
		f[i] = 0xDEADu;
	}
	render_setup(in);
	render_video_panel(f);
}

int main(void)
{
	uint8_t *pool = mmap((void *)(uintptr_t)TR_MEM_CAM_POOL,
	                     TR_MEM_CAM_POOL_SIZE,
	                     PROT_READ | PROT_WRITE,
	                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
	                     -1,
	                     0);

	assert(pool ==
	       (uint8_t *)(uintptr_t)TR_MEM_CAM_POOL); /* the renderer range-checks against this */
	for (int i = 0; i < TR_CAM_SRC_W * TR_CAM_SRC_H; i++) {
		pool[i] = (uint8_t)(i * 2654435761u >> 24);
	}
	render_init();

	tr_frame_in_t in = { 0 };

	in.flags     = TR_FLAG_ALIVE | TR_FLAG_HUD_L2;
	in.lane      = 1;
	in.air_ticks = 5u; /* JUMP lit */
	in.fw        = TR_R3D_W;

	/* 1. nothing published: the area is background + strips, the game rows untouched */
	frame(&in, fb);
	for (int y = 0; y < TR_VIEW_H; y++) {
		for (int x = 0; x < TR_R3D_W; x++) {
			assert(fb[y * TR_R3D_W + x] == 0xDEADu);
		}
	}
	for (int vy = 0; vy < TR_VID_H; vy++) {
		for (int x = 0; x < TR_R3D_W; x++) {
			assert(at(fb, x, vy) != 0xDEADu);
			if (x >= tr_cam_img_x0(90) && x < tr_cam_img_x0(90) + 400) {
				assert(at(fb, x, vy) == COLOR_PANEL_BG);
			}
		}
	}

	/* 2. published, rotated 270 and 90: the upright image at native 1:1 */
	host_hp_dbg_mem.magic       = TR_HP_DBG_MAGIC;
	host_hp_dbg_mem.loop_hz_x10 = 251u;
	for (int rot = 90; rot <= 270; rot += 180) {
		tr_cam_view_write(&host_cam_view_mem,
		                  TR_MEM_CAM_POOL,
		                  42u,
		                  TR_CAM_SRC_W,
		                  TR_CAM_SRC_H,
		                  (uint16_t)rot,
		                  1u,
		                  pip_barrier);
		frame(&in, fb);
		tr_cam_rot_rows(pool, TR_CAM_SRC_W, TR_CAM_SRC_H, rot, 0, TR_CAM_SRC_W, ref, TR_VID_W);
		for (int vy = 0; vy < TR_VID_H; vy++) {
			assert(memcmp(&fb[(TR_VID_Y0 + vy) * TR_R3D_W + tr_cam_img_x0(rot)],
			              &ref[vy * TR_VID_W],
			              400u * 2u) == 0);
		}
		assert(at(fb, 2, 2) == COLOR_PANEL_BG &&
		       at(fb, TR_R3D_W - 3, TR_VID_H - 3) == COLOR_PANEL_BG); /* no border */
		/* lamps: cells of TR_VID_H/4, the square 16 px down, centred in the left strip */
		assert(at(fb, 80, 0 * LAMP_CELL_H + 16 + LAMP_SQ / 2) == COLOR_LAMP_OFF); /* LEFT */
		assert(at(fb, 80, 2 * LAMP_CELL_H + 16 + LAMP_SQ / 2) == COLOR_LAMP_ON);  /* JUMP */
		assert(at(fb, 80, 3 * LAMP_CELL_H + 16 + LAMP_SQ / 2) == COLOR_LAMP_OFF); /* DUCK */
		/* the live Hz line, in green, in the right strip */
		int green = 0;

		for (int vy = 160; vy < 160 + 5 * 5; vy++) {
			for (int x = TR_R3D_W - VID_STRIP_W; x < TR_R3D_W; x++) {
				green += at(fb, x, vy) == COLOR_KP;
			}
		}
		assert(green > 100);
	}

	/* 3. the skeleton: a keypoint lands on the image pixel it names */
	{
		tr_pose_t pose = { 0 };

		pose.kp[TR_KP_NOSE] = (tr_kp_t){ 200, 300, 255 };
		tr_pslot_write(&host_pslot_mem, &pose, 0u, 0u, TR_HP_STATE_RUNNING, NULL, 1u, pip_barrier);
		frame(&in, fb);
		assert(at(fb, tr_cam_img_x0(270) + 200, 300) == COLOR_KP &&
		       at(fb, tr_cam_img_x0(270) + 201, 301) == COLOR_KP);
		assert(at(fb, tr_cam_img_x0(270) + 203, 300) != COLOR_KP); /* a 3x3 dot, not a smear */
	}

	/* 4. bands in reverse order, alternating cores: the same frame */
	{
		for (int i = 0; i < TR_R3D_W * TR_R3D_H; i++) {
			fb2[i] = 0xDEADu;
		}
		render_setup(&in);
		for (int vb = TR_VIDEO_BANDS - 1; vb >= 0; vb--) {
			render_video_band((uint32_t)(vb & 1), vb, fb2);
		}
		render_video_overlay(fb2);
		frame(&in, fb);
		assert(memcmp(fb, fb2, sizeof(fb)) == 0);
	}

	/* 5. rotation 0, the landscape comparison path: 640x400 at x 80, row 120 */
	tr_pslot_write(&host_pslot_mem,
	               &(tr_pose_t){ 0 },
	               0u,
	               0u,
	               TR_HP_STATE_NO_FRAME,
	               NULL,
	               2u,
	               pip_barrier); /* no skeleton */
	tr_cam_view_write(
	    &host_cam_view_mem, TR_MEM_CAM_POOL, 43u, TR_CAM_SRC_W, TR_CAM_SRC_H, 0u, 0u, pip_barrier);
	frame(&in, fb);
	for (int y = 0; y < TR_CAM_SRC_H; y++) {
		for (int x = tr_cam_img_x0(0); x < tr_cam_img_x0(0) + TR_CAM_SRC_W;
		     x++) { /* the whole picture: lamps/label are letterboxed */
			uint8_t g = pool[y * TR_CAM_SRC_W + x - tr_cam_img_x0(0)];

			assert(at(fb, x, 120 + y) == (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3)));
		}
	}
	assert(at(fb, 200, 60) == COLOR_PANEL_BG && at(fb, 200, 600) == COLOR_PANEL_BG);
	assert(at(fb, tr_cam_img_x0(0) - 1, 120 + 200) == COLOR_PANEL_BG &&
	       at(fb, tr_cam_img_x0(0) + TR_CAM_SRC_W, 120 + 200) == COLOR_PANEL_BG);
	/* the lamps along the top letterbox, one 200-px cell each: LEFT ARM unlit,
	 * BOTH ARMS lit (air_ticks), DUCK unlit */
	assert(at(fb, 100, 8 + LAND_LAMP_SQ / 2) == COLOR_LAMP_OFF);
	assert(at(fb, 500, 8 + LAND_LAMP_SQ / 2) == COLOR_LAMP_ON);
	assert(at(fb, 700, 8 + LAND_LAMP_SQ / 2) == COLOR_LAMP_OFF);
	/* the live Hz line, in green, in the bottom letterbox's right half */
	{
		int green = 0;

		for (int vy = TR_VID_H - LAND_BAND_H; vy < TR_VID_H; vy++) {
			for (int x = TR_R3D_W / 2; x < TR_R3D_W; x++) {
				green += at(fb, x, vy) == COLOR_KP;
			}
		}
		assert(green > 100);
	}

	/* 6. a view the renderer must not trust: wrong size -> background, no read */
	tr_cam_view_write(
	    &host_cam_view_mem, TR_MEM_CAM_POOL, 44u, TR_CAM_SRC_W, 200u, 270u, 0u, pip_barrier);
	frame(&in, fb);
	assert(at(fb, TR_R3D_W / 2, TR_VID_H / 2) == COLOR_PANEL_BG);

	printf("a32 video: upright 1:1 at x 200 (90/270) and landscape at x 80 (0), strips, lamps, Hz, "
	       "skeleton, "
	       "band order free\n");
	return 0;
}
