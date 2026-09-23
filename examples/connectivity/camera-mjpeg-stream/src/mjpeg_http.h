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
 *  own allocation both use, so the two can never silently disagree. */
#define MJPEG_HTTP_MAX_JPEG 65536u

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
