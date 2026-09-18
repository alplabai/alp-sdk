/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-camera-firstlight -- first-light bench proof for Raspberry-Pi-style
 * MIPI CSI-2 camera modules on the E1M-EVK's J5 connector, on an
 * E1M-AEN801/AEN803 SoM (Alif Ensemble E8, M55-HE).  Every camera call
 * below goes through the PORTABLE <alp/camera.h> API -- open / start /
 * capture-with-timeout / release / stop / close -- exactly the same four
 * calls whichever sensor shield is stacked underneath.  Nothing here talks
 * to Zephyr's drivers/video/ class directly.
 *
 * The whole CSI-2 -> CPI pipe this app exercises has never run on real
 * silicon (see the branch this example ships on).  That makes a FAILED
 * open() or a capture TIMEOUT just as informative a bench result as a
 * clean frame -- this app prints enough detail on every path (including
 * failure) that a bench engineer can tell which stage broke.
 *
 * Build one image per camera shield (stack the sensor shield on top of the
 * carrier connector shield, `e1m_evk_rpi_csi`):
 *
 *   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
 *     examples/aen/aen-camera-firstlight -- \
 *     -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_2"   # IMX219, RAW10
 *   ... -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1" # OV5647, RAW10
 *   ... -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"         # OV9281, GREY8
 *   ... -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" # IMX296, RAW10
 *
 * Which shield is stacked is a BUILD-TIME fact (exactly one sensor driver's
 * Kconfig auto-selects, `default y` under its `DT_HAS_<compat>_ENABLED` --
 * see zephyr/drivers/video/Kconfig.ov5647 / Kconfig.ov9281 and upstream
 * Zephyr's Kconfig.imx219), so this app picks its capture format the same
 * way: a compile-time #if ladder on those same four Kconfig symbols below,
 * not a runtime probe.  A new shield is one more #elif here plus one more
 * testcase.yaml scenario -- nothing else in this file changes.
 *
 * See README.md for what each printed line means and the expected result
 * per module.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

#if defined(CONFIG_VIDEO_OV9281)
/* InnoMaker CAM-OV9281: global-shutter mono, RAW8 mono only (GREY8 in the
 * portable enum).  640x400 is one of exactly two modes the ported driver
 * offers (zephyr/drivers/video/ov9281.c) -- no crop, this IS the sensor's
 * native small mode. */
#define CAM_FORMAT          ALP_PIXFMT_GREY8
#define CAM_WIDTH           640
#define CAM_HEIGHT          400
#define CAM_BYTES_PER_PIXEL 1
#define CAM_SHIELD_NAME     "innomaker_cam_ov9281 (OV9281, GREY8 640x400)"
#elif defined(CONFIG_VIDEO_IMX219) || defined(CONFIG_VIDEO_OV5647)
/* IMX219 / OV5647: both are Bayer RAW sensors whose upstream driver
 * advertises SBGGR8 and SBGGR10P at any 4-pixel-aligned size up to their
 * full resolution.  RAW10, not RAW8: the CSI-2 pixel-clock ceiling this
 * board's D-PHY divider programs is 200 MHz (see docs/boards/e1m-evk.md),
 * and IMX219's fixed 456 MHz 2-lane link needs RAW10's 182.4 Mpixel/s --
 * RAW8 would need 228 Mpixel/s and video_set_format() refuses it with
 * -ERANGE by design.  640x480 is a small centred crop (both sensors'
 * min/max/step caps accept it), chosen to keep the frame small enough for
 * a quick bench capture, not a hardware limit. */
#define CAM_FORMAT          ALP_PIXFMT_RAW10
#define CAM_WIDTH           640
#define CAM_HEIGHT          480
#define CAM_BYTES_PER_PIXEL 2
#define CAM_SHIELD_NAME \
	(IS_ENABLED(CONFIG_VIDEO_IMX219) ? "raspberry_pi_camera_module_2 (IMX219, RAW10 640x480)" \
	                                 : "raspberry_pi_camera_module_1 (OV5647, RAW10 640x480)")
#elif defined(CONFIG_VIDEO_IMX296)
/* RPi Global Shutter Camera (IMX296LQR-C): one fixed all-pixel mode, so no
 * crop -- 1440x1080 RAW10 over a single CSI-2 lane.  Unpacked that is
 * 1440 x 1080 x 2 = 3,110,400 bytes, so this example's Kconfig drops the
 * backend to ONE frame buffer and grows the SRAM0 pool to fit it. */
#define CAM_FORMAT          ALP_PIXFMT_RAW10
#define CAM_WIDTH           1440
#define CAM_HEIGHT          1080
#define CAM_BYTES_PER_PIXEL 2
#define CAM_SHIELD_NAME     "raspberry_pi_global_shutter_camera (IMX296, RAW10 1440x1080)"
#else
#error "aen-camera-firstlight needs a camera shield stacked on e1m_evk_rpi_csi -- see README.md"
#endif

/* ALP_PIXFMT_RAW10 is always delivered UNPACKED (one 16-bit little-endian
 * sample per pixel, high 6 bits zero -- see alp_pixfmt_t's Doxygen in
 * <alp/peripheral.h>), so a frame's row stride is this caller-computable
 * constant for every sensor we request it from -- no runtime pitch query
 * needed, and none exists in this API version (see the same Doxygen for
 * why: the portable surface doesn't expose negotiated format back to the
 * caller yet). GREY8 is always 1 byte/pixel, no packing to begin with. */
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

		const uint8_t *bytes = (const uint8_t *)frame.data;
		uint32_t       crc   = crc32_ieee(bytes, frame.size);
		printk("[camfl]   CRC32 = 0x%08x\n", crc);

		/* 16-bin histogram of the top 4 significant bits of each
		 * sample.  RAW10 is unpacked 16-bit little-endian with the
		 * high 6 bits zero, so its 4 top SIGNIFICANT bits are bits
		 * 9..6 of the 10-bit sample; GREY8 is one byte per pixel, so
		 * its top 4 bits are bits 7..4 of the byte. Either way the
		 * histogram bins the darkest (0) to brightest (15) quarter
		 * of the sensor's dynamic range -- a quick sanity check that
		 * the frame isn't all-zero or all-saturated garbage. */
		uint32_t hist[16] = { 0 };
#if CAM_BYTES_PER_PIXEL == 2
		size_t n_samples = frame.size / 2u;
		for (size_t i = 0; i < n_samples; ++i) {
			uint16_t sample = (uint16_t)bytes[2u * i] | ((uint16_t)bytes[2u * i + 1u] << 8);
			hist[(sample >> 6) & 0xFu]++;
		}
#else
		for (size_t i = 0; i < frame.size; ++i) {
			hist[(bytes[i] >> 4) & 0xFu]++;
		}
#endif
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

		alp_camera_release(cam, &frame);
		printk("RESULT: capture ok\n");
	}

	/* --- 4. stop + close ---------------------------------------------- */
	s = alp_camera_stop(cam);
	printk("[camfl] alp_camera_stop -> %s\n", alp_status_name(s));

	alp_camera_close(cam);
	printk("[camfl] alp_camera_close done\n");

	return 0;
}
