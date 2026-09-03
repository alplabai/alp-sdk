/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion sock` -- CC3501E TCP/UDP sockets (tcp-get <ip> <port>
 * <path>, serve <port> [seconds]), Alif companion only.  Command-group TU of the
 * alp_console_companion.c split (#673 Phase 2): registers onto the
 * (alp, companion) dynamic subcommand set the core TU declares.  Shared
 * companion context comes from
 * alp_console_companion_internal.h.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/ext/cc3501e/console.h>
#include <alp/peripheral.h>

#include "alp_console.h"
#include "alp_console_companion_internal.h"

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* ---- CC3501E TCP/UDP sockets (Alif companion) --------------------------- */
#define ALP_COMPANION_SOCK_OP_MS       15000u
#define ALP_COMPANION_SOCK_RECV_MS     8000u
#define ALP_COMPANION_SOCK_RECV_BUF    512u
#define ALP_COMPANION_SOCK_RECV_ROUNDS 128u
/* `sock serve` limits.  The request read stops at the blank line that ends the
 * HTTP headers, so the buffer only has to hold a request line + headers. */
#define ALP_COMPANION_SERVE_REQ_BUF    512u
#define ALP_COMPANION_SERVE_REQ_ROUNDS 16u
#define ALP_COMPANION_SERVE_BACKLOG    4u
#define ALP_COMPANION_SERVE_DEFAULT_S  60u
#define ALP_COMPANION_SERVE_MAX_S      3600u

/* Parse a dotted-quad "a.b.c.d" into 4 network-order octets (out[0] = a). */
static int companion_parse_ipv4(const char *s, uint8_t out[4])
{
	for (int i = 0; i < 4; i++) {
		char         *end = NULL;
		unsigned long v   = strtoul(s, &end, 10);
		if (end == s || v > 255u) {
			return -1;
		}
		out[i] = (uint8_t)v;
		if (i < 3) {
			if (*end != '.') {
				return -1;
			}
			s = end + 1;
		} else if (*end != '\0') {
			return -1;
		}
	}
	return 0;
}

/*
 * `sock tcp-get <ip> <port> <path>` -- the one-shot socket demo.
 *
 * Individual open/connect/send/recv/close over a shell is awkward fd-juggling,
 * so this command runs the WHOLE TCP client sequence in one call: open a TCP
 * socket, connect to <ip>:<port>, send a minimal HTTP/1.0 GET for <path>, drain
 * the response to the console, then close.  It is both the interactive demo and
 * the reference for the socket API (mirror it in hand-written firmware).
 */
static int cmd_companion_sock_tcp_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	uint8_t ip[4];
	if (companion_parse_ipv4(argv[1], ip) != 0) {
		shell_error(sh, "bad IPv4 address '%s' (want a.b.c.d)", argv[1]);
		return -EINVAL;
	}
	unsigned long port;
	if (alp_console_parse_ulong(argv[2], &port) != 0 || port == 0u || port > 0xFFFFu) {
		shell_error(sh, "bad port '%s' (want 1..65535)", argv[2]);
		return -EINVAL;
	}
	const char *path = argv[3];

	/* 1. Open a TCP (STREAM) socket on the CC3501E IP stack. */
	uint16_t     handle = 0;
	alp_status_t s;
	s = cc3501e_sock_open(companion_cc3501e,
	                      ALP_CC3501E_SOCK_FAMILY_IPV4,
	                      ALP_CC3501E_SOCK_TYPE_STREAM,
	                      0u,
	                      &handle,
	                      ALP_COMPANION_SOCK_OP_MS);
	if (s != ALP_OK) {
		shell_error(sh, "sock open failed (%d)", (int)s);
		return -EIO;
	}

	/* 2. Connect to the peer (runs the TCP handshake in the firmware). */
	s = cc3501e_sock_connect(
	    companion_cc3501e, handle, ip, (uint16_t)port, ALP_COMPANION_SOCK_OP_MS);
	if (s != ALP_OK) {
		shell_error(
		    sh, "connect %u.%u.%u.%u:%lu failed (%d)", ip[0], ip[1], ip[2], ip[3], port, (int)s);
		goto out_close;
	}

	/* 3. Send a minimal HTTP/1.0 request (Connection: close so the server ends the
	 *    response with a socket close we can detect as a run of empty reads). */
	char req[160];
	int  reqn = snprintf(req,
	                     sizeof(req),
	                     "GET %s HTTP/1.0\r\nHost: %u.%u.%u.%u\r\nConnection: close\r\n\r\n",
	                     path,
	                     ip[0],
	                     ip[1],
	                     ip[2],
	                     ip[3]);
	if (reqn <= 0 || (size_t)reqn >= sizeof(req)) {
		shell_error(sh, "request path too long");
		s = ALP_ERR_INVAL;
		goto out_close;
	}
	s = cc3501e_sock_send(companion_cc3501e,
	                      handle,
	                      (const uint8_t *)req,
	                      (size_t)reqn,
	                      NULL,
	                      ALP_COMPANION_SOCK_OP_MS);
	if (s != ALP_OK) {
		shell_error(sh, "send failed (%d)", (int)s);
		goto out_close;
	}

	/* 4. Drain the response.  recv returns 0 bytes on both "nothing yet" and peer
	 *    close; stop after a few consecutive empty reads (or the round cap). */
	shell_print(sh, "---- response ----");
	static uint8_t rx[ALP_COMPANION_SOCK_RECV_BUF];
	unsigned       empty    = 0;
	bool           got_data = false;
	for (unsigned round = 0; round < ALP_COMPANION_SOCK_RECV_ROUNDS; round++) {
		size_t n = 0;
		s        = cc3501e_sock_recv(
		    companion_cc3501e, handle, rx, sizeof(rx), &n, ALP_COMPANION_SOCK_RECV_MS);
		if (s != ALP_OK) {
			/* A recv error AFTER the body already arrived is the peer-close tail: the
			 * server sent its response then closed, so the firmware's post-close recv
			 * surfaces as TIMEOUT/NOT_READY/IO.  Treat it as a CLEAN end-of-response --
			 * stop quietly (clear s so the command still succeeds).  Only surface an
			 * error when the FIRST recv fails, before any data (a real fetch failure). */
			if (got_data) {
				s = ALP_OK;
			} else {
				shell_error(sh, "recv failed (%d)", (int)s);
			}
			break;
		}
		if (n == 0u) {
			if (++empty >= 3u) {
				break; /* three empty reads in a row -> treat as end of stream */
			}
			continue;
		}
		got_data = true;
		empty    = 0;
		shell_fprintf(sh, SHELL_NORMAL, "%.*s", (int)n, (const char *)rx);
	}
	shell_print(sh, "\n---- end ----");

out_close:
	(void)cc3501e_sock_close(companion_cc3501e, handle, ALP_COMPANION_SOCK_OP_MS);
	return (s == ALP_OK) ? 0 : -EIO;
}

/*
 * `sock serve <port> [seconds]` -- the listening-socket demo (protocol v9).
 *
 * The counterpart to `tcp-get`: instead of connecting OUT, this binds a
 * listening socket and answers inbound HTTP requests over the module's own
 * soft-AP, which is what a product with no Ethernet PHY needs in order to serve
 * an embedded web console.  Bring the AP up first (`alp companion wifi
 * ap-start ...`), then run this and point a browser on an associated client at
 * the AP address it prints.
 *
 * There is no accept call to wait on: the firmware accepts each connection on
 * its housekeeping tick and publishes it as an EVT_SOCK_ACCEPTED event, so this
 * command registers an event callback, hands the accepted handles to a queue,
 * and serves them from the shell thread.  The event thread
 * (alp_console_companion.c) is what drains the firmware ring, so it must be
 * running for connections to arrive at all -- this command does not poll.
 */
static struct k_msgq companion_accept_q;
static uint16_t      companion_accept_slots[8];

static void companion_serve_event_cb(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
{
	ARG_UNUSED(user);
	alp_cc3501e_sock_accepted_evt_t ev;

	if (opcode != (uint8_t)ALP_CC3501E_EVT_SOCK_ACCEPTED) {
		return;
	}
	if (cc3501e_sock_accepted_decode(payload, len, &ev) != ALP_OK) {
		return;
	}
	/* Queue-full drops the handle, which LEAKS a firmware socket -- the firmware
	 * hands ownership over with this event and never closes it itself.  Eight
	 * slots is deep for a console demo, and dropping silently would be the worse
	 * failure, so the serve loop reports the drop count when it finishes. */
	if (k_msgq_put(&companion_accept_q, &ev.handle, K_NO_WAIT) != 0) {
		return;
	}
}

/* Serve one accepted connection: read the request headers, answer a fixed
 * HTTP/1.0 200, close.  Returns 0 on success. */
static int companion_serve_one(const struct shell *sh, uint16_t handle)
{
	static uint8_t req[ALP_COMPANION_SERVE_REQ_BUF];
	size_t         used  = 0;
	unsigned       empty = 0;

	for (unsigned round = 0; round < ALP_COMPANION_SERVE_REQ_ROUNDS; round++) {
		size_t       n = 0;
		alp_status_t s = cc3501e_sock_recv(companion_cc3501e,
		                                   handle,
		                                   &req[used],
		                                   sizeof(req) - used - 1u,
		                                   &n,
		                                   ALP_COMPANION_SOCK_RECV_MS);
		if (s != ALP_OK) {
			break; /* client went away mid-request -- still answer what we can */
		}
		if (n == 0u) {
			if (++empty >= 3u) {
				break;
			}
			continue;
		}
		empty = 0;
		used += n;
		req[used] = 0u;
		if (strstr((const char *)req, "\r\n\r\n") != NULL) {
			break; /* headers complete */
		}
		if (used >= sizeof(req) - 1u) {
			break;
		}
	}
	/* Echo the request line so the bench log shows what the client actually
	 * asked for, not just that something connected. */
	if (used > 0u) {
		const char *eol = strchr((const char *)req, '\r');
		shell_print(sh, "  request: %.*s", eol ? (int)(eol - (const char *)req) : (int)used, req);
	} else {
		shell_print(sh, "  request: (none read)");
	}

	static const char body[] = "<!doctype html><title>ALP</title>"
	                           "<h1>Served by the CC3501E soft-AP</h1>";
	char              resp[320];
	const int         n = snprintf(resp,
	                               sizeof(resp),
	                               "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
	                               "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
	                               (unsigned)(sizeof(body) - 1u),
	                               body);
	if (n <= 0 || (size_t)n >= sizeof(resp)) {
		return -EINVAL;
	}
	const alp_status_t s = cc3501e_sock_send(companion_cc3501e,
	                                         handle,
	                                         (const uint8_t *)resp,
	                                         (size_t)n,
	                                         NULL,
	                                         ALP_COMPANION_SOCK_OP_MS);
	if (s != ALP_OK) {
		shell_error(sh, "  send failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "  replied %d bytes", n);
	return 0;
}

static int cmd_companion_sock_serve(const struct shell *sh, size_t argc, char **argv)
{
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}
	unsigned long port;
	if (alp_console_parse_ulong(argv[1], &port) != 0 || port == 0u || port > 0xFFFFu) {
		shell_error(sh, "bad port '%s' (want 1..65535)", argv[1]);
		return -EINVAL;
	}
	unsigned long secs = ALP_COMPANION_SERVE_DEFAULT_S;
	if (argc > 2 && (alp_console_parse_ulong(argv[2], &secs) != 0 || secs == 0u ||
	                 secs > ALP_COMPANION_SERVE_MAX_S)) {
		shell_error(
		    sh, "bad duration '%s' (want 1..%u seconds)", argv[2], ALP_COMPANION_SERVE_MAX_S);
		return -EINVAL;
	}

	/* The AP-side address is what a client must aim at.  Print it up front; a
	 * NOT_READY here means the AP role is not up, which is the usual reason a
	 * serve run sees nothing. */
	uint8_t            apip[4] = { 0 };
	const alp_status_t ips =
	    cc3501e_wifi_get_ip(companion_cc3501e, (uint8_t)ALP_CC3501E_WIFI_IFACE_AP, apip);
	if (ips == ALP_OK) {
		shell_print(sh, "ap ip: %u.%u.%u.%u", apip[0], apip[1], apip[2], apip[3]);
	} else {
		shell_warn(sh, "ap ip: unavailable (%d) -- is the AP started?", (int)ips);
	}

	k_msgq_init(&companion_accept_q,
	            (char *)companion_accept_slots,
	            sizeof(companion_accept_slots[0]),
	            ARRAY_SIZE(companion_accept_slots));

	uint16_t     srv = 0;
	alp_status_t s   = cc3501e_sock_open(companion_cc3501e,
	                                     ALP_CC3501E_SOCK_FAMILY_IPV4,
	                                     ALP_CC3501E_SOCK_TYPE_STREAM,
	                                     0u,
	                                     &srv,
	                                     ALP_COMPANION_SOCK_OP_MS);
	if (s != ALP_OK) {
		shell_error(sh, "sock open failed (%d)", (int)s);
		return -EIO;
	}
	/* Print the handle: the firmware hands out lwIP fd + 1, so a handle that
	 * climbs across successive runs is the visible symptom of a listening socket
	 * that was not released, and a bind/listen failure is only diagnosable
	 * against the handle it was attempted on. */
	shell_print(sh, "listen handle %u", (unsigned)srv);
	/* NULL ip = INADDR_ANY: the AP address does not exist until the role is up,
	 * so binding it explicitly would race the role-up. */
	s = cc3501e_sock_bind(companion_cc3501e, srv, NULL, (uint16_t)port, ALP_COMPANION_SOCK_OP_MS);
	if (s != ALP_OK) {
		shell_error(sh, "bind :%lu failed (%d)", port, (int)s);
		goto out_close;
	}
	s = cc3501e_sock_listen(
	    companion_cc3501e, srv, ALP_COMPANION_SERVE_BACKLOG, ALP_COMPANION_SOCK_OP_MS);
	if (s != ALP_OK) {
		shell_error(sh, "listen failed (%d)", (int)s);
		goto out_close;
	}
	s = cc3501e_add_event_callback(companion_cc3501e, companion_serve_event_cb, NULL);
	if (s != ALP_OK) {
		shell_error(sh, "cannot register the accept callback (%d)", (int)s);
		goto out_close;
	}

	/* The shell is BLOCKED for the whole window and there is no way to cut it
	 * short: Zephyr's shell has no cancellation hook a running command can poll,
	 * so ctrl-c does not reach this loop.  Say the duration plainly rather than
	 * offering an escape that does not exist -- pick a [seconds] you are willing
	 * to wait out. */
	shell_print(sh, "listening on :%lu for %lu s (the shell is blocked until then)", port, secs);
	const int64_t deadline = k_uptime_get() + (int64_t)secs * 1000;
	unsigned      served   = 0;
	while (k_uptime_get() < deadline) {
		uint16_t handle = 0;

		if (k_msgq_get(&companion_accept_q, &handle, K_MSEC(200)) != 0) {
			continue; /* nobody connected in this window */
		}
		shell_print(sh, "accepted handle %u", (unsigned)handle);
		(void)companion_serve_one(sh, handle);
		(void)cc3501e_sock_close(companion_cc3501e, handle, ALP_COMPANION_SOCK_OP_MS);
		served++;
	}
	(void)cc3501e_remove_event_callback(companion_cc3501e, companion_serve_event_cb, NULL);
	shell_print(sh, "served %u connection(s)", served);

out_close:
	(void)cc3501e_sock_close(companion_cc3501e, srv, ALP_COMPANION_SOCK_OP_MS);
	return (s == ALP_OK) ? 0 : -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_sock_subcmds,
    /* "tcp-get" is a shell command name, not a subtraction expression. */
    /* clang-format off */
    SHELL_CMD_ARG(tcp-get,
                  NULL,
                  "tcp-get <ip> <port> <path>  -- HTTP/1.0 GET over a TCP socket",
                  cmd_companion_sock_tcp_get,
                  4,
                  0),
    /* clang-format on */
    SHELL_CMD_ARG(serve,
                  NULL,
                  "serve <port> [seconds]  -- listen and answer HTTP over the soft-AP",
                  cmd_companion_sock_serve,
                  2,
                  1),
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 sock,
                 &alp_companion_sock_subcmds,
                 "CC3501E TCP/UDP sockets (tcp-get <ip> <port> <path>, serve <port> [seconds])",
                 NULL,
                 1,
                 0);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */
