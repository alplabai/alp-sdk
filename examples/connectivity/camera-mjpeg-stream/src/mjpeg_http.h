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

/**
 * @brief Start the MJPEG HTTP server thread.
 *
 * @param port  TCP port to listen on (this example uses 8080).
 * @return 0 on success, negative errno on socket/bind/listen failure.
 */
int mjpeg_http_server_start(uint16_t port);

/**
 * @brief Publish the latest encoded JPEG frame for the server to serve.
 *
 * Non-blocking (a short mutex hold, no I/O) -- safe to call from the
 * camera-capture loop.  Overwrites whatever frame was published before;
 * a client mid-read of the previous frame keeps reading its own copy
 * (see mjpeg_http.c), so this never blocks on a slow client.
 *
 * @param jpeg  Encoded JPEG bytes (SOI...EOI).
 * @param len   Length in bytes; frames larger than the server's internal
 *              buffer are dropped (silently -- see mjpeg_http.c).
 */
void mjpeg_http_publish_frame(const uint8_t *jpeg, size_t len);

#endif /* MJPEG_HTTP_H */
