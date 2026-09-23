/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * MJPEG-over-HTTP server, plain Zephyr BSD sockets, one thread, one
 * client at a time -- a teaching example, not a production web server
 * (see README.md's Limits section).
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
 * Frame hand-off is zero-copy, ping-pong between two SRAM0 buffers (see
 * mjpeg_http.h): the camera-capture loop in main.c encodes directly into
 * whichever buffer mjpeg_http_claim_write_buffer() hands it, then calls
 * mjpeg_http_publish_frame(). Publish makes that buffer `front` (the one
 * readers see) and flips the write target to the other one -- UNLESS a
 * reader currently holds `front` (mid-send), in which case publish drops
 * the new frame instead: touching `front` while a reader has a raw
 * pointer into it would tear the JPEG it's sending, and there is no third
 * buffer to swap into. The capture loop just keeps re-encoding into the
 * same `back` buffer until the reader releases it. Both SO_RCVTIMEO
 * (request line) and SO_SNDTIMEO (every send) bound how long a stalled or
 * malicious peer can hold that reference, which is what keeps this drop
 * window finite.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "mjpeg_http.h"

/*
 * The Hantro VC9000E JPEG encoder is an external AXI bus master: it DMAs
 * its source frame and writes its output stream over its OWN AXI master,
 * not through the CPU, so both must sit in memory that master can reach
 * -- the global on-chip SRAM0 bank (@0x02000000), never the M55's local
 * TCM (see alp/jpeg.h's alp_jpeg_encode() @note, and
 * src/backends/jpeg/alif_hantro.c's _is_dma_reachable()). Mirrors
 * main.c's own JPEG_DMA_MEM for its source (synthetic-frame) buffer --
 * duplicated rather than shared through a header because it is six lines
 * and each file already needs its own #if for a different buffer.
 * CONFIG_ALP_SDK_JPEG_ALIF_HANTRO only exists on the AEN board target
 * (board-scoped Kconfig) -- everywhere else, including native_sim, the
 * software backend has no such placement restriction and both the
 * section attribute and the alignment below are inert.
 */
#if defined(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO)
#define JPEG_DMA_MEM __attribute__((section("SRAM0")))
#else
#define JPEG_DMA_MEM
#endif

#define BOUNDARY     "alpframe"
#define STACK_SIZE   4096
#define REQUEST_LINE 128
/* SO_RCVTIMEO/SO_SNDTIMEO + wait_and_claim() -- bounds a wedged client AND a
 * stalled capture loop; the server thread must never wedge on either. */
#define IDLE_TIMEOUT_S 2

/* The two ping-pong buffers -- see the file header for the state machine.
 * __aligned(32): the Hantro AXI master and the CPU's D-cache maintenance
 * (sys_cache_data_flush_and_invd_range(), src/backends/jpeg/alif_hantro.c)
 * both work in cache-line units; an unaligned buffer risks a maintenance
 * op touching a neighbour's bytes. */
static uint8_t jpeg_buf[2][MJPEG_HTTP_MAX_JPEG] JPEG_DMA_MEM __aligned(32);
static size_t                                                jpeg_len[2];
static int      front = -1; /* buffer index holding the latest published frame, or none yet */
static int      back;       /* buffer index main.c's next claim_write_buffer() call returns */
static bool     reading;    /* a reader currently holds a raw pointer into jpeg_buf[front] */
static uint32_t seq;        /* bumps on every successful publish -- lets /stream wait for NEW */
static K_MUTEX_DEFINE(frame_lock);
static K_CONDVAR_DEFINE(frame_ready);

K_THREAD_STACK_DEFINE(server_stack, STACK_SIZE);
static struct k_thread server_thread;

uint8_t *mjpeg_http_claim_write_buffer(void)
{
	/* `back` only ever changes inside publish(), and only the capture
	 * loop calls claim/publish -- no lock needed for this read. */
	return jpeg_buf[back];
}

void mjpeg_http_publish_frame(size_t len)
{
	if (len > MJPEG_HTTP_MAX_JPEG) {
		return; /* oversized -- drop, never seen in practice at this quality/resolution */
	}
	k_mutex_lock(&frame_lock, K_FOREVER);
	if (reading) {
		/* A client is mid-send of `front`; `back` is the only buffer safe
		 * to touch right now, so this frame can't become the new front
		 * yet -- drop it. No tear: `front` is never written while a
		 * reader holds it, and the capture loop will just overwrite
		 * `back` again next iteration. */
		k_mutex_unlock(&frame_lock);
		return;
	}
	jpeg_len[back] = len;
	front          = back;
	back           = 1 - back;
	seq++;
	k_condvar_broadcast(&frame_ready);
	k_mutex_unlock(&frame_lock);
}

/* Non-blocking: claims `front` if a frame has ever been published. */
static bool try_claim(uint8_t **out_buf, size_t *out_len)
{
	k_mutex_lock(&frame_lock, K_FOREVER);
	if (front < 0) {
		k_mutex_unlock(&frame_lock);
		return false;
	}
	reading  = true;
	*out_buf = jpeg_buf[front];
	*out_len = jpeg_len[front];
	k_mutex_unlock(&frame_lock);
	return true;
}

/* Waits up to IDLE_TIMEOUT_S for a frame newer than `*last_seq`, then
 * claims it. Returns false on timeout (no new frame in time -- a stalled
 * capture loop must not wedge the server thread's /stream client
 * forever); the caller drops the client in that case. */
static bool wait_and_claim(uint32_t *last_seq, uint8_t **out_buf, size_t *out_len)
{
	k_mutex_lock(&frame_lock, K_FOREVER);
	while (front < 0 || seq == *last_seq) {
		if (k_condvar_wait(&frame_ready, &frame_lock, K_SECONDS(IDLE_TIMEOUT_S)) != 0) {
			k_mutex_unlock(&frame_lock);
			return false;
		}
	}
	reading   = true;
	*out_buf  = jpeg_buf[front];
	*out_len  = jpeg_len[front];
	*last_seq = seq;
	k_mutex_unlock(&frame_lock);
	return true;
}

static void release_claim(void)
{
	k_mutex_lock(&frame_lock, K_FOREVER);
	reading = false;
	k_mutex_unlock(&frame_lock);
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
	uint8_t *buf;
	size_t   len;

	if (!try_claim(&buf, &len)) {
		/* No frame published yet (server just started). */
		static const char resp[] = "HTTP/1.1 503 Service Unavailable\r\n"
		                           "Content-Length: 0\r\nConnection: close\r\n\r\n";
		send_all(sock, resp, sizeof(resp) - 1);
		return;
	}

	char hdr[128];
	int  hlen = snprintf(hdr,
	                     sizeof(hdr),
	                     "HTTP/1.1 200 OK\r\n"
	                     "Content-Type: image/jpeg\r\n"
	                     "Content-Length: %u\r\n"
	                     "Connection: close\r\n\r\n",
	                     (unsigned)len);

	if (send_all(sock, hdr, (size_t)hlen) == 0) {
		send_all(sock, buf, len);
	}
	release_claim();
}

static void handle_stream(int sock)
{
	static const char preamble[] =
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n\r\n";

	if (send_all(sock, preamble, sizeof(preamble) - 1) != 0) {
		return;
	}

	uint32_t last_seq = 0; /* 0 never equals a real seq (first publish makes it 1) */

	/* One part per NEWLY PUBLISHED frame -- wait_and_claim() blocks (up to
	 * IDLE_TIMEOUT_S) until seq advances, so a slow camera loop paces this
	 * loop too; it never resends a frame the client has already seen.
	 * Runs until the client goes away (send_all fails, e.g. SO_SNDTIMEO
	 * expires or it closed the connection) or the capture loop stalls for
	 * IDLE_TIMEOUT_S -- never a fixed frame count, so VLC/ffmpeg/a browser
	 * can watch indefinitely as long as frames keep arriving. */
	for (;;) {
		uint8_t *buf;
		size_t   len;

		if (!wait_and_claim(&last_seq, &buf, &len)) {
			return; /* no new frame in time -- drop the client, don't wedge */
		}

		char part[96];
		int  plen = snprintf(part,
		                     sizeof(part),
		                     "--" BOUNDARY "\r\n"
		                     "Content-Type: image/jpeg\r\n"
		                     "Content-Length: %u\r\n\r\n",
		                     (unsigned)len);

		bool failed = send_all(sock, part, (size_t)plen) != 0 || send_all(sock, buf, len) != 0 ||
		              send_all(sock, "\r\n", 2) != 0;
		release_claim();
		if (failed) {
			return;
		}
	}
}

/* True if `req` is "GET <path>" followed by a space or '?' -- not just a
 * same-length prefix match, so "GET /snapshot.jpg2" doesn't false-match
 * "/snapshot.jpg" and "GET /stream" doesn't false-match a hypothetical
 * "/streamx". */
static bool request_is(const char *req, const char *path)
{
	size_t plen = strlen(path);

	if (strncmp(req, "GET ", 4) != 0 || strncmp(req + 4, path, plen) != 0) {
		return false;
	}
	char c = req[4 + plen];

	return c == ' ' || c == '?';
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

		/* Bound every blocking op on this socket -- an idle or
		 * deliberately-stalled peer must not wedge the single server
		 * thread forever. */
		struct zsock_timeval tv = { .tv_sec = IDLE_TIMEOUT_S, .tv_usec = 0 };
		bool                 timeouts_armed =
		    zsock_setsockopt(client, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
		    zsock_setsockopt(client, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv)) == 0;

		if (!timeouts_armed) {
			/* Can't bound this client's blocking ops -- refuse it rather
			 * than risk wedging the one server thread on a peer that
			 * never sends a request line. */
			zsock_close(client);
			continue;
		}

		char    req[REQUEST_LINE] = { 0 };
		ssize_t n                 = zsock_recv(client, req, sizeof(req) - 1, 0);

		if (n <= 0) {
			/* Idle client: SO_RCVTIMEO expired (or it closed without
			 * sending anything) -- nothing to answer, just close. */
		} else if (request_is(req, "/stream")) {
			handle_stream(client);
		} else if (request_is(req, "/snapshot.jpg")) {
			handle_snapshot(client);
		} else {
			static const char resp404[] = "HTTP/1.1 404 Not Found\r\n"
			                              "Content-Length: 0\r\nConnection: close\r\n\r\n";
			send_all(client, resp404, sizeof(resp404) - 1);
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

	k_thread_create(&server_thread,
	                server_stack,
	                STACK_SIZE,
	                server_main,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(8),
	                0,
	                K_NO_WAIT);
	k_thread_name_set(&server_thread, "mjpeg_http");
	return 0;
}
