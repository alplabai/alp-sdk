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
 * camera at all) it falls back to a software JPEG encoder and a
 * synthetic test-pattern frame, so the same binary always has something
 * to stream.
 *
 * Threading, and why it matters: THIS file's main() is the camera-capture
 * loop, and it must never block on the network -- a blocked capture loop
 * starves the ISP's auto-exposure convergence and has reset AE on real
 * silicon.  The HTTP server (mjpeg_http.c) runs in its own thread and
 * only ever touches the LATEST published frame; a slow client (or none at
 * all) never backs up into this loop.  DHCP runs concurrently too: it is
 * started once, up front, and negotiates in Zephyr's own net-stack
 * thread while the capture loop below is already running.
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

/* A modest resolution shared by both the real-camera path and the
 * synthetic fallback: big enough to be a real MJPEG demo, small enough
 * that the encoded frame comfortably fits JPEG_OUT_CAP below at quality
 * 80 with margin to spare (real photographic content compresses worse
 * than this file's flat test pattern -- see README.md's TBD-from-bench
 * note on the real bytes-per-frame figure). */
#define FRAME_W 320
#define FRAME_H 240
#define JPEG_OUT_CAP 49152u /* matches mjpeg_http.c's MAX_JPEG */

/*
 * The Hantro VC9000E JPEG encoder is an external AXI bus master: it DMAs
 * its source frame and writes its output stream over its OWN AXI master,
 * not through the CPU, so both must sit in memory that master can reach
 * -- the global on-chip SRAM0 bank (@0x02000000), never the M55's local
 * TCM (see alp/jpeg.h's alp_jpeg_encode() @note, and
 * src/backends/jpeg/alif_hantro.c's _is_dma_reachable()). CONFIG_ALP_SDK_
 * JPEG_ALIF_HANTRO only exists on the AEN board target (board-scoped
 * Kconfig, boards/alp_e1m_aen80{1,3}_...conf) -- everywhere else,
 * including native_sim, the software backend has no such placement
 * restriction and this attribute is a no-op.
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
 * chroma, so the exact sub-layout of that neutral fill never matters. */
static uint8_t synth_frame[FRAME_W * FRAME_H + (FRAME_W * FRAME_H) / 2] JPEG_DMA_MEM;
static uint8_t jpeg_out[JPEG_OUT_CAP] JPEG_DMA_MEM;

/* Build one alp_jpeg_encode_req_t against a single contiguous W*H*3/2
 * buffer, in whichever layout `fmt` names.  Used both for the synthetic
 * frame above and for a real camera frame (alp_camera_frame_t::data),
 * since camera/video backends pack a frame the same contiguous way. */
static void jpeg_req_from_packed(alp_jpeg_encode_req_t *req, alp_pixfmt_t fmt, void *base,
                                  uint16_t w, uint16_t h)
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

/* Print the DHCP lease + the stream/snapshot URLs once bound -- runs on
 * the net-mgmt callback's own context, never inside the capture loop, so
 * this never adds network latency to a capture iteration. */
static struct net_mgmt_event_callback dhcp_cb;

static void on_net_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
                          struct net_if *iface)
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

	alp_jpeg_capabilities(jpeg, &jcaps);
	alp_pixfmt_t pixfmt = (jcaps.pixfmt_mask & (1u << ALP_PIXFMT_NV12))
	                          ? ALP_PIXFMT_NV12
	                          : ALP_PIXFMT_YUV420_PLANAR;

	printf("[camera-mjpeg-stream] jpeg backend: hw_accelerated=%d pixfmt=%s\n",
	       (int)jcaps.hw_accelerated, (pixfmt == ALP_PIXFMT_NV12) ? "NV12" : "YUV420_PLANAR");

	/* Camera path is capability-gated, not chip- or SoC-gated (see
	 * deciding-the-portability-contract): alp_has() reads this SoC's
	 * generated capability table, so the same binary tries the real
	 * camera wherever one is wired up and falls straight through to
	 * the synthetic frame everywhere else -- including native_sim, and
	 * including an AEN board with no sensor actually attached (open()
	 * itself fails in that case, same fallback). */
	alp_camera_t *camera = NULL;

	if (alp_has(ALP_CAP_ID_HW_MIPI_CSI)) {
		alp_camera_config_t ccfg = ALP_CAMERA_CONFIG_DEFAULT(0);

		ccfg.width  = FRAME_W;
		ccfg.height = FRAME_H;
		ccfg.fps    = 15;
		ccfg.format = pixfmt;
		camera      = alp_camera_open(&ccfg);
		if (camera == NULL || alp_camera_start(camera) != ALP_OK) {
			printf("[camera-mjpeg-stream] no usable camera (err=%d); "
			       "serving a synthetic frame instead\n",
			       (int)alp_last_error());
			alp_camera_close(camera);
			camera = NULL;
		} else {
			printf("[camera-mjpeg-stream] camera open + streaming, %ux%u\n", FRAME_W,
			       FRAME_H);
		}
	} else {
		printf("[camera-mjpeg-stream] no MIPI-CSI on this SoC; serving a synthetic frame\n");
	}

	/* HTTP server thread -- serves whatever mjpeg_http_publish_frame()
	 * last handed it; nothing has been published yet, so an early
	 * client sees a zero-length frame until the first capture below
	 * completes. */
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

	/* The camera-capture loop.  Every iteration either captures a real
	 * frame (bounded timeout -- never wait forever on a wedged sensor)
	 * or rebuilds the synthetic one, encodes it, and publishes it; it
	 * never opens a socket, never blocks on mjpeg_http_publish_frame()
	 * (a short mutex hold, no I/O), and never waits on the network. */
	for (;;) {
		alp_jpeg_encode_req_t req;
		alp_camera_frame_t     frame;
		bool                    have_frame = false;

		if (camera != NULL) {
			if (alp_camera_capture(camera, &frame, 200) == ALP_OK) {
				jpeg_req_from_packed(&req, pixfmt, frame.data, FRAME_W, FRAME_H);
				have_frame = true;
			}
		} else {
			build_synthetic_frame(pixfmt, &req);
			have_frame = true;
			k_msleep(66); /* ~15 fps -- matches the requested camera fps above */
		}

		if (have_frame) {
			size_t out_len = 0;

			if (alp_jpeg_encode(jpeg, &req, jpeg_out, sizeof(jpeg_out), &out_len) ==
			    ALP_OK) {
				mjpeg_http_publish_frame(jpeg_out, out_len);
			}
			if (camera != NULL) {
				alp_camera_release(camera, &frame);
			}
		}
	}
}
