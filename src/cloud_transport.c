/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/cloud_transport.h> on Zephyr sockets and mbedtls.  The contract and
 * the return convention are documented in the header.
 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>

#include <alp/cloud_transport.h>

#include "alp_errno.h"

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
#include <zephyr/net/tls_credentials.h>

/* The public config takes `const int *` so the header needs no Zephyr type. */
BUILD_ASSERT(sizeof(int) == sizeof(sec_tag_t), "sec_tags is passed to Zephyr as sec_tag_t[]");
#endif

#define USEC_PER_MSEC_CT 1000

/* Zephyr reports a timed-out blocking call as EAGAIN (EWOULDBLOCK is the same
 * value on Zephyr, but name both so the intent survives a libc that differs). */
static bool would_block(int err)
{
	return err == EAGAIN || err == EWOULDBLOCK;
}

/* Record the positive errno for diagnostics and map it to a status. */
static alp_status_t fail(alp_cloud_transport_t *t, int err)
{
	t->last_errno = err;
	return alp_status_from_zephyr_errno(-err);
}

static alp_status_t resolve(const char *host, uint16_t port, struct sockaddr_in *out)
{
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port   = htons(port);

	if (zsock_inet_pton(AF_INET, host, &out->sin_addr) == 1) {
		return ALP_OK;
	}

#if defined(CONFIG_DNS_RESOLVER)
	struct zsock_addrinfo  hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res   = NULL;

	if (zsock_getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
		if (res != NULL) {
			zsock_freeaddrinfo(res);
		}
		return ALP_ERR_NOT_FOUND;
	}
	out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	zsock_freeaddrinfo(res);
	return ALP_OK;
#else
	/* No resolver in this build: only an IPv4 literal can be a host. */
	return ALP_ERR_NOT_FOUND;
#endif
}

/* Returns 0 or a positive errno. */
static int set_timeout(int fd, int optname, uint32_t timeout_ms)
{
	if (timeout_ms == 0U) {
		return 0;
	}

	struct zsock_timeval tv = {
		.tv_sec  = timeout_ms / MSEC_PER_SEC,
		.tv_usec = (timeout_ms % MSEC_PER_SEC) * USEC_PER_MSEC_CT,
	};

	return zsock_setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv)) < 0 ? errno : 0;
}

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
/* Returns 0 or a positive errno. */
static int configure_tls(int fd, const alp_cloud_transport_config_t *cfg)
{
	/* Verification is not optional: REQUIRED is also the client default, but
	 * a default can change under us and this is the property the whole
	 * transport exists to provide. */
	int verify = TLS_PEER_VERIFY_REQUIRED;

	if (zsock_setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify)) < 0) {
		return errno;
	}
	if (zsock_setsockopt(fd,
	                     SOL_TLS,
	                     TLS_SEC_TAG_LIST,
	                     cfg->sec_tags,
	                     cfg->sec_tag_count * sizeof(cfg->sec_tags[0])) < 0) {
		return errno;
	}
	/* SNI and the name the certificate is checked against. */
	const char *name = (cfg->server_name != NULL) ? cfg->server_name : cfg->host;

	if (zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME, name, strlen(name)) < 0) {
		return errno;
	}
#if defined(CONFIG_MBEDTLS_SSL_ALPN)
	if (cfg->alpn != NULL && cfg->alpn_count > 0U) {
		if (zsock_setsockopt(
		        fd, SOL_TLS, TLS_ALPN_LIST, cfg->alpn, cfg->alpn_count * sizeof(cfg->alpn[0])) <
		    0) {
			return errno;
		}
	}
#endif
	return 0;
}
#endif /* CONFIG_NET_SOCKETS_SOCKOPT_TLS */

static alp_status_t validate_tls(const alp_cloud_transport_config_t *cfg)
{
	/* TLS with no credentials would have nothing to verify the peer against,
	 * and an empty name would switch the name check off inside mbedtls. */
	if (cfg->sec_tags == NULL || cfg->sec_tag_count == 0U ||
	    cfg->sec_tag_count > (size_t)INT_MAX / sizeof(cfg->sec_tags[0])) {
		return ALP_ERR_INVAL;
	}
	if (cfg->server_name != NULL && cfg->server_name[0] == '\0') {
		return ALP_ERR_INVAL;
	}
	if (!IS_ENABLED(CONFIG_NET_SOCKETS_SOCKOPT_TLS)) {
		return ALP_ERR_NOSUPPORT;
	}
	if (cfg->alpn != NULL && cfg->alpn_count > 0U) {
#if defined(CONFIG_MBEDTLS_SSL_ALPN)
		if (cfg->alpn_count > CONFIG_NET_SOCKETS_TLS_MAX_APP_PROTOCOLS) {
			return ALP_ERR_INVAL;
		}
#else
		/* Dropping a requested ALPN would connect and then fail in a way
		 * that is hard to trace (AWS on 443 closes the connection). */
		return ALP_ERR_NOSUPPORT;
#endif
	}
	return ALP_OK;
}

static alp_status_t validate(const alp_cloud_transport_t        *t,
                             const alp_cloud_transport_config_t *cfg)
{
	if (t == NULL || cfg == NULL || cfg->host == NULL || cfg->host[0] == '\0' || cfg->port == 0U) {
		return ALP_ERR_INVAL;
	}
	if (t->fd >= 0) {
		return ALP_ERR_BUSY;
	}
	return cfg->plaintext ? ALP_OK : validate_tls(cfg);
}

alp_status_t alp_cloud_transport_connect(alp_cloud_transport_t              *t,
                                         const alp_cloud_transport_config_t *cfg)
{
	struct sockaddr_in addr;
	alp_status_t       st = validate(t, cfg);

	if (st != ALP_OK) {
		return st;
	}
	t->last_errno = 0;
	st            = resolve(cfg->host, cfg->port, &addr);
	if (st != ALP_OK) {
		return st;
	}

	int fd = zsock_socket(AF_INET, SOCK_STREAM, cfg->plaintext ? IPPROTO_TCP : IPPROTO_TLS_1_2);

	if (fd < 0) {
		return fail(t, errno);
	}

	int err = 0;

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	if (!cfg->plaintext) {
		err = configure_tls(fd, cfg);
	}
#endif
	if (err == 0) {
		err = set_timeout(fd, SO_SNDTIMEO, cfg->send_timeout_ms);
	}
	if (err == 0) {
		err = set_timeout(fd, SO_RCVTIMEO, cfg->recv_timeout_ms);
	}
	if (err == 0 && zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		err = errno;
	}
	if (err != 0) {
		(void)zsock_close(fd);
		return fail(t, err);
	}

	t->fd = fd;
	return ALP_OK;
}

int32_t alp_cloud_transport_send(alp_cloud_transport_t *t, const void *buf, size_t len)
{
	if (t == NULL || buf == NULL) {
		return ALP_ERR_INVAL;
	}
	if (t->fd < 0) {
		return ALP_ERR_NOT_READY;
	}
	if (len == 0U) {
		return 0;
	}

	ssize_t n = zsock_send(t->fd, buf, MIN(len, (size_t)INT32_MAX), 0);

	if (n < 0) {
		return would_block(errno) ? 0 : fail(t, errno);
	}
	return (int32_t)n;
}

int32_t alp_cloud_transport_recv(alp_cloud_transport_t *t, void *buf, size_t len)
{
	if (t == NULL || buf == NULL) {
		return ALP_ERR_INVAL;
	}
	if (t->fd < 0) {
		return ALP_ERR_NOT_READY;
	}
	if (len == 0U) {
		return 0;
	}

	ssize_t n = zsock_recv(t->fd, buf, MIN(len, (size_t)INT32_MAX), 0);

	if (n < 0) {
		return would_block(errno) ? 0 : fail(t, errno);
	}
	if (n == 0) {
		/* Orderly shutdown by the peer.  Reporting it as 0 would read as
		 * "no data yet, retry" and spin the caller forever. */
		t->last_errno = ENOTCONN;
		return ALP_ERR_IO;
	}
	return (int32_t)n;
}

void alp_cloud_transport_close(alp_cloud_transport_t *t)
{
	if (t == NULL || t->fd < 0) {
		return;
	}
	(void)zsock_close(t->fd);
	t->fd = -1;
}
