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
 */

#include <stdbool.h>
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
#include "mjpeg_http.h"

#define HTTP_PORT 8080

/* Resolution + frame rate, CONFIG_CAMERA_MJPEG_STREAM_1280X960-selected
 * (Kconfig, this directory) -- see issue #2286 Stage A.  Default: 640x480
 * @ 30 fps, the bench-proven path both the real camera and the synthetic
 * fallback request. mjpeg_http.h's MJPEG_HTTP_MAX_JPEG scales with this
 * same symbol; see boards/alp_e1m_aen80{1,3}_..._rtss_he.conf for how the
 * AEN video buffer pool is sized for 640x480, and
 * boards/overlay-1280x960-aen803.conf for the 1280x960 sizing.
 *
 * 1280x960 @ 15 fps is the OV5647's EXISTING centre-crop mode (no sensor
 * register-table change, ov5647.c) -- the field of view is the centre crop,
 * not a full-sensor 2x2-binned mode (that is Stage B, tracked separately).
 * 15 fps, not 30: two 1,843,200 B NV12 frames already consume most of the
 * AEN's 4 MiB SRAM0 bank alongside the JPEG output buffers (see
 * boards/overlay-1280x960-aen803.conf), leaving no SRAM0 budget for the
 * synthetic-frame fallback below -- CAMERA_MJPEG_STREAM_1280X960 compiles
 * that fallback path out entirely, not just moves it. */
#if defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
#define FRAME_W   1280
#define FRAME_H   960
#define FRAME_FPS 15
#else
#define FRAME_W   640
#define FRAME_H   480
#define FRAME_FPS 30
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
 * are paid for -- see boards/overlay-1280x960-aen803.conf. A real camera
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
                                 uint16_t               h)
{
	uint8_t *y     = base;
	size_t   y_len = (size_t)w * h;

	memset(req, 0, sizeof(*req));
	req->width     = w;
	req->height    = h;
	req->format    = fmt;
	req->subsample = ALP_JPEG_SUBSAMPLE_420;
	req->quality   = 80;
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
	jpeg_req_from_packed(req, fmt, synth_frame, FRAME_W, FRAME_H);
}
#endif

/* Print the DHCP lease + the stream/snapshot URLs once bound -- runs on
 * the net-mgmt callback's own context, never inside the capture loop, so
 * this never adds network latency to a capture iteration. */
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
				jpeg_req_from_packed(&req, pixfmt, frame.data, FRAME_W, FRAME_H);
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
			size_t       out_len = 0;
			alp_status_t erc     = alp_jpeg_encode(
			    jpeg, &req, mjpeg_http_claim_write_buffer(), JPEG_OUT_CAP, &out_len);

			if (erc == ALP_OK) {
				mjpeg_http_publish_frame(out_len);
			} else if (rate_limited(&encode_fail_last_log, &encode_fail_count)) {
				printf("[camera-mjpeg-stream] alp_jpeg_encode failed (rc=%d, "
				       "total=%u)\n",
				       (int)erc,
				       encode_fail_count);
			}
			if (camera != NULL) {
				alp_camera_release(camera, &frame);
			}
		}
	}
}
