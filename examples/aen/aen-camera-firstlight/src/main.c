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
 * OV9281 is bench-verified (an E1M-AEN803 on the E1M-EVK, 2026-09-21): real
 * GREY8 frames land in memory in all three of the driver's modes.  The
 * OV5647 path is bench-verified too (runs 52/61/62, issue #2248).  The
 * IMX296 path (issue #2287) is Stage A bench-verified: I2C identity (bench
 * run 229), CSI-2 streaming, and a real 1456x1088 RAW10 frame captured
 * through this app (bench run 292, 0.98 correlation against a diag control
 * capture) -- see docs/camera-shields.md for the full write-up on all
 * three, and for what remains unverified on IMX296 (ISP-Pico, AE,
 * fast-trigger mode).
 * This app still prints enough detail on every path (including a failed
 * open() or a capture TIMEOUT) that a bench engineer can tell which stage
 * broke if the sensor isn't seated or the shield stack is wrong.
 *
 * Build one image per camera shield (stack the sensor shield on top of the
 * carrier connector shield, `e1m_evk_rpi_csi`):
 *
 *   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
 *     examples/aen/aen-camera-firstlight -- \
 *     -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1" # OV5647, RAW10
 *   ... -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"         # OV9281, GREY8
 *   ... -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" # IMX296, RAW10
 *
 * Which shield is stacked is a BUILD-TIME fact (exactly one sensor driver's
 * Kconfig auto-selects, `default y` under its `DT_HAS_<compat>_ENABLED` --
 * see zephyr/drivers/video/Kconfig.ov5647 / Kconfig.ov9281 / Kconfig.imx296),
 * so this app picks its capture format the same way: a compile-time #if
 * ladder on those same Kconfig symbols below, not a runtime probe.  A new
 * shield is one more #elif here plus one more testcase.yaml scenario --
 * nothing else in this file changes.
 *
 * See README.md for what each printed line means and the expected result
 * per module.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

#define CAM_CAPTURE_TIMEOUT_MS 2000u

#if defined(AEN_CAMERA_TRIGGER) && defined(CONFIG_VIDEO_IMX296)
/*
 * Opt-in bench mode (issue #2287, `-DAEN_CAMERA_TRIGGER=ON`, see
 * CMakeLists.txt + trigger_gpio.overlay): drives IMX296's fast
 * trigger mode and pulses the sensor module's own J3 Trig+ line (on the
 * INNO-MAKER module itself, not the E1M-EVK carrier) instead of free-run
 * capture. UNVERIFIED ON SILICON -- not benched by this change; issue #2287
 * benches it later. **Electrical polarity is UNVERIFIED too -- see
 * trigger_gpio.overlay's header comment. Do not wire J3 until that
 * module's input circuit is confirmed from its own documentation.**
 *
 * This is the one place in this file that steps outside the portable
 * <alp/camera.h> surface: neither <alp/camera.h> nor src/backends/camera/
 * has any concept of "trigger mode" today (checked before writing this --
 * see the driver-level comment in zephyr/drivers/video/imx296.c on
 * IMX296_CID_TRIGGER_MODE for why it rides Zephyr's driver-private-CID
 * convention instead of a new portable control). Reaching the sensor's
 * Zephyr device directly, by the same DT alias src/backends/camera/
 * zephyr_video.c itself resolves (`alp-camera0`), is the smallest way to
 * reach a control the portable API does not carry, without adding one to
 * <alp/camera.h> on the strength of a single sensor's need.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>

#define AEN_CAMERA_TRIGGER_ACTIVE 1

/* Mirrors zephyr/drivers/video/imx296.c's IMX296_CID_TRIGGER_MODE -- not a
 * public header (driver-private CID), so redefined here from the same
 * VIDEO_CID_PRIVATE_BASE convention; see that macro's comment in imx296.c. */
#define TRIGGER_CID           (VIDEO_CID_PRIVATE_BASE + 0x01)
#define TRIGGER_MODE_EXTERNAL 1

/* trigger_gpio.overlay puts the pin + its pinctrl state on the
 * special `/zephyr,user` node (see that overlay's header comment for why:
 * no binding/compatible needed for edtlib to type its phandle-array
 * properties, unlike any other devicetree path). */
#define TRIGGER_GPIO_NODE   DT_PATH(zephyr_user)
#define TRIGGER_PULSE_MS    5u
#define TRIGGER_FRAME_COUNT 3u

static const struct gpio_dt_spec trigger_gpio =
    GPIO_DT_SPEC_GET(TRIGGER_GPIO_NODE, imx296_trigger_gpios);

/* Defines the pin control config for TRIGGER_GPIO_NODE's `pinctrl-0`
 * (PIN_P5_1__GPIO, trigger_gpio.overlay) -- `/zephyr,user` is not a
 * real device with an init hook to apply it automatically, so trigger_arm()
 * applies it explicitly below; see PINCTRL_DT_DEV_CONFIG_DECLARE's own doc
 * comment in <zephyr/drivers/pinctrl.h> for this being the intended use of
 * runtime pin control from an app rather than a device driver. */
PINCTRL_DT_DEFINE(TRIGGER_GPIO_NODE);

/* Sets fast-trigger mode on the sensor (must land before alp_camera_start(),
 * see imx296.c's IMX296_CID_TRIGGER_MODE comment), muxes P5_1 to GPIO
 * explicitly (not relying on its reset-default alt function), and readies
 * the XTRIG pulse line in its logical-inactive (idle) state -- see
 * trigger_gpio.overlay for why GPIO_ACTIVE_HIGH/_LOW there, not a
 * hardcoded level here, is the single polarity flip point. */
static int trigger_arm(void)
{
	const struct device *sensor = DEVICE_DT_GET(DT_ALIAS(alp_camera0));
	struct video_control ctrl   = { .id = TRIGGER_CID, .val = TRIGGER_MODE_EXTERNAL };
	int                  ret;

	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(TRIGGER_GPIO_NODE), PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		printk("[camfl]   trigger GPIO pinctrl apply failed: %d\n", ret);
		return ret;
	}

	if (!gpio_is_ready_dt(&trigger_gpio)) {
		printk("[camfl]   trigger GPIO not ready\n");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&trigger_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("[camfl]   trigger GPIO configure failed: %d\n", ret);
		return ret;
	}

	ret = video_set_ctrl(sensor, &ctrl);
	if (ret < 0) {
		printk("[camfl]   IMX296_CID_TRIGGER_MODE set failed: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * One XTRIG pulse: "Fast trigger mode ... starts exposure at fall of XTRIG
 * immediately" (IMX296 datasheet page 64) -- assert the line (logical
 * active, whatever GPIO_ACTIVE_HIGH/_LOW in the overlay makes that
 * physically) for TRIGGER_PULSE_MS, then deassert it back to idle. Returns
 * the k_uptime_get() timestamp the pulse was asserted at, scaled to us so
 * it prints and subtracts directly against frame.timestamp_us below --
 * NOTE both are only ms-RESOLUTION in practice: k_uptime_get() itself
 * returns whole milliseconds, and zephyr_video.c's frame.timestamp_us is
 * likewise a Zephyr video buffer's millisecond `timestamp` field times
 * 1000, not a genuine microsecond capture. A delta under ~1 ms is noise,
 * not evidence either way.
 */
static uint64_t trigger_pulse(void)
{
	uint64_t pulse_us = (uint64_t)k_uptime_get() * 1000u;

	gpio_pin_set_dt(&trigger_gpio, 1);
	k_msleep(TRIGGER_PULSE_MS);
	gpio_pin_set_dt(&trigger_gpio, 0);

	return pulse_us;
}

/*
 * Weak, best-effort sanity check on a captured frame's content -- NOT proof
 * of a real triggered image (see README.md: "capture ok" alone doesn't
 * prove triggering worked, only that alp_camera_capture() returned a
 * frame-shaped buffer before its timeout). Flags the two cheapest failure
 * signatures to rule out: every RAW10 sample pinned at the 10-bit maximum
 * (0x3FF, a saturated/stuck-high bus), and every sample identical to the
 * first (stuck-at-some-other-value, or a frame that never moved at all).
 * Mirrors the histogram in the free-run path below, at pulse-loop scale.
 */
static const char *trigger_frame_content_check(const alp_camera_frame_t *frame)
{
	const uint8_t *bytes     = (const uint8_t *)frame->data;
	size_t         n_samples = frame->size / 2u;
	bool           all_3ff   = true;
	bool           all_first = true;
	uint16_t       first;

	if (n_samples == 0) {
		return "empty frame";
	}

	first = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);

	for (size_t i = 0; i < n_samples; i++) {
		uint16_t sample = (uint16_t)bytes[2u * i] | ((uint16_t)bytes[2u * i + 1u] << 8);

		all_3ff   = all_3ff && (sample == 0x3FFu);
		all_first = all_first && (sample == first);
		if (!all_3ff && !all_first) {
			return "varies (not proof of a real triggered image, but not stuck)";
		}
	}

	if (all_3ff) {
		return "SUSPECT: every sample is 0x3FF (saturated / stuck-high bus)";
	}

	return "SUSPECT: every sample is identical (stuck data, or nothing moved)";
}

/* Pulses XTRIG TRIGGER_FRAME_COUNT times, capturing, timestamping (pulse vs.
 * frame arrival) and content-checking one frame per pulse. Returns true iff
 * every frame arrived before its own CAM_CAPTURE_TIMEOUT_MS window. */
static bool trigger_capture_loop(alp_camera_t *cam)
{
	bool all_ok = true;

	for (unsigned i = 0; i < TRIGGER_FRAME_COUNT; i++) {
		alp_camera_frame_t frame;
		alp_status_t       s;
		uint64_t           pulse_us = trigger_pulse();

		s = alp_camera_capture(cam, &frame, CAM_CAPTURE_TIMEOUT_MS);
		if (s != ALP_OK) {
			printk("[camfl]   trigger frame %u: alp_camera_capture -> %s\n", i, alp_status_name(s));
			all_ok = false;
			continue;
		}

		printk("[camfl]   trigger frame %u: pulse @ %llu us, frame @ %llu us (delta %lld us), "
		       "%u bytes\n",
		       i,
		       (unsigned long long)pulse_us,
		       (unsigned long long)frame.timestamp_us,
		       (long long)(frame.timestamp_us - pulse_us),
		       (unsigned)frame.size);
		printk("[camfl]     content check: %s\n", trigger_frame_content_check(&frame));
		alp_camera_release(cam, &frame);
	}

	return all_ok;
}
#endif

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
#elif defined(CONFIG_VIDEO_OV5647)
/* OV5647: a Bayer RAW sensor advertising SBGGR8 and SBGGR10P at any
 * 4-pixel-aligned size up to its full 2592x1944 resolution.  RAW10, not
 * RAW8 (issue #2248, AUTHORIZED LOCAL DIVERGENCE #3): RAW8 (SBGGR8) is
 * unverified.  640x480 is the sensor's own real full-array
 * subsampled+binned mode -- see the vendored driver's file header and
 * docs/camera-shields.md -- not a crop. */
#define CAM_FORMAT          ALP_PIXFMT_RAW10
#define CAM_WIDTH           640
#define CAM_HEIGHT          480
#define CAM_BYTES_PER_PIXEL 2
#define CAM_SHIELD_NAME     "raspberry_pi_camera_module_1 (OV5647, RAW10 640x480)"
#elif defined(CONFIG_VIDEO_IMX296)
/* RPi Global Shutter Camera (IMX296LQR-C colour, issue #2287): one fixed all-pixel mode, so no
 * crop -- 1456x1088 RAW10 over a single CSI-2 lane. That is what the sensor transmits: the
 * datasheet's 1440x1080 "recording" area plus the 8-column/4-row colour-processing margin on
 * every side, which is sent, not cropped. Unpacked that is 1456 x 1088 x 2 = 3,168,256 bytes, so
 * this example's Kconfig drops the backend to ONE frame buffer and grows the SRAM0 pool to fit
 * it. Bench run 229 confirmed this sensor's I2C identity only (see docs/camera-shields.md) --
 * CSI-2 streaming through this app has not run on real silicon yet. */
#define CAM_FORMAT          ALP_PIXFMT_RAW10
#define CAM_WIDTH           1456
#define CAM_HEIGHT          1088
#define CAM_BYTES_PER_PIXEL 2
#define CAM_SHIELD_NAME     "raspberry_pi_global_shutter_camera (IMX296, RAW10 1456x1088)"
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

#if defined(AEN_CAMERA_TRIGGER_ACTIVE)
	printk("[camfl] AEN_CAMERA_TRIGGER: arming fast-trigger mode + XTRIG GPIO ...\n");
	if (trigger_arm() != 0) {
		printk("RESULT: trigger arm failed\n");
		alp_camera_close(cam);
		return 0;
	}
#endif

	/* --- 2. start stream -------------------------------------------- */
	alp_status_t s = alp_camera_start(cam);
	printk("[camfl] alp_camera_start -> %s\n", alp_status_name(s));
	if (s != ALP_OK) {
		printk("RESULT: stream start failed\n");
		alp_camera_close(cam);
		return 0;
	}

#if defined(AEN_CAMERA_TRIGGER_ACTIVE)
	/* --- 3. pulse XTRIG TRIGGER_FRAME_COUNT times, one capture each -- */
	printk("[camfl] AEN_CAMERA_TRIGGER: pulsing XTRIG for %u frames ...\n", TRIGGER_FRAME_COUNT);
	bool triggered_ok = trigger_capture_loop(cam);
	printk("RESULT: %s\n", triggered_ok ? "capture ok" : "capture failed");
#else
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
		/* frame.timestamp_us is ms-RESOLUTION only (zephyr_video.c scales the Zephyr
		 * video buffer's millisecond timestamp field by 1000), despite the "us" unit. */
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

	release_frame:
		release_status = alp_camera_release(cam, &frame);
		printk("[camfl] alp_camera_release -> %s\n", alp_status_name(release_status));
		if (!frame_size_ok || release_status != ALP_OK) {
			printk("RESULT: capture failed\n");
		} else {
			printk("RESULT: capture ok\n");
		}
	}

#endif /* !defined(AEN_CAMERA_TRIGGER_ACTIVE) */

	/* --- 4. stop + close ---------------------------------------------- */
	s = alp_camera_stop(cam);
	printk("[camfl] alp_camera_stop -> %s\n", alp_status_name(s));

	alp_camera_close(cam);
	printk("[camfl] alp_camera_close done\n");

	return 0;
}
