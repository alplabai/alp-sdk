/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_sim CI selftest: loopback-connect to this app's OWN HTTP server
 * over 127.0.0.1 and check both endpoints:
 *   - GET /snapshot.jpg: response body is a real JPEG (starts FF D8, ends
 *     FF D9).
 *   - GET /stream: the multipart/x-mixed-replace FRAMING is well-formed
 *     -- the `--alpframe` boundary line, a Content-Type + Content-Length
 *     header pair, the blank line, and the trailing CRLF after the body
 *     -- not just that JPEG bytes arrive somewhere in the response.
 * Compiled in ONLY when CONFIG_NET_LOOPBACK is on (CMakeLists.txt gates
 * this file on `BOARD MATCHES "native_sim"`) -- a real board build never
 * links this thread, so it never spends a socket on a self-test in the
 * field.
 *
 * Every failure prints a distinct "SELFTEST-FAILED: <reason>" line before
 * the final ok/fail summary, so a red run is diagnosable straight from
 * the console log without attaching a debugger -- twister's own
 * console-harness `regex:` only matches the PASS string ("selftest ok"),
 * so a failure still surfaces as a timeout rather than an immediate
 * twister-level fail, but the explicit marker means the log tells you
 * why the moment you look at it instead of after reconstructing the
 * failure from a truncated snapshot.
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

#define SNAPSHOT_RESP_CAP 51200
#define STREAM_RESP_CAP   131072
#define CLIENT_TIMEOUT_S  5 /* a broken build must fail this test, not hang twister */

static uint8_t snapshot_resp[SNAPSHOT_RESP_CAP];
static uint8_t stream_resp[STREAM_RESP_CAP];

/* memmem() equivalent -- not every libc this SDK targets ships the GNU
 * extension. Returns the offset of the first match at/after `from`, or
 * -1. */
static ssize_t find_from(const uint8_t *buf, size_t len, size_t from, const char *needle)
{
	size_t nlen = strlen(needle);

	if (nlen == 0 || from + nlen > len) {
		return -1;
	}
	for (size_t i = from; i + nlen <= len; i++) {
		if (memcmp(buf + i, needle, nlen) == 0) {
			return (ssize_t)i;
		}
	}
	return -1;
}

static int connect_loopback(void)
{
	int                sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port   = htons(8080),
		.sin_addr   = INADDR_LOOPBACK_INIT, /* 127.0.0.1 */
	};
	struct zsock_timeval tv = { .tv_sec = CLIENT_TIMEOUT_S, .tv_usec = 0 };

	zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv));
	if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		zsock_close(sock);
		return -1;
	}
	return sock;
}

static bool check_snapshot(void)
{
	int sock = connect_loopback();

	if (sock < 0) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /snapshot.jpg connect\n");
		return false;
	}

	static const char req[] = "GET /snapshot.jpg HTTP/1.1\r\nHost: localhost\r\n\r\n";

	zsock_send(sock, req, sizeof(req) - 1, 0);

	size_t total = 0;

	for (;;) {
		ssize_t n = zsock_recv(sock, snapshot_resp + total, sizeof(snapshot_resp) - total, 0);

		if (n <= 0) {
			break; /* server closes after one response (Connection: close) */
		}
		total += (size_t)n;
	}
	zsock_close(sock);

	ssize_t sep = find_from(snapshot_resp, total, 0, "\r\n\r\n");

	if (sep < 0) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /snapshot.jpg no header/body "
		       "separator\n");
		return false;
	}

	const uint8_t *body     = snapshot_resp + sep + 4;
	size_t         body_len = total - (size_t)(sep + 4);
	bool           ok       = (body_len >= 4) && (body[0] == 0xFFu) && (body[1] == 0xD8u) &&
	                          (body[body_len - 2] == 0xFFu) && (body[body_len - 1] == 0xD9u);

	if (!ok) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /snapshot.jpg body is not a JPEG "
		       "(len=%u)\n",
		       (unsigned)body_len);
	}
	return ok;
}

static bool check_stream(void)
{
	int sock = connect_loopback();

	if (sock < 0) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /stream connect\n");
		return false;
	}

	static const char req[] = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";

	zsock_send(sock, req, sizeof(req) - 1, 0);

	/* Read until we've captured the START of a SECOND boundary line (the
	 * whole first part, framing included) or the buffer/timeout gives
	 * out -- /stream never closes on its own, so EOF is not how this
	 * loop ends in the success case. */
	size_t total     = 0;
	int    boundary2 = -1;

	for (;;) {
		ssize_t n = zsock_recv(sock, stream_resp + total, sizeof(stream_resp) - total, 0);

		if (n <= 0) {
			break;
		}
		total += (size_t)n;

		ssize_t b1 = find_from(stream_resp, total, 0, "--alpframe\r\n");

		if (b1 >= 0) {
			ssize_t b2 = find_from(stream_resp, total, (size_t)b1 + 1, "--alpframe\r\n");

			if (b2 >= 0) {
				boundary2 = (int)b2;
				break;
			}
		}
		if (total >= sizeof(stream_resp)) {
			break;
		}
	}
	zsock_close(sock);

	if (find_from(stream_resp, total, 0, "HTTP/1.1 200 OK") != 0) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /stream missing 200 OK\n");
		return false;
	}
	if (find_from(stream_resp, total, 0, "multipart/x-mixed-replace; boundary=alpframe") < 0) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /stream missing multipart "
		       "Content-Type\n");
		return false;
	}
	if (boundary2 < 0) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /stream never produced two "
		       "boundary lines (got %u bytes)\n",
		       (unsigned)total);
		return false;
	}

	ssize_t part1    = find_from(stream_resp, total, 0, "--alpframe\r\n");
	ssize_t ctype    = find_from(stream_resp, total, (size_t)part1, "Content-Type: image/jpeg\r\n");
	ssize_t clen     = find_from(stream_resp, total, (size_t)part1, "Content-Length: ");
	ssize_t hdrs_end = find_from(stream_resp, total, (size_t)part1, "\r\n\r\n");

	if (ctype < 0 || clen < 0 || hdrs_end < 0 || hdrs_end >= boundary2) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /stream part 1 missing "
		       "Content-Type/Content-Length/blank-line before the next boundary\n");
		return false;
	}

	const uint8_t *body_start = stream_resp + hdrs_end + 4;
	size_t body_len = (size_t)boundary2 - (size_t)(hdrs_end + 4) - 2; /* -2: trailing CRLF */
	bool   jpeg_ok  = (body_len >= 4) && (body_start[0] == 0xFFu) && (body_start[1] == 0xD8u) &&
	                  (body_start[body_len - 2] == 0xFFu) && (body_start[body_len - 1] == 0xD9u);

	if (!jpeg_ok) {
		printf("[camera-mjpeg-stream] SELFTEST-FAILED: /stream part 1 body is not a JPEG "
		       "(len=%u)\n",
		       (unsigned)body_len);
	}
	return jpeg_ok;
}

static void selftest_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bool ok = check_snapshot() && check_stream();

	printf("[camera-mjpeg-stream] selftest %s\n", ok ? "ok" : "fail");
}

K_THREAD_DEFINE(selftest_tid, 4096, selftest_main, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 1000);
