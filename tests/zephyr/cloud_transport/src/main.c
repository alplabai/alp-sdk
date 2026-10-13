/* SPDX-License-Identifier: Apache-2.0 */

/*
 * <alp/cloud_transport.h> over plain-TCP loopback: argument checks, the
 * lifecycle, and the receive contract an MQTT process loop depends on
 * (0 = retry, negative = error or peer gone).  The test is its own peer; no
 * host network, no TLS handshake.  The send side is covered only for the
 * success and not-connected cases: a send timeout and a partial send are not
 * exercised here.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include <alp/cloud_transport.h>

#include "peer.h"

/* One port per test: a closed listener's port is not reusable immediately. */
enum {
	PORT_ROUND_TRIP = 4201,
	PORT_TIMEOUT,
	PORT_PEER_CLOSE,
	PORT_LIFECYCLE,
	PORT_NO_LISTENER,
};

#define RECV_TIMEOUT_MS 100

int peer_listen(uint16_t port)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
	int                fd   = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	zassert_true(fd >= 0, "socket: %d", errno);
	zassert_equal(zsock_inet_pton(AF_INET, PEER_HOST, &addr.sin_addr), 1);
	zassert_ok(zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)), "bind: %d", errno);
	zassert_ok(zsock_listen(fd, 1), "listen: %d", errno);
	return fd;
}

int peer_accept(int listen_fd)
{
	int fd = zsock_accept(listen_fd, NULL, NULL);

	zassert_true(fd >= 0, "accept: %d", errno);
	return fd;
}

static alp_cloud_transport_config_t plaintext_cfg(uint16_t port)
{
	return (alp_cloud_transport_config_t){
		.host            = PEER_HOST,
		.port            = port,
		.plaintext       = true,
		.recv_timeout_ms = RECV_TIMEOUT_MS,
	};
}

ZTEST(cloud_transport, test_connect_rejects_bad_arguments)
{
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = plaintext_cfg(PORT_NO_LISTENER);

	zassert_equal(alp_cloud_transport_connect(NULL, &cfg), ALP_ERR_INVAL);
	zassert_equal(alp_cloud_transport_connect(&t, NULL), ALP_ERR_INVAL);

	cfg.host = "";
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_INVAL);
	cfg.host = NULL;
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_INVAL);

	cfg = plaintext_cfg(0);
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_INVAL);
	zassert_true(t.fd < 0, "a rejected connect must not leave a socket behind");
}

ZTEST(cloud_transport, test_tls_is_never_silently_downgraded)
{
	static const int             tags[] = { 1 };
	alp_cloud_transport_t        t      = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg    = plaintext_cfg(PORT_NO_LISTENER);

	/* TLS (plaintext not asked for) with no security tag: nothing to verify
	 * the peer against, so it is an error, not a fall-back to plain TCP. */
	cfg.plaintext = false;
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_INVAL);

	cfg.sec_tags      = tags;
	cfg.sec_tag_count = 0;
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_INVAL);

	if (!IS_ENABLED(CONFIG_NET_SOCKETS_SOCKOPT_TLS)) {
		/* TLS asked for with credentials, in a build without TLS sockets. */
		cfg.sec_tag_count = ARRAY_SIZE(tags);
		zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_NOSUPPORT);
	}
	zassert_true(t.fd < 0);
}

ZTEST(cloud_transport, test_unresolvable_host_is_reported)
{
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = plaintext_cfg(PORT_NO_LISTENER);

	if (IS_ENABLED(CONFIG_DNS_RESOLVER)) {
		ztest_test_skip();
	}
	cfg.host = "broker.invalid";
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_NOT_FOUND);
	zassert_true(t.fd < 0);
}

ZTEST(cloud_transport, test_connect_without_listener_fails)
{
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = plaintext_cfg(PORT_NO_LISTENER);

	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_IO);
	zassert_equal(t.last_errno, ECONNREFUSED, "last_errno = %d", t.last_errno);
	zassert_true(t.fd < 0);
}

ZTEST(cloud_transport, test_round_trip)
{
	static const char            ping[] = "ping";
	static const char            pong[] = "pong!";
	char                         buf[16];
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = plaintext_cfg(PORT_ROUND_TRIP);
	int                          lfd = peer_listen(PORT_ROUND_TRIP);

	zassert_ok(alp_cloud_transport_connect(&t, &cfg));
	int pfd = peer_accept(lfd);

	zassert_equal(alp_cloud_transport_send(&t, ping, sizeof(ping)), sizeof(ping));
	zassert_equal(zsock_recv(pfd, buf, sizeof(buf), 0), sizeof(ping));
	zassert_mem_equal(buf, ping, sizeof(ping));

	zassert_equal(zsock_send(pfd, pong, sizeof(pong), 0), sizeof(pong));
	zassert_equal(alp_cloud_transport_recv(&t, buf, sizeof(buf)), sizeof(pong));
	zassert_mem_equal(buf, pong, sizeof(pong));

	alp_cloud_transport_close(&t);
	zsock_close(pfd);
	zsock_close(lfd);
}

ZTEST(cloud_transport, test_recv_timeout_returns_zero)
{
	char                         buf[8];
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = plaintext_cfg(PORT_TIMEOUT);
	int                          lfd = peer_listen(PORT_TIMEOUT);

	zassert_ok(alp_cloud_transport_connect(&t, &cfg));
	int     pfd   = peer_accept(lfd);
	int64_t start = k_uptime_get();

	/* The peer is connected and silent: "no data yet", so 0, and only after
	 * the configured timeout rather than at once or never. */
	zassert_equal(alp_cloud_transport_recv(&t, buf, sizeof(buf)), 0);
	zassert_true(k_uptime_get() - start >= RECV_TIMEOUT_MS / 2);

	alp_cloud_transport_close(&t);
	zsock_close(pfd);
	zsock_close(lfd);
}

ZTEST(cloud_transport, test_peer_close_is_an_error_not_zero)
{
	char                         buf[8];
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = plaintext_cfg(PORT_PEER_CLOSE);
	int                          lfd = peer_listen(PORT_PEER_CLOSE);

	zassert_ok(alp_cloud_transport_connect(&t, &cfg));
	zsock_close(peer_accept(lfd));

	/* coreMQTT retries on 0 forever; a closed connection must be negative. */
	zassert_equal(alp_cloud_transport_recv(&t, buf, sizeof(buf)), ALP_ERR_IO);
	zassert_equal(t.last_errno, ENOTCONN);

	alp_cloud_transport_close(&t);
	zsock_close(lfd);
}

ZTEST(cloud_transport, test_lifecycle)
{
	char                         buf[4] = { 0 };
	alp_cloud_transport_t        t      = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg    = plaintext_cfg(PORT_LIFECYCLE);
	int                          lfd    = peer_listen(PORT_LIFECYCLE);

	zassert_equal(alp_cloud_transport_send(&t, buf, sizeof(buf)), ALP_ERR_NOT_READY);
	zassert_equal(alp_cloud_transport_recv(&t, buf, sizeof(buf)), ALP_ERR_NOT_READY);

	zassert_ok(alp_cloud_transport_connect(&t, &cfg));
	int pfd = peer_accept(lfd);

	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_BUSY);
	zassert_equal(alp_cloud_transport_send(&t, NULL, 1), ALP_ERR_INVAL);
	zassert_equal(alp_cloud_transport_recv(&t, NULL, 1), ALP_ERR_INVAL);
	zassert_equal(alp_cloud_transport_send(&t, buf, 0), 0);

	alp_cloud_transport_close(&t);
	zassert_true(t.fd < 0);
	alp_cloud_transport_close(&t); /* idempotent */
	alp_cloud_transport_close(NULL);
	zassert_equal(alp_cloud_transport_send(&t, buf, sizeof(buf)), ALP_ERR_NOT_READY);

	zsock_close(pfd);
	zsock_close(lfd);
}

ZTEST_SUITE(cloud_transport, NULL, NULL, NULL, NULL, NULL);
