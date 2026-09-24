/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-isp-ov5647-viewfinder -- the portable <alp/camera.h> counterpart to
 * examples/aen/aen-isp-ov5647-capture: a real OV5647 sensor frame through
 * the Alif ISP-Pico (VeriSilicon ISP Nano), AE + AWB on, but through
 * alp_camera_open/start/capture/release/stop/close instead of the raw
 * Zephyr video_* API -- proving the portable backend itself
 * (src/backends/camera/alif_isp_pico.c) produces colour frames, not just
 * that the driver underneath it does.
 *
 * FORMAT: the portable <alp/camera.h> enum has no RGB output the ISP MI
 * can produce directly (isp_pico.c's supported_output_fmts is
 * YUV/mono/Bayer only), so this app's ALP_PIXFMT_RGB565 request makes
 * alif_isp_pico.c negotiate a YUV MI output (YUV420 planar preferred,
 * YUYV fallback) and convert every pixel to RGB565 on the CPU -- see
 * that backend's isp_open()/isp_capture().  This app just asks for
 * RGB565 and gets it back, the same as any other portable camera app;
 * it does not know or care which YUV layout the ISP actually produced.
 *
 * DEVICETREE: builds with the same two shields as the raw *-capture
 * example (-DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"), but
 * this app's own board overlay wires the ISP into the graph (cam port@2
 * -> isp, port@1 deleted) and overrides the `alp-camera0` alias from
 * e1m_evk_rpi_csi's default (&csi_capture_port, the RAW sensor-only path
 * aen-camera-firstlight's OV5647 scenario uses) to &isp -- see
 * the boards/ overlays for why that override is local to this example and
 * doesn't touch the raw path.
 *
 * LOOP: N_FRAMES captures, not a single shot -- exercises the same
 * fifo-keep-fed contract the backend documents: isp_pico.c auto-stops
 * once its incoming fifo empties, so alp_camera_capture() re-arms
 * video_stream_start() (tolerating -EBUSY) before every dequeue, and this
 * app releases each frame promptly after taking its stats so the backend
 * can keep buffers moving.  The last frame is kept in frame_copy (below)
 * for a bench `savebin`, the same convention aen-isp-ov5647-capture uses.
 *
 * TIMING: every frame prints `capture=<N> us` -- alp_camera_capture()'s
 * wall time, k_cycle_get_32()-measured. This is the bench observable for
 * the backend's CPU-side YUV->RGB565 conversion cost, which is the whole
 * reason a slow first cut of this backend couldn't keep AE converging
 * (isp_pico.c's fifo starved while a frame converted -- see
 * src/backends/camera/alif_isp_pico.c's "AE-convergence fix"): a duration
 * far past the ~100 ms frame period this app's own cfg.fps = 10 request
 * (below) targets says the conversion (or the dequeue wait behind it) is
 * still the bottleneck.
 *
 * FPS: this app pins cfg.fps = 10 explicitly (issue #2276 changed
 * ALP_CAMERA_CONFIG_DEFAULT's fps from 10 to 30, so this app must now ask
 * for 10 itself to keep the behaviour this file's comments above describe)
 * -- it has never been re-benched at 30 fps, and 10 fps buys two things
 * this app still needs at that rate: (1) CPU conversion headroom, since
 * the ~100 ms 10 fps frame period is what let the fix above keep two
 * buffers queued to the ISP MI while the third converts (a 30 fps ~33 ms
 * period gives the same conversion far less slack to land in before the
 * fifo starves again); and (2) AE exposure headroom in a dim scene (see
 * issue #2277: the OV5647 calibration AE block's exposure ceiling is
 * still pinned to the 10 fps VTS, so running faster than 10 fps narrows
 * how much exposure AE can command before it hits that ceiling).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/time_units.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

#define CAM_WIDTH              640
#define CAM_HEIGHT             480
#define FRAME_SIZE             ((size_t)CAM_WIDTH * CAM_HEIGHT * sizeof(uint16_t)) /* RGB565 */
#define N_FRAMES               30
#define CAM_CAPTURE_TIMEOUT_MS 2000u

static uint8_t frame_copy[FRAME_SIZE] __attribute__((section("SRAM0"), aligned(64)));

struct rgb565_stats {
	uint32_t mean;
	uint16_t min;
	uint16_t max;
};

static void rgb565_stats_compute(const uint16_t *px, size_t n, struct rgb565_stats *st)
{
	uint32_t sum = 0;
	uint16_t mn = 0xFFFFu, mx = 0u;

	for (size_t i = 0; i < n; i++) {
		uint16_t v = px[i];

		sum += v;
		mn = MIN(mn, v);
		mx = MAX(mx, v);
	}

	st->mean = n ? sum / n : 0;
	st->min  = mn;
	st->max  = mx;
}

int main(void)
{
	printk("\n=== aen-isp-ov5647-viewfinder (portable <alp/camera.h>, OV5647 through "
	       "the ISP, AE+AWB on) ===\n");

	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0);
	cfg.width               = CAM_WIDTH;
	cfg.height              = CAM_HEIGHT;
	cfg.format              = ALP_PIXFMT_RGB565;
	/* Pin 10 fps explicitly -- see the file header's FPS note.  This app's
	 * CPU YUV->RGB565 conversion and dim-scene AE headroom are only
	 * bench-proven at 10 fps (runs 147/154); ALP_CAMERA_CONFIG_DEFAULT's
	 * fps default moved to 30 with issue #2276, but this app was never
	 * re-benched there. */
	cfg.fps = 10;

	/* --- 1. open -------------------------------------------------- */
	printk("[ispvf] alp_camera_open(id=0, %ux%u, RGB565) ...\n", cfg.width, cfg.height);
	alp_camera_t *cam = alp_camera_open(&cfg);
	if (cam == NULL) {
		alp_status_t err = alp_last_error();

		printk("[ispvf] alp_camera_open FAILED: %s (%s)\n",
		       alp_status_name(err),
		       alp_status_description(err));
		printk("RESULT: camera open failed\n");
		return 0;
	}
	printk("[ispvf] alp_camera_open OK\n");

	/* --- 2. start stream -------------------------------------------- */
	alp_status_t s = alp_camera_start(cam);

	printk("[ispvf] alp_camera_start -> %s\n", alp_status_name(s));
	if (s != ALP_OK) {
		printk("RESULT: stream start failed\n");
		alp_camera_close(cam);
		return 0;
	}

	/* --- 3. capture loop ---------------------------------------------- */
	bool have_last_frame = false;

	for (int f = 1; f <= N_FRAMES; f++) {
		alp_camera_frame_t frame;

		/* Per-frame alp_camera_capture() wall time -- the bench observable
		 * for the backend's CPU-side YUV->RGB565 conversion cost (see
		 * src/backends/camera/alif_isp_pico.c's "AE-convergence fix").
		 * k_cycle_get_32() over sys_clock_hw_cycles_per_sec() rather than
		 * k_uptime_get() -- the call itself is the thing being timed, and
		 * a sub-millisecond fast-path conversion needs cycle, not tick,
		 * resolution. Includes the driver's video_dequeue() wait, not just
		 * the conversion -- see the printed value against the ~100 ms
		 * frame period (10 fps) to tell the two apart. */
		uint32_t cap_start_cyc = k_cycle_get_32();

		s = alp_camera_capture(cam, &frame, CAM_CAPTURE_TIMEOUT_MS);

		uint32_t cap_us = k_cyc_to_us_floor32(k_cycle_get_32() - cap_start_cyc);

		if (s != ALP_OK) {
			printk("[ispvf] f%d alp_camera_capture -> %s (%u us)\n", f, alp_status_name(s), cap_us);
			break;
		}
		printk("[ispvf] f%d capture=%u us\n", f, cap_us);

		if (f == 1 || f % 5 == 0 || f == N_FRAMES) {
			struct rgb565_stats st;

			rgb565_stats_compute((const uint16_t *)frame.data, frame.size / sizeof(uint16_t), &st);
			printk("[ispvf] f%d %u bytes @ %llu us, rgb565 min=0x%04x mean=0x%04x "
			       "max=0x%04x\n",
			       f,
			       (unsigned)frame.size,
			       (unsigned long long)frame.timestamp_us,
			       st.min,
			       st.mean,
			       st.max);
		}

		if (f == N_FRAMES) {
			memcpy(frame_copy, frame.data, MIN(frame.size, FRAME_SIZE));
			have_last_frame = true;
		}

		alp_status_t release_status = alp_camera_release(cam, &frame);

		if (release_status != ALP_OK) {
			printk("[ispvf] f%d alp_camera_release -> %s\n", f, alp_status_name(release_status));
			break;
		}
	}

	/* --- 4. stop + close ---------------------------------------------- */
	s = alp_camera_stop(cam);
	printk("[ispvf] alp_camera_stop -> %s\n", alp_status_name(s));

	alp_camera_close(cam);
	printk("[ispvf] alp_camera_close done\n");

	if (have_last_frame) {
		uint32_t crc = crc32_ieee(frame_copy, FRAME_SIZE);

		printk("snapshot(frame %d): addr=%p size=%u crc32=0x%08x\n",
		       N_FRAMES,
		       (void *)frame_copy,
		       (unsigned)FRAME_SIZE,
		       crc);
		printk("RESULT PASS: %d frame(s) captured (see per-frame stats above)\n", N_FRAMES);
	} else {
		printk("RESULT FAIL: frame %d never captured\n", N_FRAMES);
	}

	printk("Holding 20 s for a bench `savebin` of the snapshot buffer...\n");
	k_sleep(K_SECONDS(20));

	return 0;
}
