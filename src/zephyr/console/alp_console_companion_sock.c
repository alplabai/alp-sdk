/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion sock` -- CC3501E TCP/UDP sockets (tcp-get <ip> <port>
 * <path>), Alif companion only.  Command-group TU of the
 * alp_console_companion.c split (#673 Phase 2): registers onto the
 * (alp, companion) dynamic subcommand set the core TU declares.  Shared
 * companion context + bridge-bus mutex come from
 * alp_console_companion_internal.h.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	s = cc3501e_sock_open(companion_cc3501e,
	                      ALP_CC3501E_SOCK_FAMILY_IPV4,
	                      ALP_CC3501E_SOCK_TYPE_STREAM,
	                      0u,
	                      &handle,
	                      ALP_COMPANION_SOCK_OP_MS);
	k_mutex_unlock(&companion_bus_lock);
	if (s != ALP_OK) {
		shell_error(sh, "sock open failed (%d)", (int)s);
		return -EIO;
	}

	/* 2. Connect to the peer (runs the TCP handshake in the firmware). */
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	s = cc3501e_sock_connect(
	    companion_cc3501e, handle, ip, (uint16_t)port, ALP_COMPANION_SOCK_OP_MS);
	k_mutex_unlock(&companion_bus_lock);
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
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	s = cc3501e_sock_send(companion_cc3501e,
	                      handle,
	                      (const uint8_t *)req,
	                      (size_t)reqn,
	                      NULL,
	                      ALP_COMPANION_SOCK_OP_MS);
	k_mutex_unlock(&companion_bus_lock);
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
		k_mutex_lock(&companion_bus_lock, K_FOREVER);
		s = cc3501e_sock_recv(
		    companion_cc3501e, handle, rx, sizeof(rx), &n, ALP_COMPANION_SOCK_RECV_MS);
		k_mutex_unlock(&companion_bus_lock);
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
	k_mutex_lock(&companion_bus_lock, K_FOREVER);
	(void)cc3501e_sock_close(companion_cc3501e, handle, ALP_COMPANION_SOCK_OP_MS);
	k_mutex_unlock(&companion_bus_lock);
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
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 sock,
                 &alp_companion_sock_subcmds,
                 "CC3501E TCP/UDP sockets (tcp-get <ip> <port> <path>)",
                 NULL,
                 1,
                 0);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */
