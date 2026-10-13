/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The TLS path of <alp/cloud_transport.h> over loopback.  A server thread terminates
 * TLS with Zephyr's bundled test certificate (CN=localhost, signed by the
 * bundled test CA); the transport connects to 127.0.0.1 and verifies it by
 * name.  The two refusal tests are the point: a peer that does not chain to
 * the configured CA, or whose certificate is for another name, must not
 * connect.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/ztest.h>

#include <alp/cloud_transport.h>

#include "peer.h"

static const unsigned char ca_cert[] = {
#include "ca.der.inc"
};
static const unsigned char server_cert[] = {
#include "server.der.inc"
};
static const unsigned char server_key[] = {
#include "server_privkey.der.inc"
};
/* Self-signed and unrelated to ca_cert: a trust anchor the server does not
 * chain to. */
static const unsigned char other_ca_cert[] = {
#include "echo-apps-cert.der.inc"
};

enum {
	TAG_SERVER = 10,
	TAG_CA,
	TAG_OTHER_CA,
};

enum {
	PORT_TLS_OK = 4401,
	PORT_TLS_WRONG_CA,
	PORT_TLS_WRONG_NAME,
	PORT_TLS_WRONG_NAME_DEFAULT,
	PORT_TLS_ALPN_OK,
	PORT_TLS_ALPN_MISMATCH,
};

/* The protocol the test server offers when a test asks for ALPN. */
#define SERVER_ALPN "x-alp-test"

/* What a refused handshake looks like to the caller: Zephyr reports a failed
 * TLS handshake on connect() as ECONNABORTED, mapped to ALP_ERR_IO.  Asserted
 * exactly, so a refusal cannot be confused with a config or memory failure. */
#define assert_handshake_refused(t, cfg) \
	do { \
		zassert_equal(alp_cloud_transport_connect((t), (cfg)), ALP_ERR_IO); \
		zassert_equal((t)->last_errno, ECONNABORTED, "last_errno = %d", (t)->last_errno); \
		zassert_true((t)->fd < 0); \
	} while (0)

/* The certificate's subject; the socket itself connects to PEER_HOST. */
#define SERVER_NAME "localhost"

#define SERVER_STACK_SIZE 8192
K_THREAD_STACK_DEFINE(g_server_stack, SERVER_STACK_SIZE);
static struct k_thread g_server_thread;
/* The listening socket of the running server, or -1 when none is running. */
static int g_listen_fd = -1;

/* Accept one connection (the TLS handshake happens inside accept), echo one
 * message back, and close.  A refused handshake makes accept fail; that is the
 * expected outcome of the refusal tests, so it is not asserted here. */
static void tls_server(void *p1, void *p2, void *p3)
{
	int  lfd = POINTER_TO_INT(p1);
	char buf[32];

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int fd = zsock_accept(lfd, NULL, NULL);

	if (fd < 0) {
		return;
	}

	ssize_t n = zsock_recv(fd, buf, sizeof(buf), 0);

	if (n > 0) {
		(void)zsock_send(fd, buf, n, 0);
	}
	zsock_close(fd);
}

static void tls_listen_alpn(uint16_t port, bool offer_alpn)
{
	static const sec_tag_t   tags[] = { TAG_SERVER };
	static const char *const alpn[] = { SERVER_ALPN };
	struct sockaddr_in       addr   = { .sin_family = AF_INET, .sin_port = htons(port) };
	int                      fd     = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);

	zassert_true(fd >= 0, "socket: %d", errno);
	/* Recorded first, so a failed assertion below still gets it closed. */
	g_listen_fd = fd;
	zassert_ok(zsock_setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags)));
	if (offer_alpn) {
		zassert_ok(zsock_setsockopt(fd, SOL_TLS, TLS_ALPN_LIST, alpn, sizeof(alpn)));
	}
	zassert_equal(zsock_inet_pton(AF_INET, PEER_HOST, &addr.sin_addr), 1);
	zassert_ok(zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)), "bind: %d", errno);
	zassert_ok(zsock_listen(fd, 1), "listen: %d", errno);

	k_thread_create(&g_server_thread,
	                g_server_stack,
	                SERVER_STACK_SIZE,
	                tls_server,
	                INT_TO_POINTER(fd),
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(8),
	                0,
	                K_NO_WAIT);
}

static void tls_listen(uint16_t port)
{
	tls_listen_alpn(port, false);
}

/* Closing the listener unblocks a server still parked in accept.  Also the
 * suite's `after` hook, so a failed assertion cannot leave the thread object
 * live for the next test to reuse. */
static void tls_stop(void)
{
	if (g_listen_fd < 0) {
		return;
	}
	zsock_close(g_listen_fd);
	g_listen_fd = -1;
	if (k_thread_join(&g_server_thread, K_SECONDS(5)) != 0) {
		k_thread_abort(&g_server_thread);
	}
}

static void tls_after(void *fixture)
{
	ARG_UNUSED(fixture);
	tls_stop();
}

static void *tls_setup(void)
{
	zassert_ok(tls_credential_add(
	    TAG_SERVER, TLS_CREDENTIAL_PUBLIC_CERTIFICATE, server_cert, sizeof(server_cert)));
	zassert_ok(
	    tls_credential_add(TAG_SERVER, TLS_CREDENTIAL_PRIVATE_KEY, server_key, sizeof(server_key)));
	zassert_ok(tls_credential_add(TAG_CA, TLS_CREDENTIAL_CA_CERTIFICATE, ca_cert, sizeof(ca_cert)));
	zassert_ok(tls_credential_add(
	    TAG_OTHER_CA, TLS_CREDENTIAL_CA_CERTIFICATE, other_ca_cert, sizeof(other_ca_cert)));
	return NULL;
}

static alp_cloud_transport_config_t tls_cfg(uint16_t port, const int *tag, const char *name)
{
	return (alp_cloud_transport_config_t){
		.host            = PEER_HOST,
		.server_name     = name,
		.port            = port,
		.sec_tags        = tag,
		.sec_tag_count   = 1,
		.recv_timeout_ms = 2000,
	};
}

ZTEST(cloud_transport_tls, test_handshake_and_round_trip)
{
	static const int             tag   = TAG_CA;
	static const char            msg[] = "over tls";
	char                         buf[16];
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = tls_cfg(PORT_TLS_OK, &tag, SERVER_NAME);

	tls_listen(PORT_TLS_OK);
	zassert_ok(alp_cloud_transport_connect(&t, &cfg));
	zassert_equal(alp_cloud_transport_send(&t, msg, sizeof(msg)), sizeof(msg));
	zassert_equal(alp_cloud_transport_recv(&t, buf, sizeof(buf)), sizeof(msg));
	zassert_mem_equal(buf, msg, sizeof(msg));

	alp_cloud_transport_close(&t);
}

ZTEST(cloud_transport_tls, test_untrusted_ca_is_refused)
{
	static const int             tag = TAG_OTHER_CA;
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = tls_cfg(PORT_TLS_WRONG_CA, &tag, SERVER_NAME);

	tls_listen(PORT_TLS_WRONG_CA);
	/* A server that does not chain to the configured CA. */
	assert_handshake_refused(&t, &cfg);
}

ZTEST(cloud_transport_tls, test_wrong_server_name_is_refused)
{
	static const int             tag = TAG_CA;
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = tls_cfg(PORT_TLS_WRONG_NAME, &tag, "broker.invalid");

	tls_listen(PORT_TLS_WRONG_NAME);
	/* A certificate issued for another name. */
	assert_handshake_refused(&t, &cfg);

	/* Same CA, same server, the name left to default to the IP literal:
	 * the certificate is for "localhost", so this is refused as well. */
	cfg.server_name = NULL;
	tls_stop();
	tls_listen(PORT_TLS_WRONG_NAME_DEFAULT);
	cfg.port = PORT_TLS_WRONG_NAME_DEFAULT;
	assert_handshake_refused(&t, &cfg);

	cfg.server_name = "";
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_INVAL);
}

ZTEST(cloud_transport_tls, test_alpn_is_negotiated)
{
	static const int             tag    = TAG_CA;
	static const char *const     alpn[] = { SERVER_ALPN };
	static const char            msg[]  = "alpn";
	char                         buf[8];
	alp_cloud_transport_t        t   = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg = tls_cfg(PORT_TLS_ALPN_OK, &tag, SERVER_NAME);

	cfg.alpn       = alpn;
	cfg.alpn_count = ARRAY_SIZE(alpn);
	tls_listen_alpn(PORT_TLS_ALPN_OK, true);

	zassert_ok(alp_cloud_transport_connect(&t, &cfg));
	zassert_equal(alp_cloud_transport_send(&t, msg, sizeof(msg)), sizeof(msg));
	zassert_equal(alp_cloud_transport_recv(&t, buf, sizeof(buf)), sizeof(msg));

	alp_cloud_transport_close(&t);
}

ZTEST(cloud_transport_tls, test_alpn_mismatch_is_refused)
{
	static const int             tag    = TAG_CA;
	static const char *const     alpn[] = { "x-not-offered" };
	static const char *const     many[] = { "a", "b", "c", "d", "e" };
	alp_cloud_transport_t        t      = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg    = tls_cfg(PORT_TLS_ALPN_MISMATCH, &tag, SERVER_NAME);

	BUILD_ASSERT(ARRAY_SIZE(many) > CONFIG_NET_SOCKETS_TLS_MAX_APP_PROTOCOLS);

	/* The server offers SERVER_ALPN only: proof the client's list reaches
	 * the handshake rather than being dropped on the way. */
	cfg.alpn       = alpn;
	cfg.alpn_count = ARRAY_SIZE(alpn);
	tls_listen_alpn(PORT_TLS_ALPN_MISMATCH, true);
	assert_handshake_refused(&t, &cfg);

	/* More names than the stack holds is a caller error, caught before any
	 * socket exists. */
	cfg.alpn       = many;
	cfg.alpn_count = ARRAY_SIZE(many);
	zassert_equal(alp_cloud_transport_connect(&t, &cfg), ALP_ERR_INVAL);
}

ZTEST_SUITE(cloud_transport_tls, NULL, tls_setup, NULL, tls_after, NULL);
