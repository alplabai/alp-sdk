/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_sim CI selftest: loopback-connect to this app's OWN HTTP server
 * over 127.0.0.1 and fetch `GET /snapshot.jpg`, then check the response
 * body is a real JPEG (starts FF D8, ends FF D9).  Compiled in ONLY when
 * CONFIG_NET_LOOPBACK is on (CMakeLists.txt gates this file on
 * `BOARD MATCHES "native_sim"`) -- a real board build never links this
 * thread, so it never spends a socket on a self-test in the field.
 *
 * Uses the `zsock_*`-prefixed calls, same reason as mjpeg_http.c: the
 * plain POSIX names collide with the HOST's own libc socket() on
 * native_sim's host-executable build and silently fail every call.
 *
 * Starts itself via K_THREAD_DEFINE with a 1 s boot delay: long enough
 * for main.c's capture loop to publish at least one synthetic frame and
 * for mjpeg_http_server_start() to be listening, without main.c needing
 * to know this file exists.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#define RESP_CAP 51200

static uint8_t resp[RESP_CAP];

/* Find the header/body separator without relying on memmem() (not every
 * libc this SDK targets ships the GNU extension). */
static uint8_t *find_crlfcrlf(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i + 4 <= len; i++) {
		if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
			return buf + i;
		}
	}
	return NULL;
}

static void selftest_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int                sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port   = htons(8080),
		.sin_addr   = INADDR_LOOPBACK_INIT, /* 127.0.0.1 */
	};

	if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("[camera-mjpeg-stream] selftest fail: connect\n");
		return;
	}

	static const char req[] = "GET /snapshot.jpg HTTP/1.1\r\nHost: localhost\r\n\r\n";

	zsock_send(sock, req, sizeof(req) - 1, 0);

	size_t total = 0;

	for (;;) {
		ssize_t n = zsock_recv(sock, resp + total, sizeof(resp) - total, 0);

		if (n <= 0) {
			break; /* server closes after one response (Connection: close) */
		}
		total += (size_t)n;
	}
	zsock_close(sock);

	/* Body starts right after the header/body CRLFCRLF separator. */
	uint8_t *body = find_crlfcrlf(resp, total);
	bool     ok   = false;

	if (body != NULL) {
		body += 4;
		size_t body_len = total - (size_t)(body - resp);

		ok = (body_len >= 4) && (body[0] == 0xFFu) && (body[1] == 0xD8u) &&
		     (body[body_len - 2] == 0xFFu) && (body[body_len - 1] == 0xD9u);
	}

	printf("[camera-mjpeg-stream] selftest %s\n", ok ? "ok" : "fail");
}

K_THREAD_DEFINE(selftest_tid, 4096, selftest_main, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 1000);
