/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cloud_transport.h
 * @brief TLS-over-TCP client transport for cloud MQTT stacks.
 *
 * The AWS IoT and Azure IoT embedded-C SDKs ship no network transport:
 * coreMQTT calls an application-supplied `TransportInterface_t`, and the Azure
 * hub client only builds topics and credentials.  This is the socket both sit
 * on.  It is a client transport on Zephyr sockets and mbedtls, gated by
 * @c CONFIG_ALP_CLOUD_TRANSPORT; the coreMQTT adapter over it lives with the
 * AWS glue (`vendors/aws-iot/alp_cloud_transport_coremqtt.h`).
 *
 * The caller owns an @ref alp_cloud_transport_t (no pool, no heap), connects
 * it with @ref alp_cloud_transport_connect, and moves bytes with
 * @ref alp_cloud_transport_send / @ref alp_cloud_transport_recv.
 *
 * @par Credentials and verification
 * TLS credentials are Zephyr's: register the CA (and, for mutual TLS, the
 * client certificate and key) with `tls_credential_add()` under a security
 * tag, and pass the tags in @c sec_tags.  The peer certificate is ALWAYS
 * verified, against @c server_name, or against @c host when that is NULL.
 * There is no option to turn verification off.  TLS with no security tag is
 * rejected; it never falls back to plain TCP, which has to be requested with
 * @c plaintext.  mbedtls checks certificate validity dates only when
 * @c CONFIG_MBEDTLS_HAVE_TIME_DATE is set and the system clock is valid.
 *
 * @par Limits
 * IPv4 only.  @c host is resolved through DNS when @c CONFIG_DNS_RESOLVER is
 * set and must be an IPv4 literal otherwise.
 *
 * @code
 *     static const int tags[] = { MY_CA_TAG };
 *     alp_cloud_transport_t t = ALP_CLOUD_TRANSPORT_INIT;
 *     const alp_cloud_transport_config_t cfg = {
 *         .host = "192.0.2.10", .server_name = "broker.example.com",
 *         .port = 8883, .sec_tags = tags, .sec_tag_count = 1,
 *         .recv_timeout_ms = 100,
 *     };
 *     if (alp_cloud_transport_connect(&t, &cfg) == ALP_OK) {
 *         alp_cloud_transport_send(&t, buf, len);
 *     }
 * @endcode
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 */

#ifndef ALP_CLOUD_TRANSPORT_H
#define ALP_CLOUD_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Connection parameters.  Zero-initialise, then set what you need. */
typedef struct {
	/** Broker host name or dotted IPv4 literal. */
	const char *host;
	/**
	 * Name sent as SNI and checked against the server certificate.  NULL
	 * uses @c host.  Set it when @c host is an IP literal (a build with no
	 * DNS resolver), so the certificate is still checked against the
	 * broker's name.
	 */
	const char *server_name;
	/** TCP port (8883 for MQTT over TLS, 443 for AWS with ALPN). */
	uint16_t port;
	/**
	 * Zephyr TLS security tags holding the CA and optional client
	 * certificate/key.  Required unless @c plaintext is set.
	 */
	const int *sec_tags;
	/** Number of entries in @c sec_tags. */
	size_t sec_tag_count;
	/**
	 * Optional ALPN protocol names (e.g. `"x-amzn-mqtt-ca"` for AWS IoT
	 * Core on port 443), NULL for none.  Needs @c CONFIG_MBEDTLS_SSL_ALPN.
	 * The array and its strings are NOT copied: they must stay valid until
	 * @ref alp_cloud_transport_close.
	 */
	const char *const *alpn;
	/**
	 * Number of entries in @c alpn; at most
	 * @c CONFIG_NET_SOCKETS_TLS_MAX_APP_PROTOCOLS.
	 */
	size_t alpn_count;
	/**
	 * Plain TCP, no TLS.  For a local test broker only: credentials and
	 * payloads cross the network unencrypted.  It must be asked for
	 * explicitly; leaving @c sec_tags empty does not select it.
	 */
	bool plaintext;
	/** Send timeout in milliseconds.  0 blocks until the stack accepts data. */
	uint32_t send_timeout_ms;
	/**
	 * Receive timeout in milliseconds.  0 blocks until data arrives.  An
	 * MQTT process loop wants a short, non-zero value so it can time out.
	 */
	uint32_t recv_timeout_ms;
} alp_cloud_transport_config_t;

/**
 * @brief Caller-allocated connection state.
 *
 * Initialise with @ref ALP_CLOUD_TRANSPORT_INIT.  The layout is
 * @c [ABI-EXPERIMENTAL]; read @c last_errno, leave @c fd alone.
 */
typedef struct {
	/** Socket descriptor, or a negative value when not connected. */
	int fd;
	/**
	 * Positive errno of the most recent socket-level failure, 0 if none.
	 * Diagnostic only: the status code is the contract, this tells a
	 * refused connection (ECONNREFUSED) from a failed handshake.
	 */
	int last_errno;
} alp_cloud_transport_t;

/** @brief Static initialiser for an unconnected @ref alp_cloud_transport_t. */
#define ALP_CLOUD_TRANSPORT_INIT { .fd = -1, .last_errno = 0 }

/**
 * @brief Resolve the host, connect, and (unless plaintext) complete the TLS
 *        handshake with the peer certificate verified.
 *
 * @param t   Unconnected transport.
 * @param cfg Connection parameters; not retained, except the ALPN array.
 *
 * @retval ALP_OK            connected.
 * @retval ALP_ERR_INVAL     NULL argument, empty host or server name, port 0,
 *                           TLS requested with no security tag, or more ALPN
 *                           names than the stack accepts.
 * @retval ALP_ERR_BUSY      @p t is already connected.
 * @retval ALP_ERR_NOSUPPORT TLS requested but
 *                           @c CONFIG_NET_SOCKETS_SOCKOPT_TLS is off, or ALPN
 *                           requested but @c CONFIG_MBEDTLS_SSL_ALPN is off.
 * @retval ALP_ERR_NOT_FOUND the host did not resolve.
 * @retval ALP_ERR_IO        connection refused or the TLS handshake failed
 *                           (see @c last_errno); other socket errors map
 *                           through the SDK's errno baseline.
 */
alp_status_t alp_cloud_transport_connect(alp_cloud_transport_t              *t,
                                         const alp_cloud_transport_config_t *cfg);

/**
 * @brief Send up to @p len bytes.
 *
 * @param t   Connected transport.
 * @param buf Bytes to send.
 * @param len Number of bytes in @p buf.
 *
 * @return The number of bytes sent, which may be fewer than @p len; 0 when the
 *         send timed out with nothing written (retry); or a negative
 *         @ref alp_status_t -- @c ALP_ERR_INVAL for a NULL argument,
 *         @c ALP_ERR_NOT_READY when @p t is not connected, otherwise the
 *         mapped socket error.
 */
int32_t alp_cloud_transport_send(alp_cloud_transport_t *t, const void *buf, size_t len);

/**
 * @brief Receive up to @p len bytes.
 *
 * @param t   Connected transport.
 * @param buf Destination buffer.
 * @param len Capacity of @p buf.
 *
 * @return The number of bytes received; 0 when the receive timed out with no
 *         data (retry); or a negative @ref alp_status_t -- @c ALP_ERR_INVAL for
 *         a NULL argument, @c ALP_ERR_NOT_READY when @p t is not connected,
 *         @c ALP_ERR_IO when the peer closed the connection, otherwise the
 *         mapped socket error.  A closed connection never returns 0.
 */
int32_t alp_cloud_transport_recv(alp_cloud_transport_t *t, void *buf, size_t len);

/**
 * @brief Close the connection.
 *
 * Safe on NULL and on an unconnected transport.
 *
 * @param t Transport to close.
 */
void alp_cloud_transport_close(alp_cloud_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CLOUD_TRANSPORT_H */
