/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * MJPEG-over-HTTP server, plain Zephyr BSD sockets, one thread, one
 * client at a time -- a teaching example, not a production web server.
 *
 * Uses the `zsock_*`-prefixed socket calls (CONFIG_NET_SOCKETS) rather
 * than the plain POSIX names (`socket()`/`bind()`/...): on native_sim the
 * app links as an ordinary host executable against the host's own libc,
 * and depending on archive link order the plain names can silently
 * resolve to the HOST's socket() instead of Zephyr's -- every call then
 * fails immediately with EPROTONOSUPPORT because the host interprets
 * Zephyr's AF_INET(=1) as its own AF_UNIX. `zsock_*` has no libc
 * counterpart, so there is nothing for it to collide with; this is the
 * same API CONFIG_POSIX_API's plain names are themselves a thin wrapper
 * over (see zephyr/lib/posix/options/net.c).
 *
 * Frame hand-off: the camera-capture loop in main.c calls
 * mjpeg_http_publish_frame() every time it has a new encoded frame; this
 * file just overwrites `g_frame` under a mutex.  There is no queue, so a
 * slow client (or no client at all) never makes the publisher block or
 * back up -- it always just replaces the newest frame.  A client mid-read
 * of a `/stream` chunk works off its own `scratch` copy taken under the
 * same short lock, so a publish landing mid-send never tears a frame.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "mjpeg_http.h"

#define MAX_JPEG     49152 /* matches main.c's JPEG_OUT_CAP */
#define BOUNDARY     "alpframe"
#define STACK_SIZE   4096
#define REQUEST_LINE 128

static uint8_t g_frame[MAX_JPEG];
static size_t  g_frame_len;
static K_MUTEX_DEFINE(g_frame_lock);
static uint8_t scratch[MAX_JPEG]; /* server thread only -- one client at a time */

K_THREAD_STACK_DEFINE(server_stack, STACK_SIZE);
static struct k_thread server_thread;

void mjpeg_http_publish_frame(const uint8_t *jpeg, size_t len)
{
	if (len > sizeof(g_frame)) {
		return; /* drop -- oversized frame, never seen in practice at this quality/size */
	}
	k_mutex_lock(&g_frame_lock, K_FOREVER);
	memcpy(g_frame, jpeg, len);
	g_frame_len = len;
	k_mutex_unlock(&g_frame_lock);
}

/* Snapshot the latest published frame into `scratch`. Returns its length. */
static size_t take_frame(void)
{
	k_mutex_lock(&g_frame_lock, K_FOREVER);
	size_t len = g_frame_len;

	memcpy(scratch, g_frame, len);
	k_mutex_unlock(&g_frame_lock);
	return len;
}

static int send_all(int sock, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = zsock_send(sock, p, len, 0);

		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static void handle_snapshot(int sock)
{
	size_t len = take_frame();
	char   hdr[128];
	int    hlen = snprintf(hdr, sizeof(hdr),
	                     "HTTP/1.1 200 OK\r\n"
	                     "Content-Type: image/jpeg\r\n"
	                     "Content-Length: %u\r\n"
	                     "Connection: close\r\n\r\n",
	                     (unsigned)len);

	if (send_all(sock, hdr, (size_t)hlen) == 0) {
		send_all(sock, scratch, len);
	}
}

static void handle_stream(int sock)
{
	static const char preamble[] =
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n\r\n";

	if (send_all(sock, preamble, sizeof(preamble) - 1) != 0) {
		return;
	}
	/* One frame per iteration until the client goes away (send_all fails,
	 * e.g. it closed the connection) -- never a fixed frame count, so
	 * VLC/ffmpeg/a browser can watch indefinitely. */
	for (;;) {
		size_t len = take_frame();
		char   part[96];
		int    plen = snprintf(part, sizeof(part),
		                     "--" BOUNDARY "\r\n"
		                     "Content-Type: image/jpeg\r\n"
		                     "Content-Length: %u\r\n\r\n",
		                     (unsigned)len);

		if (send_all(sock, part, (size_t)plen) != 0 || send_all(sock, scratch, len) != 0 ||
		    send_all(sock, "\r\n", 2) != 0) {
			return;
		}
		k_msleep(50); /* pace to roughly the camera-loop cadence, not tighter */
	}
}

static int listen_sock = -1;

static void server_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int client = zsock_accept(listen_sock, NULL, NULL);

		if (client < 0) {
			continue;
		}

		char req[REQUEST_LINE] = { 0 };

		zsock_recv(client, req, sizeof(req) - 1, 0);
		/* "GET /snapshot.jpg" is 17 characters -- match its exact length, not
		 * 18: strncmp(..., 18) compares one byte past it (the literal's own
		 * NUL) against the real request's next byte (a space, before
		 * " HTTP/1.1"), which never matches and silently routes every
		 * snapshot request to neither branch below. */
		if (strncmp(req, "GET /stream", 11) == 0) {
			handle_stream(client);
		} else if (strncmp(req, "GET /snapshot.jpg", 17) == 0) {
			handle_snapshot(client);
		}
		zsock_close(client);
	}
}

int mjpeg_http_server_start(uint16_t port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port   = htons(port),
	};

	listen_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_sock < 0) {
		return -errno;
	}
	if (zsock_bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(listen_sock, 1) < 0) {
		int err = -errno;

		zsock_close(listen_sock);
		listen_sock = -1;
		return err;
	}

	k_thread_create(&server_thread, server_stack, STACK_SIZE, server_main, NULL, NULL, NULL,
	                K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&server_thread, "mjpeg_http");
	return 0;
}
