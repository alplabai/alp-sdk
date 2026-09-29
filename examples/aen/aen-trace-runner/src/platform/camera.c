/* src/platform/camera.c */
#include <stdbool.h>

#include <alp/camera.h>
#include <alp/peripheral.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "camera.h"

/*
 * The ONLY file in this project that talks to the camera API, exactly as
 * display.c is the only file that includes <alp/display.h>.  Everything
 * downstream works on a plain GREY8 byte buffer, so swapping sensors or SDK
 * versions touches exactly one file.
 *
 * Sensor: InnoMaker CAM-OV9281, global-shutter mono. 640x400 GREY8, 1 byte
 * per pixel, no packing -- row stride is exactly TR_CAMERA_WIDTH. Bench-
 * verified by examples/aen/aen-camera-firstlight (PR #2247); this mirrors
 * its capture pattern (open/start/capture-with-timeout/release).
 */
#define TR_CAMERA_WIDTH  640
#define TR_CAMERA_HEIGHT 400

static alp_camera_t       *g_cam;
static alp_status_t        g_first_err = ALP_OK;
static alp_camera_frame_t  g_frame;
static bool                g_frame_out;    /* a captured frame is out, awaiting release */
static bool                g_stall_warned; /* fix round 1, finding 7: latch the one-shot
                                             * "caller never released a frame" print */

int tr_camera_open(void)
{
	if (g_cam != NULL) {
		/* Idempotent rather than leaking the first handle (fix round 1,
		 * finding 7) -- a second open() with no matching close() would
		 * otherwise overwrite g_cam and orphan the first alp_camera_t. */
		return 0;
	}

	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0);

	cfg.width  = TR_CAMERA_WIDTH;
	cfg.height = TR_CAMERA_HEIGHT;
	cfg.format = ALP_PIXFMT_GREY8;

	if (!IS_ENABLED(CONFIG_VIDEO)) {
		/* Display-only build (TR_CAMERA=OFF): no camera is expected, so this
		 * is a configuration fact, not a failure -- the game runs attract/tilt. */
		printk("camera  : not built (display-only; -DTR_CAMERA=ON to enable)\n");
		return -1;
	}

	g_cam = alp_camera_open(&cfg);
	if (g_cam == NULL) {
		printk("RESULT FAIL: camera did not open -- %s\n", alp_status_name(alp_last_error()));
		return -1;
	}

	alp_status_t st = alp_camera_start(g_cam);
	if (st != ALP_OK) {
		printk("RESULT FAIL: camera stream start failed -- %s\n", alp_status_name(st));
		alp_camera_close(g_cam);
		g_cam = NULL;
		return -1;
	}

	printk("camera  : %dx%d GREY8\n", TR_CAMERA_WIDTH, TR_CAMERA_HEIGHT);
	return 0;
}

/*
 * Returns the OLDEST queued frame, not the newest: alp_camera_capture()
 * dequeues FIFO (zephyr_video.c's z_capture -> video_dequeue), so at
 * CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO_VBUF_COUNT=2 a consumer that calls this
 * no faster than the sensor captures always gets the frame behind the one
 * just finished -- one frame of latency (~33 ms at 30 fps), not the most
 * recent light that hit the sensor. Fix round 1, finding 7: this was
 * previously undocumented and the brief's "newest" prose was left standing
 * uncorrected. The dequeue behaviour itself is unchanged; only this comment
 * is new.
 */
const uint8_t *tr_camera_frame(size_t *len_out)
{
	if (g_cam == NULL) {
		return NULL;
	}
	if (g_frame_out) {
		/* g_frame_out means the caller hasn't released the last frame yet --
		 * refuse rather than hand out a second live pointer. One-shot print:
		 * a caller that forgets tr_camera_release() stalls the camera
		 * forever with otherwise zero console output, on a board whose only
		 * console is the RAM console read over SWD (fix round 1, finding 7). */
		if (!g_stall_warned) {
			g_stall_warned = true;
			printk("camera: tr_camera_frame() refused -- previous frame was never "
			       "released\n");
		}
		return NULL;
	}

	alp_camera_frame_t f;
	alp_status_t        st = alp_camera_capture(g_cam, &f, 0); /* 0 ms: never block */

	if (st == ALP_ERR_TIMEOUT) {
		return NULL; /* no frame ready yet -- the normal, common case */
	}
	if (st != ALP_OK) {
		/* Latch the first error only: a per-frame print would bury the boot
		 * log the same way display.c's blit errors would. */
		if (g_first_err == ALP_OK) {
			g_first_err = st;
			printk("camera: first capture error -- %s\n", alp_status_name(st));
		}
		return NULL;
	}

	/* The only runtime proof the CPI wrote exactly one unpacked frame and
	 * not a short/overrun one -- same check aen-camera-firstlight makes
	 * against CAM_PITCH * CAM_HEIGHT. */
	size_t expected = (size_t)TR_CAMERA_WIDTH * (size_t)TR_CAMERA_HEIGHT;

	if (f.size != expected) {
		if (g_first_err == ALP_OK) {
			g_first_err = ALP_ERR_IO;
			printk("camera: first frame size mismatch %u != %u -- short or overrun\n",
			       (unsigned)f.size, (unsigned)expected);
		}
		(void)alp_camera_release(g_cam, &f); /* do not hand a bad frame downstream */
		return NULL;
	}

	g_frame     = f;
	g_frame_out = true;
	if (len_out != NULL) {
		*len_out = f.size;
	}
	return (const uint8_t *)f.data;
}

void tr_camera_release(void)
{
	if (!g_frame_out) {
		return;
	}
	(void)alp_camera_release(g_cam, &g_frame);
	g_frame_out = false;
}

int16_t tr_camera_width(void)
{
	return TR_CAMERA_WIDTH;
}

int16_t tr_camera_height(void)
{
	return TR_CAMERA_HEIGHT;
}

void tr_camera_close(void)
{
	if (g_cam == NULL) {
		return;
	}
	if (g_frame_out) {
		/* Defensive: no call site releases a frame and then closes today,
		 * but a close() that could strand a held frame would be a second
		 * bug waiting on top of whatever called it out of sequence. */
		(void)alp_camera_release(g_cam, &g_frame);
		g_frame_out = false;
	}
	(void)alp_camera_stop(g_cam);
	alp_camera_close(g_cam);
	g_cam = NULL;
}
