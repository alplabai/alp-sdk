/* tests/host/test_a32_video.c -- the A32 renderer's video area
 * (a32/renderer/render.c render_video_band()/render_video_overlay()) end to
 * end on the host: a real frame published through tr_cam_view_t exactly as
 * hp_vision does, its pixels at the SAME address range CAM_POOL has on
 * silicon (mmap'd there, so the renderer's own range check passes
 * unmodified):
 *   - rows [0, TR_VIEW_H) are never touched by the video pass;
 *   - every video-area row is written (no stale framebuffer shows through);
 *   - the landscape camera covers rows [0, PLATE_Y) of the 800 x 512 area, exactly
 *     the cam_pip.h resample; the bottom PLATE_H rows are the opaque plate with its
 *     edge line, the four lamps (BOTH ARMS lit from air_ticks, LEFT ARM unlit) and
 *     the live Hz label;
 *   - a skeleton keypoint lands on the output pixel the inverse grid names, and
 *     is not drawn onto the plate;
 *   - a turned camera (90 / 270) is not drawn: dark area, "ROT nn" label, no skeleton;
 *   - a 720-wide panel (in.fw) gets the centre 720 columns of the same picture and
 *     its plate laid out over them;
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
static uint16_t ref[TR_VID_H * TR_VID_W];

/* Pixel (x, vy) of the area, x in render columns, of a frame fw wide (pitch fw, crop from x0c). */
static uint16_t at_fw(const uint16_t *f, int x, int vy, int fw)
{
	return f[(TR_VID_Y0 + vy) * fw + x - (TR_R3D_W - fw) / 2];
}

static uint16_t at(const uint16_t *f, int x, int vy)
{
	return at_fw(f, x, vy, TR_R3D_W);
}

static void frame_run(const tr_frame_in_t *in, uint16_t *f)
{
	for (int i = 0; i < TR_R3D_W * TR_R3D_H; i++) {
		f[i] = 0xDEADu;
	}
	render_setup(in);
	render_video_panel(f);
}

static void frame(const tr_frame_in_t *in, uint16_t *f)
{
	frame_run(in, f);
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

	const int top = PLATE_Y; /* the plate's first area row */

	/* 1. nothing published: the area is the dark background and the plate, the game rows untouched */
	frame(&in, fb);
	for (int y = 0; y < TR_VIEW_H; y++) {
		for (int x = 0; x < TR_R3D_W; x++) {
			assert(fb[y * TR_R3D_W + x] == 0xDEADu);
		}
	}
	for (int vy = 0; vy < TR_VID_H; vy++) {
		for (int x = 0; x < TR_R3D_W; x++) {
			assert(at(fb, x, vy) != 0xDEADu);
			if (vy < top) {
				assert(at(fb, x, vy) == COLOR_PANEL_BG);
			}
		}
	}

	/* 2. published, landscape: the cover resample fills rows [0, PLATE_Y) of the whole width */
	host_hp_dbg_mem.magic       = TR_HP_DBG_MAGIC;
	host_hp_dbg_mem.loop_hz_x10 = 251u;
	tr_cam_view_write(
	    &host_cam_view_mem, TR_MEM_CAM_POOL, 42u, TR_CAM_SRC_W, TR_CAM_SRC_H, 0u, 1u, pip_barrier);
	frame(&in, fb);
	tr_cam_cover_rows(pool, 0, TR_VID_H, ref, TR_VID_W);
	for (int vy = 0; vy < top; vy++) {
		assert(memcmp(&fb[(TR_VID_Y0 + vy) * TR_R3D_W], &ref[vy * TR_VID_W], TR_VID_W * 2u) == 0);
	}
	/* and the picture really is the camera's (not the background) */
	assert(at(fb, 400, 100) != COLOR_PANEL_BG || at(fb, 401, 100) != COLOR_PANEL_BG);

	/* the plate: opaque dark, its edge line on top, nothing of the picture below it */
	for (int x = 0; x < TR_R3D_W; x++) {
		assert(at(fb, x, top) == COLOR_LAMP_OFF && at(fb, x, top + 1) == COLOR_LAMP_OFF);
		assert(at(fb, x, TR_VID_H - 1) != 0xDEADu);
	}
	assert(at(fb, 3, top + 4) == COLOR_PANEL_BG && at(fb, 797, TR_VID_H - 3) == COLOR_PANEL_BG);
	/* lamps: four cells of 150 px, the square 8 px under the edge line; BOTH ARMS lit, the rest not */
	assert(at(fb, 75, top + 8 + LAMP_SQ / 2) == COLOR_LAMP_OFF);  /* LEFT ARM */
	assert(at(fb, 225, top + 8 + LAMP_SQ / 2) == COLOR_LAMP_OFF); /* RIGHT ARM */
	assert(at(fb, 375, top + 8 + LAMP_SQ / 2) == COLOR_LAMP_ON);  /* BOTH ARMS */
	assert(at(fb, 525, top + 8 + LAMP_SQ / 2) == COLOR_LAMP_OFF); /* DUCK */
	assert(at(fb, 375 - LAMP_SQ / 2 - 1, top + 8 + LAMP_SQ / 2) ==
	       COLOR_PANEL_BG); /* square only */
	{
		/* the live Hz line, in green, in the label's last quarter, on its last line */
		int green = 0;

		for (int vy = top + 58; vy < top + 58 + 15; vy++) {
			for (int x = 600; x < TR_R3D_W; x++) {
				green += at(fb, x, vy) == COLOR_KP;
			}
		}
		assert(green > 60);
	}

	/* 3. the skeleton: a keypoint lands on the output pixel the inverse grid names */
	{
		tr_pose_t pose = { 0 };

		pose.kp[TR_KP_NOSE] = (tr_kp_t){ 320, 200, 255 }; /* -> (400, 256) */
		pose.kp[TR_KP_LANK] = (tr_kp_t){ 320, 380, 255 }; /* -> row 487, on the plate */
		pose.kp[TR_KP_RANK] = (tr_kp_t){ 3, 100, 255 };   /* -> x < 0: cropped away */
		tr_pslot_write(&host_pslot_mem, &pose, 0u, 0u, TR_HP_STATE_RUNNING, NULL, 1u, pip_barrier);
		frame(&in, fb);
		assert(at(fb, 400, 256) == COLOR_KP && at(fb, 401, 257) == COLOR_KP);
		assert(at(fb, 403, 256) != COLOR_KP); /* a 3x3 dot, not a smear */
		for (int x = 380; x < 420;
		     x++) { /* under the ankle (the Hz label is green too, further right) */
			for (int vy = top; vy < TR_VID_H; vy++) {
				assert(at(fb, x, vy) != COLOR_KP); /* the plate stays clean */
			}
		}
		for (int vy = 0; vy < top; vy++) {
			assert(at(fb, 0, vy) != COLOR_KP); /* the cropped keypoint is not drawn at the edge */
		}
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

	/* 5. a 720-wide panel (the RK055): the centre 720 columns of the same picture and skeleton,
	 * the plate re-laid over them (four lamps of 135 px from column 40, the label's quarter at
	 * 580..760), nothing written past the 720-pitch frame. */
	{
		static uint16_t fbn[TR_R3D_W * TR_R3D_H] __attribute__((aligned(16)));
		tr_frame_in_t   n = in;

		n.fw = 720;
		for (size_t i = 0; i < (size_t)TR_R3D_W * TR_R3D_H; i++) {
			fbn[i] = 0xDEADu;
		}
		render_setup(&n);
		render_video_panel(fbn);
		for (int vy = 0; vy < top; vy++) {
			for (int x = 40; x < 760; x++) {
				assert(at_fw(fbn, x, vy, 720) == at(fb, x, vy));
			}
		}
		for (size_t i = (size_t)720 * TR_R3D_H; i < (size_t)TR_R3D_W * TR_R3D_H; i++) {
			assert(fbn[i] == 0xDEADu);
		}
		assert(at_fw(fbn, 40 + 67, top + 8 + LAMP_SQ / 2, 720) == COLOR_LAMP_OFF);  /* LEFT ARM */
		assert(at_fw(fbn, 40 + 337, top + 8 + LAMP_SQ / 2, 720) == COLOR_LAMP_ON);  /* BOTH ARMS */
		assert(at_fw(fbn, 40 + 539, top + 8 + LAMP_SQ / 2, 720) == COLOR_PANEL_BG); /* label side */
		int green = 0;

		for (int vy = top + 58; vy < top + 58 + 15; vy++) {
			for (int x = 40 + 540; x < 760; x++) {
				green += at_fw(fbn, x, vy, 720) == COLOR_KP;
			}
		}
		assert(green > 60); /* the Hz label stayed inside the visible columns */
	}

	/* 6. a camera the HP turned is not drawn: dark area, no skeleton, and the label says why */
	tr_cam_view_write(
	    &host_cam_view_mem, TR_MEM_CAM_POOL, 43u, TR_CAM_SRC_W, TR_CAM_SRC_H, 90u, 0u, pip_barrier);
	frame(&in, fb);
	for (int vy = 0; vy < top; vy++) {
		for (int x = 0; x < TR_R3D_W; x++) {
			assert(at(fb, x, vy) == COLOR_PANEL_BG); /* no picture, and the nose is not a dot */
		}
	}
	assert(vid_dims[0] == 'R' && vid_dims[1] == 'O' && vid_dims[2] == 'T' && vid_dims[3] == ' ' &&
	       vid_dims[4] == '9' && vid_dims[5] == '0' && vid_dims[6] == '\0');

	/* 7. a view the renderer must not trust: wrong size -> background, no read */
	tr_cam_view_write(
	    &host_cam_view_mem, TR_MEM_CAM_POOL, 44u, TR_CAM_SRC_W, 200u, 0u, 0u, pip_barrier);
	frame(&in, fb);
	assert(at(fb, TR_R3D_W / 2, TR_VID_H / 4) == COLOR_PANEL_BG);

	printf("a32 video: the landscape camera covers rows 0..%d of the area, the plate, lamps, Hz, "
	       "skeleton, a turned camera, fw 720, band order free\n",
	       top - 1);
	return 0;
}
