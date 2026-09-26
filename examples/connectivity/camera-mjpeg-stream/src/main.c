/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * camera-mjpeg-stream -- capture, JPEG-encode, and serve an MJPEG stream
 * from the board over a plain TCP socket.  Point a browser, VLC
 * (`vlc http://<ip>:8080/stream`), or ffmpeg at the printed URL; no host
 * tool ships with this example -- see README.md.
 *
 * Pipeline: <alp/camera.h> -> <alp/jpeg.h> -> mjpeg_http.c (multipart
 * HTTP over a Zephyr BSD socket, its own thread).  On the E1M-AEN family
 * this rides the VeriSilicon ISP-Pico + Hantro VC9000E JPEG hardware
 * encoder; on any other target (including native_sim, which has no
 * camera backend that links) it falls back to a software JPEG encoder
 * and a synthetic test-pattern frame, so the same binary always has
 * something to stream.
 *
 * Threading, and why it matters: THIS file's main() is the camera-capture
 * loop, and it must never block on the network -- a blocked capture loop
 * starves the ISP's auto-exposure convergence and has reset AE on real
 * silicon.  The HTTP server (mjpeg_http.c) runs in its own thread and
 * only ever touches the LATEST published frame; a slow client (or none at
 * all) never backs up into this loop.  It must also never SPIN on a
 * capture/encode error without yielding: main() runs at the default main
 * thread priority, numerically HIGHER priority than the HTTP server
 * thread (K_PRIO_PREEMPT(8)) -- an error loop with no sleep would starve
 * that thread outright, not just slow it down.  DHCP runs concurrently
 * too: it is started once, up front, and negotiates in Zephyr's own
 * net-stack thread while the capture loop below is already running.
 *
 * Format selection: this app opens the JPEG encoder FIRST and reads back
 * which source pixel-format layout it accepts (alp_jpeg_caps_t::
 * pixfmt_mask) before ever touching the camera -- see
 * examples/aen/aen-jpeg-regcheck for the same pattern in isolation.  That
 * makes the choice a RUNTIME fact about the backend that won the class,
 * not a compile-time #ifdef on which SoC this happens to be: on AEN, the
 * Hantro hardware encoder wins and only accepts NV12; everywhere else,
 * the portable software encoder wins and only accepts fully-planar
 * YUV420.  The camera is then opened in whichever format the encoder
 * asked for.
 *
 * Trigger mode (issue #2287, opt-in CONFIG_APP_CAMERA_TRIGGER, default n):
 * drives the camera through the portable alp_camera_set_trigger_mode() API
 * instead of leaving it free-run, and pulses a GPIO at CONFIG_APP_CAMERA_
 * TRIGGER_HZ to actually supply that timing -- see trigger_arm() /
 * trigger_timer_handler() below and README.md's "Trigger mode" section.
 * NOTHING in this build variant has been benched on real silicon with an
 * actual trigger pulse; see that README section for exactly what has and
 * hasn't been checked.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>

#include "alp/camera.h"
#include "alp/cap.h"
#include "alp/jpeg.h"
#include "jpeg_quality_ladder.h"
#include "mjpeg_http.h"

#if defined(CONFIG_APP_CAMERA_TRIGGER)
#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>

/*
 * The GPIO + pinctrl state this drives are declared on `/zephyr,user` in
 * both boards/alp_e1m_aen80{1,3}_..._rtss_he.overlay -- see either
 * overlay's "Opt-in trigger-mode demo wiring" block for the pin chain
 * (P5_1 / Arduino D4) and the still-unconfirmed J3 polarity caveat this
 * inherits from examples/aen/aen-camera-firstlight/trigger_gpio.overlay.
 *
 * CONFIG_APP_CAMERA_TRIGGER_PULSE_US matters beyond just "how long the pin
 * is high": on a sensor whose trigger input sets exposure by pulse WIDTH
 * (IMX296 fast-trigger mode -- see ALP_CAMERA_TRIGGER_EXTERNAL's own doc
 * comment in <alp/camera.h>), this Kconfig value IS the exposure time, not
 * a cosmetic timing detail.
 */
#define TRIGGER_GPIO_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec trigger_gpio =
    GPIO_DT_SPEC_GET(TRIGGER_GPIO_NODE, app_camera_trigger_gpios);

/* `/zephyr,user` has no init hook of its own -- trigger_arm() below applies
 * this pinctrl state explicitly, same reasoning as aen-camera-firstlight's
 * trigger_arm(). */
PINCTRL_DT_DEFINE(TRIGGER_GPIO_NODE);

static struct k_work           trigger_pulse_start_work;
static struct k_work_delayable trigger_pulse_stop_work;
static struct k_timer          trigger_timer;
static bool                    trigger_armed;

/* Second half of one pulse: runs CONFIG_APP_CAMERA_TRIGGER_PULSE_US after
 * trigger_pulse_start_work_handler() asserts the line. A k_work_delayable
 * callback, same non-ISR context as any other system-workqueue handler, but
 * scheduled instead of slept -- see this pair's header comment on why a
 * blocking k_msleep() used to sit here instead (issue #2287 dev review: it
 * blocked the shared system workqueue for the whole pulse width, delaying
 * every other work item queued behind it, including this trigger's own next
 * start at a high enough CONFIG_APP_CAMERA_TRIGGER_HZ). */
static void trigger_pulse_stop_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	gpio_pin_set_dt(&trigger_gpio, 0);
}

/* First half of one pulse: runs on the system workqueue, NOT in the
 * k_timer's own ISR context. Asserts the line, then schedules the release
 * above instead of sleeping here. */
static void trigger_pulse_start_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	gpio_pin_set_dt(&trigger_gpio, 1);
	k_work_schedule(&trigger_pulse_stop_work, K_USEC(CONFIG_APP_CAMERA_TRIGGER_PULSE_US));
}

/* k_timer expiry callback: ISR context, so this only ever submits work,
 * never touches the GPIO itself.  If a pulse is still in flight when the
 * next period elapses, k_work_submit() on an already-queued/running item
 * is a documented no-op -- a pulse can be dropped at a trigger rate faster
 * than CONFIG_APP_CAMERA_TRIGGER_PULSE_US can complete, never doubled up. */
static void trigger_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&trigger_pulse_start_work);
}

/* Configures the GPIO + pinctrl and starts the periodic pulse timer at
 * CONFIG_APP_CAMERA_TRIGGER_HZ. Caller must have already put the camera in
 * ALP_CAMERA_TRIGGER_EXTERNAL mode (alp_camera_set_trigger_mode(), before
 * alp_camera_start()) -- this function only ever drives the GPIO side. */
static int trigger_arm(void)
{
	int ret =
	    pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(TRIGGER_GPIO_NODE), PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	if (!gpio_is_ready_dt(&trigger_gpio)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&trigger_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	k_work_init(&trigger_pulse_start_work, trigger_pulse_start_work_handler);
	k_work_init_delayable(&trigger_pulse_stop_work, trigger_pulse_stop_work_handler);
	k_timer_init(&trigger_timer, trigger_timer_handler, NULL);

	k_timeout_t period = K_MSEC(1000u / CONFIG_APP_CAMERA_TRIGGER_HZ);
	k_timer_start(&trigger_timer, period, period);
	trigger_armed = true;
	return 0;
}

/* Reverse of trigger_arm(): stops the periodic timer, cancels/drains
 * whichever half of a pulse may still be in flight, and leaves the GPIO
 * inactive.  No-op if trigger_arm() was never called or already failed
 * (issue #2287 dev review: every camera stop/close path in main() below
 * calls this unconditionally, so it must be safe to call from a state where
 * nothing was ever armed). */
static void trigger_disarm(void)
{
	if (!trigger_armed) {
		return;
	}
	k_timer_stop(&trigger_timer);
	/* Waits out an in-flight start-work callback (which only ever runs
	 * briefly, no blocking calls of its own) before cancelling the
	 * delayable stop-work it may have just scheduled -- doing it in this
	 * order means a pulse that was mid-flight when this ran either
	 * completes fully (GPIO left inactive by trigger_pulse_stop_work_handler
	 * itself) or never started asserting the line, never leaves it
	 * stuck high. */
	struct k_work_sync sync;

	k_work_flush(&trigger_pulse_start_work, &sync);
	k_work_cancel_delayable(&trigger_pulse_stop_work);
	gpio_pin_set_dt(&trigger_gpio, 0);
	trigger_armed = false;
}
#endif /* CONFIG_APP_CAMERA_TRIGGER */

#define HTTP_PORT 8080

/* Resolution + frame rate, CONFIG_CAMERA_MJPEG_STREAM_1280X960-selected
 * (Kconfig, this directory) -- see issue #2286.  Default: 640x480
 * @ 30 fps, the bench-proven path both the real camera and the synthetic
 * fallback request. mjpeg_http.h's MJPEG_HTTP_MAX_JPEG scales with this
 * same symbol; see boards/alp_e1m_aen80{1,3}_..._rtss_he.conf for how the
 * AEN video buffer pool is sized for 640x480, and
 * boards/overlay-1280x960.conf for the 1280x960 sizing.
 *
 * 1280x960 @ 15 fps is the OV5647's full-sensor 2x2-binned mode (issue
 * #2286 Stage B, ov5647.c's ov5647_set_mode_regs()) -- the field of view
 * is the WHOLE sensor array, not Stage A's narrower centre crop at this
 * same output size (retired: the binned mode delivers identical output
 * pixels from a wider FOV and a faster per-mode line time, so there is no
 * remaining reason to request the crop at exactly 1280x960). Still
 * requested at 15 fps here, not the sensor mode's now-reachable 30 fps:
 * two 1,843,200 B NV12 frames already consume most of the AEN's 4 MiB
 * SRAM0 bank alongside the JPEG output buffers (see
 * boards/overlay-1280x960.conf), leaving no SRAM0 budget for the
 * synthetic-frame fallback below -- CAMERA_MJPEG_STREAM_1280X960 compiles
 * that fallback path out entirely, not just moves it. Raising the request
 * to 30 fps would also need a fresh bench pass (encode time, send-path
 * bandwidth) not done yet. */
#if defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
#define FRAME_W   1280
#define FRAME_H   960
#define FRAME_FPS 15
/*
 * Starting quality for this resolution -- bench run 242 (issue #2286):
 * quality 80 (the 640x480 default below) blew MJPEG_HTTP_MAX_JPEG
 * (mjpeg_http.h, 160 KiB) on every frame once AE converged past the
 * first ~12 dark ones, and 160 KiB has no SRAM0 headroom left to grow
 * (98.5% bank usage, see boards/overlay-1280x960.conf). 60 keeps
 * a typical frame well under the cap; a frame that still overflows at
 * 60 retries once at JPEG_QUALITY_FLOOR (jpeg_quality_ladder.h) rather
 * than being dropped outright.
 */
#define JPEG_QUALITY_DEFAULT 60u
#else
#define FRAME_W              640
#define FRAME_H              480
#define FRAME_FPS            30
/* Unchanged from before #2286 -- bench run 220 measured this comfortably
 * under the 640x480 128 KiB cap (mjpeg_http.h) even in bright daylight. */
#define JPEG_QUALITY_DEFAULT 80u
#endif

/*
 * The full physical output buffer (mjpeg_http_claim_write_buffer(),
 * MJPEG_HTTP_MAX_JPEG bytes) is what this app offers alp_jpeg_encode() --
 * the Hantro VC9000E driver derives its own HW output-size-limit register
 * (JPEG_SWREG9) from the buffer capacity minus the JPEG header size
 * internally (issue #2268), so the caller doesn't need to reserve any
 * margin here itself.
 */
#define JPEG_OUT_CAP MJPEG_HTTP_MAX_JPEG

/*
 * The Hantro VC9000E JPEG encoder is an external AXI bus master: it DMAs
 * its source frame over its OWN AXI master, not through the CPU, so the
 * source buffer must sit in memory that master can reach -- the global
 * on-chip SRAM0 bank (@0x02000000), never the M55's local TCM (see
 * alp/jpeg.h's alp_jpeg_encode() @note, and src/backends/jpeg/
 * alif_hantro.c's _is_dma_reachable()). mjpeg_http.c carries the matching
 * attribute for its own (output) buffers. CONFIG_ALP_SDK_JPEG_ALIF_HANTRO
 * only exists on the AEN board target (board-scoped Kconfig) --
 * everywhere else, including native_sim, the software backend has no
 * such placement restriction and both the section attribute and the
 * alignment below are inert.
 */
#if defined(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO)
#define JPEG_DMA_MEM __attribute__((section("SRAM0")))
#else
#define JPEG_DMA_MEM
#endif

/* Synthetic fallback frame: one buffer, big enough to be read as EITHER
 * NV12 (Y plane then interleaved UV) or fully-planar I420 (Y, then U,
 * then V) -- both are the same W*H*3/2 byte count, and this app only
 * ever fills it with a luma gradient plus NEUTRAL (grey, memset 128)
 * chroma, so the exact sub-layout of that neutral fill never matters.
 * __aligned(32): matches the Hantro AXI master's/D-cache maintenance's
 * cache-line granularity, same reasoning as mjpeg_http.c's buffers.
 *
 * DROPPED ENTIRELY at 1280x960 (issue #2286 Stage A): a second
 * JPEG_DMA_MEM buffer at this size (1,843,200 B) has no SRAM0 budget left
 * once the two real ISP buffers and the two ~160 KiB JPEG output buffers
 * are paid for -- see boards/overlay-1280x960.conf. A real camera
 * that stalls at this resolution therefore has no fallback frame to serve;
 * the capture loop just keeps retrying instead (see the main loop below). */
#if !defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
static uint8_t synth_frame[FRAME_W * FRAME_H + (FRAME_W * FRAME_H) / 2] JPEG_DMA_MEM __aligned(32);
#endif

/* Build one alp_jpeg_encode_req_t against a single contiguous W*H*3/2
 * buffer, in whichever layout `fmt` names.  Used both for the synthetic
 * frame above and for a real camera frame (alp_camera_frame_t::data),
 * since camera/video backends pack a frame the same contiguous way. */
static void jpeg_req_from_packed(alp_jpeg_encode_req_t *req,
                                 alp_pixfmt_t           fmt,
                                 void                  *base,
                                 uint16_t               w,
                                 uint16_t               h,
                                 uint8_t                quality)
{
	uint8_t *y     = base;
	size_t   y_len = (size_t)w * h;

	memset(req, 0, sizeof(*req));
	req->width     = w;
	req->height    = h;
	req->format    = fmt;
	req->subsample = ALP_JPEG_SUBSAMPLE_420;
	req->quality   = quality;
	req->y_plane   = y;
	req->y_stride  = w;
	if (fmt == ALP_PIXFMT_NV12) {
		req->u_plane = NULL; /* UV lives inside y_plane, interleaved. */
		req->v_plane = NULL;
	} else { /* ALP_PIXFMT_YUV420_PLANAR, I420 packing: Y, then U, then V. */
		req->u_plane  = y + y_len;
		req->u_stride = w / 2;
		req->v_plane  = (uint8_t *)req->u_plane + y_len / 4;
		req->v_stride = w / 2;
	}
}

#if !defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
static void build_synthetic_frame(alp_pixfmt_t fmt, alp_jpeg_encode_req_t *req)
{
	static uint8_t phase; /* advances each call so the stream visibly moves */

	for (int r = 0; r < FRAME_H; ++r) {
		for (int c = 0; c < FRAME_W; ++c) {
			synth_frame[r * FRAME_W + c] = (uint8_t)(r + c + phase);
		}
	}
	memset(synth_frame + (FRAME_W * FRAME_H), 128, (FRAME_W * FRAME_H) / 2);
	phase += 4;
	jpeg_req_from_packed(req, fmt, synth_frame, FRAME_W, FRAME_H, JPEG_QUALITY_DEFAULT);
}
#endif

/*
 * Print the DHCP lease + the stream/snapshot URLs once bound -- runs on
 * the net-mgmt callback's own context, never inside the capture loop, so
 * this never adds network latency to a capture iteration.
 *
 * Bench run 312 (#2287) never saw these lines: NOT a bug in this print
 * path, the event registration order (net_mgmt_add_event_callback() runs
 * BEFORE net_dhcpv4_start() below, so the callback can't miss the event),
 * or CONFIG_LOG_DEFAULT_LEVEL (this is a plain printf(), not a log call).
 * Prime suspect at the time: this line prints ONCE, near boot, into a
 * 16 KiB CONFIG_RAM_CONSOLE_BUFFER_SIZE circular buffer -- and that
 * run's JPEG-buffer-full failure (alif_hantro.c's driver-side LOG_ERR,
 * once per failed encode, unthrottled) fired ~14x/second for 40 seconds,
 * wrapping that ring buffer many times over before anyone read it back.
 *
 * Bench run 313 (same fixes applied, 0 encode failures this run) STILL
 * never saw these lines -- ruling that theory out as the WHOLE story:
 * something else is also capable of wrapping this same 16 KiB buffer.
 * The VSI AE/AWB library's own diagnostic prints (hal_alif's
 * isp_api_wrapper.c, routed through LOG_INF at CONFIG_VIDEO_LOG_LEVEL)
 * run continuously, every frame, for the life of the stream -- up to
 * ~13 KB/s -- which alone is enough to wrap this buffer inside two
 * seconds, with no JPEG failure needed at all. prj.conf now lowers
 * CONFIG_VIDEO_LOG_LEVEL to WRN for this app (see its own comment) to
 * stop that chatter. Re-bench needed to confirm the DHCP lines survive
 * with THIS fix in place -- not yet done. */
static struct net_mgmt_event_callback dhcp_cb;

static void
on_net_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	char                ip[NET_IPV4_ADDR_LEN] = { 0 };
	struct net_if_addr *ua                    = &iface->config.ip.ipv4->unicast[0].ipv4;

	net_addr_ntop(AF_INET, &ua->address.in_addr, ip, sizeof(ip));
	printf("[camera-mjpeg-stream] DHCP lease = %s\n", ip);
	printf("[camera-mjpeg-stream]   stream:   http://%s:%u/stream\n", ip, HTTP_PORT);
	printf("[camera-mjpeg-stream]   snapshot: http://%s:%u/snapshot.jpg\n", ip, HTTP_PORT);
}

/* Rate-limit a diagnostic to once per second so a fast error loop (e.g. a
 * wedged sensor returning immediate failures) can't flood the console --
 * `*count` still tracks the true total for the eventual printed line. */
static bool rate_limited(int64_t *last_log_ms, uint32_t *count)
{
	(*count)++;
	int64_t now = k_uptime_get();

	/* *count == 1 (the very first call) always prints, regardless of
	 * `now`: *last_log_ms starts at 0, and a failure inside the first
	 * second of boot (now < 1000) would otherwise satisfy
	 * `now - 0 < 1000` and silently suppress the FIRST failure ever
	 * seen -- exactly the one a reader most wants to know about. */
	if (*count != 1 && now - *last_log_ms < 1000) {
		return false;
	}
	*last_log_ms = now;
	return true;
}

/* Once-a-second diagnostic line (issue #2286 bench run 242): fps and
 * per-window JPEG-size/encode-time stats reset every print (so a
 * regression shows up in the NEXT line, not diluted into a lifetime
 * average); encoded/fail/retry stay cumulative for the whole run, same as
 * capture_fail_count/encode_fail_count already are above. send_ms comes
 * from mjpeg_http.c's own last-send timer (mjpeg_http_get_stats()) -- this
 * loop never touches a socket itself, see the file header. */
static void print_stats_if_due(int64_t  *last_print_ms,
                               uint32_t  total_encoded,
                               uint32_t  encode_fail_count,
                               uint32_t  encode_retry_count,
                               uint32_t *win_frames,
                               uint32_t *win_size_min,
                               uint32_t *win_size_max,
                               uint64_t *win_size_sum,
                               uint64_t *win_encode_ms_sum)
{
	int64_t now = k_uptime_get();

	if (now - *last_print_ms < 1000) {
		return;
	}

	mjpeg_http_stats_t hstats;

	mjpeg_http_get_stats(&hstats);

	uint32_t avg_size      = *win_frames ? (uint32_t)(*win_size_sum / *win_frames) : 0;
	uint32_t avg_encode_ms = *win_frames ? (uint32_t)(*win_encode_ms_sum / *win_frames) : 0;

	printf("[camera-mjpeg-stream] stats: fps=%u encoded=%u fail=%u retry=%u "
	       "jpeg_size_min=%u avg=%u max=%u B encode_ms=%u send_ms=%u\n",
	       *win_frames,
	       total_encoded,
	       encode_fail_count,
	       encode_retry_count,
	       *win_frames ? *win_size_min : 0,
	       avg_size,
	       *win_size_max,
	       avg_encode_ms,
	       hstats.send_ms);

	*last_print_ms     = now;
	*win_frames        = 0;
	*win_size_min      = UINT32_MAX;
	*win_size_max      = 0;
	*win_size_sum      = 0;
	*win_encode_ms_sum = 0;
}

/* After this many CONSECUTIVE capture failures, stop trying the camera
 * for the rest of the run and fall back to the synthetic frame -- a
 * sensor that has wedged is not coming back without a power-cycle this
 * app cannot perform, and retrying forever at CAPTURE_FAIL_BACKOFF_MS
 * just wastes the capture loop's time slice. */
#define CAPTURE_FAIL_LIMIT      10
#define CAPTURE_FAIL_BACKOFF_MS 50

int main(void)
{
	printf("[camera-mjpeg-stream] starting\n");

	/* Open the encoder FIRST -- its pixfmt_mask decides which source
	 * layout everything downstream (camera open + the synthetic
	 * fallback) builds. */
	alp_jpeg_config_t jcfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *jpeg = alp_jpeg_open(&jcfg);

	if (jpeg == NULL) {
		printf("[camera-mjpeg-stream] alp_jpeg_open failed (err=%d); nothing to serve\n",
		       (int)alp_last_error());
		return 0;
	}

	alp_jpeg_caps_t jcaps;

	if (alp_jpeg_capabilities(jpeg, &jcaps) != ALP_OK) {
		printf("[camera-mjpeg-stream] alp_jpeg_capabilities failed (err=%d); nothing to "
		       "serve\n",
		       (int)alp_last_error());
		alp_jpeg_close(jpeg);
		return 0;
	}
	alp_pixfmt_t pixfmt =
	    (jcaps.pixfmt_mask & (1u << ALP_PIXFMT_NV12)) ? ALP_PIXFMT_NV12 : ALP_PIXFMT_YUV420_PLANAR;

	printf("[camera-mjpeg-stream] jpeg backend: hw_accelerated=%d pixfmt=%s\n",
	       (int)jcaps.hw_accelerated,
	       (pixfmt == ALP_PIXFMT_NV12) ? "NV12" : "YUV420_PLANAR");

	/* Camera path is capability-gated, not chip- or SoC-gated (see
	 * deciding-the-portability-contract): alp_has() reads this SoC's
	 * generated capability table, which reflects the SoM board.yaml
	 * configures -- E1M-AEN803, which HAS a MIPI-CSI controller -- even
	 * when the actual build target is native_sim, so this branch is
	 * always taken there too.  The synthetic-frame fallback on
	 * native_sim therefore comes from alp_camera_open() failing (no
	 * real camera backend links there), NOT from alp_has() being
	 * false; alp_has() only steers the fallback on a SoC that
	 * genuinely has no MIPI-CSI controller at all. */
	alp_camera_t *camera = NULL;

	if (alp_has(ALP_CAP_ID_HW_MIPI_CSI)) {
		alp_camera_config_t ccfg = ALP_CAMERA_CONFIG_DEFAULT(0);

		ccfg.width  = FRAME_W;
		ccfg.height = FRAME_H;
		ccfg.fps    = FRAME_FPS;
		ccfg.format = pixfmt;
		camera      = alp_camera_open(&ccfg);

#if defined(CONFIG_APP_CAMERA_TRIGGER)
		/* Must land before alp_camera_start() -- alp_camera_set_trigger_mode()
		 * is only valid while the stream is stopped (<alp/camera.h>'s own
		 * doc comment). A sensor with no trigger control (anything but
		 * IMX296 today) answers ALP_ERR_NOSUPPORT here; this example just
		 * logs it and falls through to a plain free-run alp_camera_start()
		 * rather than treating it as fatal, so turning this Kconfig on for
		 * the wrong shield degrades to normal streaming instead of an
		 * outright failure to open. */
		if (camera != NULL) {
			alp_status_t trig_rc = alp_camera_set_trigger_mode(camera, ALP_CAMERA_TRIGGER_EXTERNAL);
			if (trig_rc != ALP_OK) {
				printf("[camera-mjpeg-stream] alp_camera_set_trigger_mode failed: %s "
				       "-- falling back to free-run\n",
				       alp_status_name(trig_rc));
			} else if (trigger_arm() != 0) {
				/* Sensor is latched into external-trigger mode with nothing
				 * now going to drive it -- every capture would time out
				 * forever, so undo the mode switch rather than leave the
				 * camera in a state this app can't service. Best-effort:
				 * nothing else to do if this also fails. */
				printf("[camera-mjpeg-stream] trigger GPIO arm failed -- reverting "
				       "camera to free-run\n");
				(void)alp_camera_set_trigger_mode(camera, ALP_CAMERA_TRIGGER_FREE_RUN);
			} else {
				printf("[camera-mjpeg-stream] external trigger armed at %u Hz\n",
				       CONFIG_APP_CAMERA_TRIGGER_HZ);
			}
		}
#endif

		if (camera == NULL || alp_camera_start(camera) != ALP_OK) {
#if defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
			printf("[camera-mjpeg-stream] alp_camera_open/start failed (err=%d); no "
			       "synthetic fallback at this resolution, will keep retrying\n",
			       (int)alp_last_error());
#else
			printf("[camera-mjpeg-stream] alp_camera_open/start failed (err=%d); "
			       "serving a synthetic frame instead\n",
			       (int)alp_last_error());
#endif
#if defined(CONFIG_APP_CAMERA_TRIGGER)
			trigger_disarm();
#endif
			alp_camera_close(camera);
			camera = NULL;
		} else {
			printf("[camera-mjpeg-stream] camera open + streaming, %ux%u\n", FRAME_W, FRAME_H);
		}
	} else {
#if defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
		printf("[camera-mjpeg-stream] no MIPI-CSI on this SoC; nothing to stream at this "
		       "resolution\n");
#else
		printf("[camera-mjpeg-stream] no MIPI-CSI on this SoC; serving a synthetic frame\n");
#endif
	}

	/* HTTP server thread -- serves whatever mjpeg_http_publish_frame()
	 * last handed it; nothing has been published yet, so an early
	 * client sees 503 Service Unavailable (handle_snapshot) or waits
	 * for the first frame (handle_stream) until the first capture below
	 * completes -- see mjpeg_http.c. */
	int http_rc = mjpeg_http_server_start(HTTP_PORT);

	if (http_rc != 0) {
		printf("[camera-mjpeg-stream] mjpeg_http_server_start failed (rc=%d)\n", http_rc);
	}

	/* DHCP concurrently: fire the request and return immediately --
	 * negotiation runs in Zephyr's own net-stack thread while the
	 * capture loop below starts running right away, unblocked by it. */
	struct net_if *iface = net_if_get_default();

	if (iface != NULL) {
		net_mgmt_init_event_callback(&dhcp_cb, on_net_event, NET_EVENT_IPV4_ADDR_ADD);
		net_mgmt_add_event_callback(&dhcp_cb);
		net_dhcpv4_start(iface);
	}

	uint32_t capture_fail_count    = 0;
	uint32_t capture_fail_consec   = 0;
	int64_t  capture_fail_last_log = 0;
	uint32_t encode_fail_count     = 0;
	int64_t  encode_fail_last_log  = 0;
	uint32_t encode_retry_count    = 0;

	/*
	 * Persists the quality ladder's current rung ACROSS frames, so a noisy
	 * scene that keeps overflowing JPEG_OUT_CAP stays down at a lower
	 * quality instead of re-trying JPEG_QUALITY_DEFAULT (and failing the
	 * same way) every single frame -- bench run 312 (#2287): once the
	 * out_len bug below was fixed, a per-frame-only ladder would have
	 * retried at the SAME two rungs forever without ever remembering the
	 * scene needed the lower one. Recovers back toward
	 * JPEG_QUALITY_DEFAULT one rung at a time after
	 * QUALITY_RECOVERY_STREAK consecutive clean encodes at the current
	 * rung, so a scene that brightens (or AE's new 24 dB gain ceiling,
	 * hal_alif patch 0013, keeps the noise down) climbs back up instead of
	 * staying parked at the floor forever.
	 */
	uint8_t  current_quality      = JPEG_QUALITY_DEFAULT;
	uint32_t quality_clean_streak = 0;
#define QUALITY_RECOVERY_STREAK 30u /* ~2s at 15fps, ~1s at 30fps -- a real recovery, not a blip */

	/* Once-a-second stats line (issue #2286 bench run 242): win_* reset
	 * every print, so fps/size/encode_ms report THIS window, not a
	 * lifetime average that would hide a regression under a long-running
	 * good stretch; encoded/fail/retry stay cumulative -- see the printf
	 * below. */
	int64_t  stats_last_print_ms = 0;
	uint32_t total_encoded       = 0;
	uint32_t win_frames          = 0;
	uint32_t win_size_min        = UINT32_MAX;
	uint32_t win_size_max        = 0;
	uint64_t win_size_sum        = 0;
	uint64_t win_encode_ms_sum   = 0;

	/* The camera-capture loop.  Every iteration either captures a real
	 * frame (bounded timeout -- never wait forever on a wedged sensor)
	 * or, at 640x480 only, rebuilds the synthetic one; either way the
	 * frame is encoded directly into the HTTP server's write buffer
	 * (mjpeg_http_claim_write_buffer() -- zero-copy, no memcpy on either
	 * side of the hand-off) and published; it never opens a socket,
	 * never blocks on mjpeg_http_publish_frame() (a short mutex hold, no
	 * I/O), and never waits on the network. A capture or encode failure
	 * is logged (rate-limited) and counted rather than silently skipped;
	 * a capture failure also backs off (k_msleep) so an error loop
	 * can't spin and starve the lower-priority HTTP thread. After
	 * CAPTURE_FAIL_LIMIT consecutive failures (a wedged sensor is not
	 * coming back on its own) the camera handle is closed and, at
	 * 640x480, the loop falls back to the synthetic frame; at 1280x960
	 * there is no fallback buffer (see synth_frame's comment above) --
	 * the loop just keeps skipping frames, `camera` stays NULL, and
	 * nothing new is published until an operator power-cycles the
	 * sensor. */
	for (;;) {
		alp_jpeg_encode_req_t req;
		alp_camera_frame_t    frame;
		bool                  have_frame = false;

		if (camera != NULL) {
			alp_status_t rc = alp_camera_capture(camera, &frame, 200);

			if (rc == ALP_OK) {
				capture_fail_consec = 0;
				jpeg_req_from_packed(&req, pixfmt, frame.data, FRAME_W, FRAME_H, current_quality);
				have_frame = true;
			} else {
				if (rate_limited(&capture_fail_last_log, &capture_fail_count)) {
					printf("[camera-mjpeg-stream] alp_camera_capture failed "
					       "(rc=%d, total=%u)\n",
					       (int)rc,
					       capture_fail_count);
				}
				capture_fail_consec++;
				k_msleep(CAPTURE_FAIL_BACKOFF_MS);
				if (capture_fail_consec >= CAPTURE_FAIL_LIMIT) {
#if defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
					printf("[camera-mjpeg-stream] camera stalled (%u "
					       "consecutive failures); no synthetic fallback at "
					       "this resolution, giving up on the camera\n",
					       capture_fail_consec);
#else
					printf("[camera-mjpeg-stream] camera stalled (%u "
					       "consecutive failures); falling back to the "
					       "synthetic frame\n",
					       capture_fail_consec);
#endif
#if defined(CONFIG_APP_CAMERA_TRIGGER)
					trigger_disarm();
#endif
					alp_camera_stop(camera);
					alp_camera_close(camera);
					camera = NULL;
				}
			}
		} else {
#if defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
			k_msleep(CAPTURE_FAIL_BACKOFF_MS); /* nothing to publish; don't spin */
#else
			build_synthetic_frame(pixfmt, &req);
			have_frame = true;
			k_msleep(1000 / FRAME_FPS); /* matches the requested camera fps above */
#endif
		}

		if (have_frame) {
			size_t       out_len   = 0;
			int64_t      encode_t0 = k_uptime_get();
			alp_status_t erc       = alp_jpeg_encode(
			    jpeg, &req, mjpeg_http_claim_write_buffer(), JPEG_OUT_CAP, &out_len);

			/*
			 * Bounded ladder, ONE retry per frame -- gated on the error
			 * code alone, NOT on out_len (bench run 312, #2287): out_len
			 * is the REQUIRED size only on the done->bytesused > out_cap
			 * path (src/backends/jpeg/alif_hantro.c) -- the actual
			 * failure this ladder exists for, JPEG_BUFFER_FULL
			 * (video_dequeue() returning -ENOSPC), used to leave out_len
			 * at its prior value (0 on a first attempt), so
			 * `out_len > JPEG_OUT_CAP` was always false and the ladder
			 * never engaged (retry=0 every run). alif_hantro.c's -ENOSPC
			 * path now also sets *out_len (to out_cap + 1, "too big,
			 * amount unknown") so that signal is reliable again -- but
			 * gating on the error code directly is simpler and covers
			 * BOTH NOMEM sub-causes: a pool-exhaustion NOMEM
			 * (video_import_buffer() -ENOBUFS) can't be fixed by a lower
			 * quality either, but retrying it anyway is harmless (bounded
			 * to this one extra attempt, same as the buffer-overrun case)
			 * and simpler than trying to keep telling the two apart from
			 * the caller's side. req.quality > JPEG_QUALITY_FLOOR guards
			 * against retrying with the exact quality that just failed,
			 * which jpeg_quality_step_down()'s floor would otherwise do.
			 */
			if (jpeg_quality_should_retry(erc == ALP_ERR_NOMEM, req.quality)) {
				encode_retry_count++;
				req.quality = jpeg_quality_step_down(req.quality);
				out_len     = 0;
				erc         = alp_jpeg_encode(
				    jpeg, &req, mjpeg_http_claim_write_buffer(), JPEG_OUT_CAP, &out_len);
			}

			uint32_t encode_ms = (uint32_t)(k_uptime_get() - encode_t0);

			if (erc == ALP_OK) {
				mjpeg_http_publish_frame(out_len);
				total_encoded++;
				win_frames++;
				win_size_min = (uint32_t)out_len < win_size_min ? (uint32_t)out_len : win_size_min;
				win_size_max = (uint32_t)out_len > win_size_max ? (uint32_t)out_len : win_size_max;
				win_size_sum += out_len;
				win_encode_ms_sum += encode_ms;

				/*
				 * Quality-ladder state, persisted across frames (see
				 * current_quality's own comment above). req.quality is
				 * whichever rung actually succeeded THIS frame (the
				 * original one, or the stepped-down retry) -- remember
				 * it so the NEXT frame starts there instead of re-
				 * failing at JPEG_QUALITY_DEFAULT again. Climb back up
				 * one rung after a real streak of clean encodes at the
				 * current rung, never on a single lucky frame.
				 */
				if (req.quality < current_quality) {
					current_quality      = req.quality;
					quality_clean_streak = 0;
				} else if (current_quality < JPEG_QUALITY_DEFAULT) {
					if (++quality_clean_streak >= QUALITY_RECOVERY_STREAK) {
						current_quality =
						    jpeg_quality_step_up(current_quality, JPEG_QUALITY_DEFAULT);
						quality_clean_streak = 0;
					}
				}
			} else {
				/*
				 * Both attempts failed -- reset the recovery streak (a
				 * failure breaks any run of clean encodes), but do NOT
				 * force current_quality down here (bench run 313, #2287
				 * reviewer fix): a pool-exhaustion NOMEM
				 * (video_import_buffer() -ENOBUFS, no free pool slot --
				 * see jpeg_quality_should_retry()'s own comment) is not
				 * fixed by a lower quality at all, and unconditionally
				 * dropping to the floor here would permanently lower
				 * quality for every LATER frame too, for a cause a lower
				 * quality can never address. current_quality is only
				 * ever lowered in the ALP_OK branch above, i.e. only
				 * when a lower-quality retry (or the current rung
				 * itself) actually SUCCEEDED -- never wedges either
				 * way, the next frame just tries again at whatever rung
				 * last succeeded. */
				quality_clean_streak = 0;
				if (rate_limited(&encode_fail_last_log, &encode_fail_count)) {
					printf("[camera-mjpeg-stream] alp_jpeg_encode failed (rc=%d, "
					       "total=%u, retries=%u)\n",
					       (int)erc,
					       encode_fail_count,
					       encode_retry_count);
				}
			}
			if (camera != NULL) {
				alp_camera_release(camera, &frame);
			}
		}

		print_stats_if_due(&stats_last_print_ms,
		                   total_encoded,
		                   encode_fail_count,
		                   encode_retry_count,
		                   &win_frames,
		                   &win_size_min,
		                   &win_size_max,
		                   &win_size_sum,
		                   &win_encode_ms_sum);
	}
}
