/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal HTTP server for `GET /stream` (multipart/x-mixed-replace MJPEG)
 * and `GET /snapshot.jpg` (one JPEG).  Runs in its own Zephyr thread so
 * the camera-capture/JPEG-encode loop in main.c never touches a socket --
 * see mjpeg_http.c's file header for the frame hand-off contract.
 */

#ifndef MJPEG_HTTP_H
#define MJPEG_HTTP_H

#include <stddef.h>
#include <stdint.h>

/** Capacity of each of the two ping-pong JPEG buffers this file owns --
 *  the one shared bound main.c's alp_jpeg_encode() call and this file's
 *  own allocation both use, so the two can never silently disagree.
 *  main.c passes this full capacity to alp_jpeg_encode() as-is -- the
 *  Hantro driver derives its own HW output-size-limit register internally
 *  (issue #2268).
 *
 *  128 KiB at 640x480: bench run 205 measured ~37 KB/frame on a DARK scene
 *  (a lit scene compresses worse).
 *
 *  160 KiB at 1280x960 (CONFIG_CAMERA_MJPEG_STREAM_1280X960, issue #2286
 *  Stage A): four times the pixel count scales worst-case JPEG size
 *  roughly with it, and 160 KiB keeps 2x this buffer inside the SRAM0
 *  budget boards/overlay-1280x960.conf works out alongside the
 *  (also larger) raw ISP buffer pool -- see that file for the full
 *  SRAM0 accounting. Bench run 243 (E1M-AEN803 2026W36-0001) measured
 *  131-135 KB JPEGs at quality 60 -- comfortably under this cap, 0
 *  buffer-full.
 *
 *  Bench run 312 (E1M-AEN803 2026W36-0001, IMX296, night room, #2287)
 *  DID overflow this cap repeatedly, once AE ran gain to its full 48 dB
 *  ceiling in a dim scene -- 160 KiB is still the right budget (SRAM0 is
 *  already at 98.5% for this resolution, see overlay-1280x960.conf; a
 *  larger buffer has nowhere to come from), so that run's fix is in two
 *  OTHER places instead of here: main.c's quality ladder now actually
 *  engages on a buffer-full encode (its retry gate keys off the error
 *  code alone now, jpeg_quality_should_retry(), not the encoder's
 *  reported size) and persists/recovers the reduced quality across
 *  frames, and hal_alif patch 0013 caps the AE library's own gain
 *  ceiling at 24 dB (the register's analog-only side of its gain-vs-
 *  code bend, p.56) instead of the full 48 dB -- plausibly because the
 *  digital side only multiplies read noise, though bench run 313 (0
 *  overflow at the new 24 dB cap, different scene) is consistent with
 *  that but not a controlled proof of it (see hal_alif patch 0013's own
 *  header comment). Run 313 also found isp_apply_ae() had been pushing
 *  the sensor's wider manual gain range over this calibration's ceiling
 *  every isp_stream_start() -- the library's own clamp held regardless
 *  (bench-confirmed), but the push is now corrected too
 *  (CONFIG_VIDEO_ISP_VSI_AE_AGAIN_MAX_DB_TENTHS, isp_pico.c). */
#if defined(CONFIG_CAMERA_MJPEG_STREAM_1280X960)
#define MJPEG_HTTP_MAX_JPEG 163840u
#else
#define MJPEG_HTTP_MAX_JPEG 131072u
#endif

/**
 * @brief Start the MJPEG HTTP server thread.
 *
 * @param port  TCP port to listen on (this example uses 8080).
 * @return 0 on success, negative errno on socket/bind/listen failure.
 */
int mjpeg_http_server_start(uint16_t port);

/**
 * @brief Claim the buffer to encode the NEXT frame into.
 *
 * Zero-copy hand-off: the camera-capture loop encodes directly into this
 * buffer (capacity @ref MJPEG_HTTP_MAX_JPEG bytes), then calls
 * @ref mjpeg_http_publish_frame with the encoded length -- no memcpy on
 * either side.  The returned pointer stays valid to write into until that
 * call; call this again for the frame after that.
 *
 * @return Pointer to a @ref MJPEG_HTTP_MAX_JPEG-byte write buffer, backed
 *         by SRAM0 on the AEN hardware-JPEG path (DMA-reachable by the
 *         Hantro encoder).
 */
uint8_t *mjpeg_http_claim_write_buffer(void);

/** Diagnostics mjpeg_http.c itself owns -- read once a second by main.c's
 *  stats line (issue #2286 bench run 242); main.c never touches a socket
 *  itself, so send timing has to come from here. */
typedef struct {
	/** Wall time of the most recent JPEG-body send (handle_stream's part
	 *  body or handle_snapshot's whole body), milliseconds. 0 before the
	 *  first send. */
	uint32_t send_ms;
} mjpeg_http_stats_t;

/**
 * @brief Read the latest send-timing diagnostics.
 *
 * @param[out] out  Must be non-NULL.
 */
void mjpeg_http_get_stats(mjpeg_http_stats_t *out);

/**
 * @brief Publish the frame just encoded into the buffer
 *        @ref mjpeg_http_claim_write_buffer last returned.
 *
 * Non-blocking (a short mutex hold, no I/O, no copy) -- safe to call from
 * the camera-capture loop.  If a client is mid-read of the previous frame,
 * this drops the new one instead of overwriting a buffer still in flight
 * (see mjpeg_http.c) -- the two buffers ping-pong once the client releases
 * it, so the capture loop never blocks on a slow client.
 *
 * @param len  Encoded length in bytes; must be <= @ref MJPEG_HTTP_MAX_JPEG.
 */
void mjpeg_http_publish_frame(size_t len);

#endif /* MJPEG_HTTP_H */
