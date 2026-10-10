/* probe/camera/src/main.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * DIAGNOSTIC, NOT PRODUCT CODE. E1M-AEN803 2026W36-0009 fails
 * alp_camera_open() with ALP_ERR_IO, reproducibly. The
 * src/backends/camera/zephyr_video.c (z_open(), ~line 200) calls
 * video_get_caps() (~line 228), video_set_format() (~line 250) and
 * video_enqueue() (~line 324); every failure from any of those three goes
 * through _errno_to_alp(), which maps any errno it does not special-case to
 * ALP_ERR_IO. alp_last_error() returns that same translated status. So
 * ALP_ERR_IO alone says nothing about which call failed or why.
 *
 * This probe deliberately bypasses <alp/*> and calls the Zephyr
 * drivers/video/ and drivers/i2c/ APIs directly, in the same order z_open()
 * does, printing each call's RAW return value -- the thing <alp/*> throws
 * away. Its only job is to say which step fails and with what errno; it is
 * not a camera example and does not use the portable API.
 *
 * Leading suspect: an I2C NACK during the OV9281 format write. This board's
 * I2C is also documented to sometimes FAIL OPEN (every address ACKs, every
 * register read returns a byte equal to its own address instead of real
 * data) -- a plausible-looking value alone is not proof of anything, so
 * Step 1 reads the sensor's fixed chip-ID registers as a ground-truth
 * control before anything else touches the bus, and Step 4 repeats it
 * after, to catch the format write breaking the bus outright.
 *
 * Sensor: ov9281@60 on &csi_i2c (E1M I2C1), from
 * zephyr/boards/shields/innomaker_cam_ov9281/innomaker_cam_ov9281.overlay.
 *
 * Console is the RAM buffer 'ram_console_buf' -- see prj.conf and README.md.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video.h>

/* ---- devicetree identity (Step 1) ------------------------------------- */

/* csi_i2c is a label alias for &i2c1 (E1M I2C1), set by
 * e1m_evk_rpi_csi.overlay -- see its boards/e1m_aen.dtsi header. */
#define I2C_BUS_NODE DT_NODELABEL(csi_i2c)
/* ov9281@60 on that bus, from innomaker_cam_ov9281.overlay. */
#define SENSOR_NODE DT_NODELABEL(ov9281)
#define SENSOR_ADDR DT_REG_ADDR(SENSOR_NODE)

/* The video device z_open() actually calls -- e1m_evk_rpi_csi.overlay's
 * `alp-camera0 = &csi_capture_port;` alias, the same DT_ALIAS(alp_camera0)
 * zephyr_video.c's _devs[0] resolves for camera_id 0. */
#define CAM_NODE DT_ALIAS(alp_camera0)

static const struct device *const i2c_bus = DEVICE_DT_GET(I2C_BUS_NODE);
static const struct device *const sensor  = DEVICE_DT_GET(SENSOR_NODE);
static const struct device *const cam     = DEVICE_DT_GET(CAM_NODE);

/* ---- OV9281 chip-ID control read (Step 2 + Step 4) -------------------- */

/* OmniVision OV9281 datasheet: 16-bit register address, fixed silicon
 * constants -- unlike a config register, these can never legitimately read
 * back as anything else, which is exactly what makes them a valid ground
 * truth against a fail-open bus. */
#define OV9281_REG_CHIP_ID_HI 0x300AU
#define OV9281_REG_CHIP_ID_LO 0x300BU
#define OV9281_CHIP_ID_HI     0x92U
#define OV9281_CHIP_ID_LO     0x81U

#define SCAN_LO 0x08U
#define SCAN_HI 0x77U /* 0x08..0x77 = 112 addresses -- the conventional 7-bit scan window. */

/* Errno name lookup -- printk's minimal libc backing on this build has no
 * strerror(), and the raw number alone is exactly what this probe exists to
 * stop discarding, so spell out the ones that matter. */
static const char *errno_name(int rc)
{
	switch (rc < 0 ? -rc : rc) {
	case 0:
		return "OK";
	case EIO:
		return "EIO";
	case ENXIO:
		return "ENXIO";
	case EAGAIN:
		return "EAGAIN";
	case ENOMEM:
		return "ENOMEM";
	case EACCES:
		return "EACCES";
	case EBUSY:
		return "EBUSY";
	case EINVAL:
		return "EINVAL";
	case ENOSYS:
		return "ENOSYS";
	case ENOBUFS:
		return "ENOBUFS";
	case ENOTSUP:
		return "ENOTSUP";
	case ENODEV:
		return "ENODEV";
	case ETIMEDOUT:
		return "ETIMEDOUT";
	default:
		return "?";
	}
}

/* Read one byte at a 16-bit big-endian register address (SCCB/CCI
 * convention -- see ov9281.c's OV9281_REG16 / VIDEO_REG_ADDR16_DATA16_BE).
 * Raw i2c_write_read(), not video_read_cci_reg(): the point is the errno
 * this call returns, unfiltered. */
static int ov9281_reg8_read(uint16_t reg, uint8_t *val)
{
	uint8_t addr_be[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFU) };

	return i2c_write_read(i2c_bus, SENSOR_ADDR, addr_be, sizeof(addr_be), val, 1U);
}

/* A fail-open bus echoes the register address it was asked for instead of
 * real silicon data -- if the byte we got back equals either half of the
 * 16-bit address we sent, that is the signature, independent of what value
 * a real chip would have returned. */
static bool looks_like_addr_echo(uint16_t reg, uint8_t val)
{
	return val == (uint8_t)(reg >> 8) || val == (uint8_t)(reg & 0xFFU);
}

static unsigned int scan_bus(void)
{
	unsigned int n = 0U;
	uint8_t      dummy;

	for (uint16_t addr = SCAN_LO; addr <= SCAN_HI; addr++) {
		if (i2c_read(i2c_bus, &dummy, 1U, addr) == 0) {
			n++;
		}
	}
	return n;
}

/* Ground-truth control: scan + fixed chip-ID read, verdict printed.
 * Returns true if the bus read back as HEALTHY, so callers can compare
 * start-of-run vs end-of-run without re-deriving the verdict string. */
static bool run_i2c_control(const char *when)
{
	unsigned int n_ack = scan_bus();

	printk("[camprobe] i2c control (%s): %u/112 addresses ACKed (0x%02x..0x%02x)%s\n",
	       when,
	       n_ack,
	       SCAN_LO,
	       SCAN_HI,
	       (n_ack >= 100U) ? " -- suspiciously high, most of a fail-open bus" : "");

	uint8_t hi = 0U, lo = 0U;
	int     rc_hi = ov9281_reg8_read(OV9281_REG_CHIP_ID_HI, &hi);
	int     rc_lo = (rc_hi == 0) ? ov9281_reg8_read(OV9281_REG_CHIP_ID_LO, &lo) : rc_hi;

	printk("[camprobe] i2c control (%s): reg 0x%04x -> rc=%d (%s) val=0x%02x\n",
	       when,
	       OV9281_REG_CHIP_ID_HI,
	       rc_hi,
	       errno_name(rc_hi),
	       hi);
	printk("[camprobe] i2c control (%s): reg 0x%04x -> rc=%d (%s) val=0x%02x\n",
	       when,
	       OV9281_REG_CHIP_ID_LO,
	       rc_lo,
	       errno_name(rc_lo),
	       lo);

	bool healthy = false;

	if (rc_hi != 0 || rc_lo != 0) {
		printk("[camprobe] i2c (%s): NACK\n", when);
	} else if (hi == OV9281_CHIP_ID_HI && lo == OV9281_CHIP_ID_LO) {
		printk("[camprobe] i2c (%s): HEALTHY (chip id 0x9281)\n", when);
		healthy = true;
	} else if (looks_like_addr_echo(OV9281_REG_CHIP_ID_HI, hi) ||
	           looks_like_addr_echo(OV9281_REG_CHIP_ID_LO, lo) || n_ack >= 100U) {
		printk("[camprobe] i2c (%s): FAIL-OPEN SUSPECTED\n", when);
	} else {
		printk("[camprobe] i2c (%s): UNEXPECTED (chip id 0x%02x%02x, neither the "
		       "real ID nor an address echo)\n",
		       when,
		       hi,
		       lo);
	}
	return healthy;
}

/* ---- video sequence (Step 3) ------------------------------------------ */

/* The exact request z_open() would build for the game's config: GREY8
 * 640x400, the sensor's native small mode -- see
 * aen-camera-firstlight/src/main.c's CAM_FORMAT/CAM_WIDTH/CAM_HEIGHT. */
#define REQ_WIDTH  640U
#define REQ_HEIGHT 400U
/* GREY8 is 1 B/px, no packing -- computed rather than queried so buffer
 * sizing does not depend on video_get_caps()/video_set_format() having
 * succeeded (see the "keep going after a failure" ordering below). */
#define REQ_FRAME_BYTES (REQ_WIDTH * REQ_HEIGHT)
#define REQ_VBUF_COUNT  2U /* the pool holds two, matching z_open()'s ARRAY_SIZE(st->vbufs). */

/* First-failure tracking: one shared recorder so the summary line names
 * whichever step failed first, however many steps run after it. */
static bool have_first_fail;
static char first_fail_step[40];
static int  first_fail_rc;
static bool first_fail_no_errno;

static void record_fail(const char *step, int rc, bool no_errno)
{
	if (!have_first_fail) {
		have_first_fail     = true;
		first_fail_rc       = rc;
		first_fail_no_errno = no_errno;
		strncpy(first_fail_step, step, sizeof(first_fail_step) - 1U);
		first_fail_step[sizeof(first_fail_step) - 1U] = '\0';
	}
}

/* Print + fail-track one raw int-returning call in a single place, so every
 * step line has the same shape and the tracking can't be forgotten at a
 * call site. */
static int report(const char *step, int rc)
{
	printk("[camprobe] %-24s -> rc=%d (%s)\n", step, rc, errno_name(rc));
	if (rc != 0) {
		record_fail(step, rc, false);
	}
	return rc;
}

int main(void)
{
	printk("\n=== camprobe: camera bring-up diagnostic, E1M-AEN803 2026W36-0009 ===\n");

	/* --- Step 1: identify from devicetree, readiness ------------------- */
	printk("[camprobe] sensor: %s @ 0x%02x on bus %s\n",
	       DT_NODE_FULL_NAME(SENSOR_NODE),
	       (unsigned int)SENSOR_ADDR,
	       i2c_bus->name);
	printk("[camprobe] device_is_ready(sensor) = %d\n", (int)device_is_ready(sensor));
	printk("[camprobe] device_is_ready(video)  = %d\n", (int)device_is_ready(cam));

	/* --- Step 2: I2C ground truth, before anything else touches the bus - */
	bool healthy_start = run_i2c_control("start");

	/* --- Step 3: the same calls z_open() makes, in the same order -------
	 * Every step below is attempted regardless of earlier failures where
	 * that is safe (each Zephyr call just returns an errno, it does not
	 * corrupt state on failure) -- a dependent step that cannot safely run
	 * (no buffers to start streaming, no stream to dequeue from) is
	 * explicitly SKIPPED with why, not silently omitted. */

	struct video_caps vcaps   = { .type = VIDEO_BUF_TYPE_OUTPUT };
	int               caps_rc = report("video_get_caps", video_get_caps(cam, &vcaps));

	if (caps_rc == 0) {
		unsigned int n = 0U;

		for (const struct video_format_cap *fc = vcaps.format_caps;
		     fc != NULL && fc->pixelformat != 0U && n < 16U;
		     fc++, n++) {
			printk("[camprobe]   cap: %s %ux%u..%ux%u\n",
			       VIDEO_FOURCC_TO_STR(fc->pixelformat),
			       fc->width_min,
			       fc->height_min,
			       fc->width_max,
			       fc->height_max);
		}
		if (n == 0U) {
			printk("[camprobe]   (no format_caps entries)\n");
		}
	} else {
		printk("[camprobe]   format_caps: SKIPPED (video_get_caps failed)\n");
	}

	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_GREY,
		.width       = REQ_WIDTH,
		.height      = REQ_HEIGHT,
		.pitch       = 0U,
	};
	(void)report("video_set_format", video_set_format(cam, &fmt));

	struct video_buffer *vbufs[REQ_VBUF_COUNT] = { NULL };
	unsigned int         n_enqueued            = 0U;

	for (unsigned int i = 0U; i < REQ_VBUF_COUNT; i++) {
		char label[32];

		snprintf(label, sizeof(label), "video_buffer_alloc[%u]", i);
		vbufs[i] = video_buffer_alloc(REQ_FRAME_BYTES, K_NO_WAIT);
		if (vbufs[i] == NULL) {
			printk("[camprobe] %-24s -> NULL (no errno available)\n", label);
			record_fail(label, 0, true);
			continue;
		}
		printk("[camprobe] %-24s -> %p\n", label, (void *)vbufs[i]);

		vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		snprintf(label, sizeof(label), "video_enqueue[%u]", i);
		if (report(label, video_enqueue(cam, vbufs[i])) == 0) {
			n_enqueued++;
		} else {
			video_buffer_release(vbufs[i]);
			vbufs[i] = NULL;
		}
	}

	bool streaming = false;

	if (n_enqueued == 0U) {
		printk("[camprobe] video_stream_start        -> SKIPPED (0 of %u buffers enqueued)\n",
		       REQ_VBUF_COUNT);
	} else {
		streaming =
		    report("video_stream_start", video_stream_start(cam, VIDEO_BUF_TYPE_OUTPUT)) == 0;
	}

	if (!streaming) {
		printk("[camprobe] video_dequeue             -> SKIPPED (stream not started)\n");
	} else {
		struct video_buffer *vb = NULL;
		int deq_rc              = report("video_dequeue", video_dequeue(cam, &vb, K_MSEC(2000)));

		if (deq_rc == 0 && vb != NULL) {
			printk("[camprobe]   frame: bytesused=%u first16=", vb->bytesused);
			uint32_t n = vb->bytesused < 16U ? vb->bytesused : 16U;
			for (uint32_t i = 0U; i < n; i++) {
				printk("%02x ", vb->buffer[i]);
			}
			printk("\n");
		}
	}

	/* --- Step 4: repeat the chip-ID read -------------------------------- */
	bool healthy_end = run_i2c_control("end");

	if (healthy_start && !healthy_end) {
		printk("[camprobe] FINDING: bus was HEALTHY at start and is NOT healthy at end "
		       "-- something between the two control reads (the format write is the "
		       "leading suspect) broke the bus.\n");
	} else if (healthy_start && healthy_end) {
		printk("[camprobe] bus health unchanged: HEALTHY before and after.\n");
	} else {
		printk("[camprobe] bus was already unhealthy at start -- the run above did not "
		       "make it worse or better in a way this probe can attribute.\n");
	}

	/* --- Step 5: summary -------------------------------------------------- */
	if (have_first_fail) {
		if (first_fail_no_errno) {
			printk("RESULT: first failing step = %s, NULL return (no errno available)\n",
			       first_fail_step);
		} else {
			printk("RESULT: first failing step = %s, raw errno = %d (%s)\n",
			       first_fail_step,
			       first_fail_rc,
			       errno_name(first_fail_rc));
		}
	} else {
		printk("RESULT: no step returned a nonzero/NULL result\n");
	}

	return 0;
}
