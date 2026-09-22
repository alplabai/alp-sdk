/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-camera-firstlight -- first-light bench proof for the InnoMaker
 * CAM-OV9281 Raspberry-Pi-style MIPI CSI-2 camera module on the E1M-EVK's
 * J5 connector, on an E1M-AEN801/AEN803 SoM (Alif Ensemble E8, M55-HE).
 * Every camera call below goes through the PORTABLE <alp/camera.h> API --
 * open / start / capture-with-timeout / release / stop / close.  Nothing
 * here talks to Zephyr's drivers/video/ class directly.
 *
 * OV9281 is bench-verified (an E1M-AEN803 on the E1M-EVK, 2026-09-21): real
 * GREY8 frames land in memory in all three of the driver's modes.  This app
 * still prints enough detail on every path (including a failed open() or a
 * capture TIMEOUT) that a bench engineer can tell which stage broke if the
 * sensor isn't seated or the shield stack is wrong.
 *
 * Build (stack the sensor shield on top of the carrier connector shield,
 * `e1m_evk_rpi_csi`):
 *
 *   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
 *     examples/aen/aen-camera-firstlight -- \
 *     -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"         # OV9281, GREY8
 *
 * Which shield is stacked is a BUILD-TIME fact (the OV9281 driver's Kconfig
 * auto-selects, `default y` under its `DT_HAS_<compat>_ENABLED` -- see
 * zephyr/drivers/video/Kconfig.ov9281), so this app picks its capture
 * format the same way: a compile-time #if on that Kconfig symbol below, not
 * a runtime probe.
 *
 * See README.md for what each printed line means and the expected result.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

#if defined(CONFIG_VIDEO_OV9281)
/* InnoMaker CAM-OV9281: global-shutter mono, RAW8 mono only (GREY8 in the
 * portable enum).  640x400 is one of three modes the ported driver offers
 * (zephyr/drivers/video/ov9281.c: 640x400@100, 1280x720@50 and
 * 1280x800@100, all bench-verified) -- no crop, this IS the sensor's native
 * small mode. */
#define CAM_FORMAT          ALP_PIXFMT_GREY8
#define CAM_WIDTH           640
#define CAM_HEIGHT          400
#define CAM_BYTES_PER_PIXEL 1
#define CAM_SHIELD_NAME     "innomaker_cam_ov9281 (OV9281, GREY8 640x400)"
#else
#error \
    "aen-camera-firstlight needs the innomaker_cam_ov9281 shield stacked on e1m_evk_rpi_csi -- see README.md"
#endif

/* GREY8 is always 1 byte/pixel, no packing -- so a frame's row stride is
 * this caller-computable constant; no runtime pitch query needed, and none
 * exists in this API version (see alp_pixfmt_t's Doxygen in
 * <alp/peripheral.h> for why: the portable surface doesn't expose
 * negotiated format back to the caller yet). */
#define CAM_PITCH (CAM_WIDTH * CAM_BYTES_PER_PIXEL)

#define CAM_CAPTURE_TIMEOUT_MS 2000u

int main(void)
{
	printk("\n=== aen-camera-firstlight: %s ===\n", CAM_SHIELD_NAME);

	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0);
	cfg.width               = CAM_WIDTH;
	cfg.height              = CAM_HEIGHT;
	cfg.format              = CAM_FORMAT;

	/* --- 1. open -------------------------------------------------- */
	printk("[camfl] alp_camera_open(id=0, %ux%u) ...\n", cfg.width, cfg.height);
	alp_camera_t *cam = alp_camera_open(&cfg);
	if (cam == NULL) {
		alp_status_t err = alp_last_error();
		/* A failed open is a real, informative bench result on
		 * silicon this pipe has never run on -- print WHY, not just
		 * that it failed. */
		printk("[camfl] alp_camera_open FAILED: %s (%s)\n",
		       alp_status_name(err),
		       alp_status_description(err));
		if (err == ALP_ERR_NOT_READY) {
			/* zephyr_video.c's open() calls device_is_ready() on the
			 * cam device, which for a CSI-2 sensor node only goes
			 * ready once its driver's init hook reads back a
			 * matching chip-ID over I2C -- so NOT_READY here means
			 * the sensor never answered that probe. */
			printk("[camfl]   NOT_READY at open() means the sensor's chip-ID probe "
			       "never\n"
			       "[camfl]   answered on I2C1 during driver init -- check the module "
			       "is\n"
			       "[camfl]   seated on J5 and self-enabling (J5 pin 11 / CAM_EN must "
			       "stay\n"
			       "[camfl]   0, see docs/boards/e1m-evk.md's Camera section).\n");
		}
		printk("RESULT: camera open failed\n");
		return 0;
	}
	printk("[camfl] alp_camera_open OK\n");

	/* --- 2. start stream -------------------------------------------- */
	alp_status_t s = alp_camera_start(cam);
	printk("[camfl] alp_camera_start -> %s\n", alp_status_name(s));
	if (s != ALP_OK) {
		printk("RESULT: stream start failed\n");
		alp_camera_close(cam);
		return 0;
	}

	/* --- 3. wait for one frame, with a timeout ----------------------- */
	alp_camera_frame_t frame;
	printk("[camfl] alp_camera_capture: waiting up to %u ms for one frame ...\n",
	       CAM_CAPTURE_TIMEOUT_MS);
	s = alp_camera_capture(cam, &frame, CAM_CAPTURE_TIMEOUT_MS);
	if (s == ALP_ERR_TIMEOUT) {
		printk("[camfl] alp_camera_capture TIMED OUT -- no frame arrived in %u ms.\n",
		       CAM_CAPTURE_TIMEOUT_MS);
		/* A CSI receiver error/status readout would belong here
		 * (D-PHY stop-state, PHY/packet error counters), but
		 * zephyr/drivers/video/video_csi_dw.c exposes none of that
		 * through a public API -- its register offsets and struct
		 * layout (video_csi_dw.h) are translation-unit-private, not
		 * a header this example can include without reaching into
		 * the driver's internals.  Skipped rather than done via a
		 * driver-private header; the timeout above is the bench
		 * result on this path today. */
		printk("RESULT: capture timeout\n");
	} else if (s != ALP_OK) {
		printk("[camfl] alp_camera_capture FAILED: %s\n", alp_status_name(s));
		printk("RESULT: capture failed\n");
	} else {
		printk("[camfl] alp_camera_capture OK: %u bytes @ %llu us\n",
		       (unsigned)frame.size,
		       (unsigned long long)frame.timestamp_us);

		/* The only runtime proof that the CPI wrote exactly one unpacked frame and not a
		 * short/overrun one: CAM_PITCH * CAM_HEIGHT is the byte count this example expects
		 * for every sensor above (RAW10 unpacked to 16 bits/sample, or GREY8 at 1
		 * byte/sample -- CAM_PITCH already folds in CAM_BYTES_PER_PIXEL). A mismatch here is
		 * exactly the class of bug a wrong sensor width/height or a wrong RAW10 unpack would
		 * produce -- loud on purpose, since nothing downstream re-checks frame.size. */
		size_t expected_size = (size_t)CAM_PITCH * (size_t)CAM_HEIGHT;
		if (frame.size != expected_size) {
			printk("[camfl]   MISMATCH: frame.size %u != expected %u "
			       "(CAM_PITCH %u * CAM_HEIGHT %u) -- frame is short or overrun\n",
			       (unsigned)frame.size,
			       (unsigned)expected_size,
			       (unsigned)CAM_PITCH,
			       (unsigned)CAM_HEIGHT);
		}

		alp_status_t release_status;
		bool         frame_size_ok = frame.size == expected_size;
		if (!frame_size_ok) {
			/* Do not inspect frame.size bytes after a size mismatch: a broken
			 * driver may report more bytes than the pool allocation actually
			 * owns, and turning the diagnostic into an out-of-bounds read would
			 * hide the original capture defect. */
			goto release_frame;
		}

		const uint8_t *bytes = (const uint8_t *)frame.data;
		uint32_t       crc   = crc32_ieee(bytes, frame.size);
		printk("[camfl]   CRC32 = 0x%08x\n", crc);

		/* 16-bin histogram of the top 4 significant bits of each
		 * sample.  GREY8 is one byte per pixel, so its top 4 bits
		 * are bits 7..4 of the byte -- the histogram bins the
		 * darkest (0) to brightest (15) quarter of the sensor's
		 * dynamic range, a quick sanity check that the frame isn't
		 * all-zero or all-saturated garbage. */
		uint32_t hist[16] = { 0 };
		for (size_t i = 0; i < frame.size; ++i) {
			hist[(bytes[i] >> 4) & 0xFu]++;
		}
		printk("[camfl]   histogram (bin:count), darkest..brightest quarter:\n");
		for (int b = 0; b < 16; ++b) {
			printk("[camfl]     [%2d] %u\n", b, hist[b]);
		}

		/* First 16 bytes of row 0 / mid / last -- CAM_PITCH is the
		 * caller-computable constant documented above, no pitch
		 * query needed. */
		const size_t rows[3]      = { 0u, (size_t)CAM_HEIGHT / 2u, (size_t)CAM_HEIGHT - 1u };
		const char  *row_names[3] = { "row 0   ", "row mid ", "row last" };
		for (int r = 0; r < 3; ++r) {
			size_t off = rows[r] * (size_t)CAM_PITCH;
			printk("[camfl]   %s (offset %u): ", row_names[r], (unsigned)off);
			for (size_t c = 0; c < 16u && (off + c) < frame.size; ++c) {
				printk("%02x ", bytes[off + c]);
			}
			printk("\n");
		}

	release_frame:
		release_status = alp_camera_release(cam, &frame);
		printk("[camfl] alp_camera_release -> %s\n", alp_status_name(release_status));
		if (!frame_size_ok || release_status != ALP_OK) {
			printk("RESULT: capture failed\n");
		} else {
			printk("RESULT: capture ok\n");
		}
	}

	/* --- 4. stop + close ---------------------------------------------- */
	s = alp_camera_stop(cam);
	printk("[camfl] alp_camera_stop -> %s\n", alp_status_name(s));

	alp_camera_close(cam);
	printk("[camfl] alp_camera_close done\n");

	return 0;
}
