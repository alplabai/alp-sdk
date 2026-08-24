/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for #1645: src/backends/mqtt/zephyr_drv.c's
 * alp_mqtt_evt_cb() bounded its copy of an oversized PUBLISH payload
 * into `be->rx_buf` correctly (memory-safe -- the audit's claim on
 * that half was already refuted), but never drained the remainder
 * off the wire.  Zephyr's mqtt_client tracks how many payload bytes
 * are still owed via `internal.remaining_payload`; leaving it
 * non-zero makes every later `mqtt_input()` return `-EBUSY`
 * (subsys/net/lib/mqtt/mqtt.c's `client_read()`), so the connection
 * stops delivering ANYTHING -- forever -- after one broker message
 * larger than the scratch buffer, even though the message was
 * already PUBACKed (QoS 1+) or simply dropped (QoS 0) and will not
 * be resent.
 *
 * This is deliberately NOT a "does it link" test and NOT a
 * unit test against a hand-built mqtt_client struct: it drives the
 * REAL Zephyr mqtt_client state machine (mqtt_connect/mqtt_input/
 * mqtt_read_publish_payload) over a REAL TCP loopback socket, the
 * same recipe Zephyr's own upstream
 * tests/net/lib/mqtt/v3_1_1/mqtt_client uses to prove MQTT client
 * behaviour on native_sim -- adapted to IPv4 because
 * src/backends/mqtt/zephyr_drv.c's resolve_broker_addr() only does
 * a numeric zsock_inet_pton(AF_INET, ...) (no IPv6, no DNS, in this
 * v0.2 wrapper). A fake "broker" thread on the other end of the
 * loopback pair hand-encodes raw MQTT v3.1.1 wire bytes -- no
 * mosquitto/paho dependency.
 *
 * The proof this file exists to deliver: after the oversized
 * PUBLISH is dispatched, a SECOND alp_mqtt_loop() call must still
 * process the broker's next packet (a PINGRESP queued right behind
 * the undrained bytes) and return ALP_OK -- not ALP_ERR_BUSY. That
 * is the actual, externally-observable "does the connection recover"
 * behaviour, not merely "did the bounded copy avoid a crash" (which
 * was never in question).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include "alp/iot.h"
#include "alp/peripheral.h"

#define BROKER_PORT 18830

/* CONFIG_ALP_SDK_MQTT_BUF_SIZE stays at its production default (256 --
 * see this suite's prj.conf for why shrinking it broke mqtt_connect()
 * itself), so the oversized-publish fixture must clear that bound
 * with headroom. */
#define OVERSIZED_PAYLOAD_LEN 300u
#define TOPIC                 "t"

/* ---- Hand-encoded MQTT v3.1.1 wire bytes ------------------------- */

static const uint8_t connack_bytes[]  = { 0x20, 0x02, 0x00, 0x00 };
static const uint8_t pingresp_bytes[] = { 0xD0, 0x00 };

static uint8_t publish_payload[OVERSIZED_PAYLOAD_LEN];

/* PUBLISH, QoS 0, topic "t": fixed header (type|flags, remaining len),
 * variable header (2-byte BE topic length + topic, no packet id at
 * QoS 0), then the raw payload. */
static uint8_t publish_bytes[1 + 4 + 2 + sizeof(TOPIC) - 1 + OVERSIZED_PAYLOAD_LEN];
static size_t  publish_bytes_len;

/* MQTT v3.1.1 remaining-length encoding (base-128, continuation bit
 * on every byte but the last) -- mirrors Zephyr's own upstream test
 * helper (tests/net/lib/mqtt/v3_1_1/mqtt_client's encode_fixed_hdr). */
static size_t encode_remaining_length(uint8_t *out, uint32_t length)
{
	size_t n = 0;

	do {
		uint8_t byte = (uint8_t)(length % 128u);

		length /= 128u;
		if (length > 0) byte |= 0x80u;
		out[n++] = byte;
	} while (length > 0);

	return n;
}

static void build_publish_packet(void)
{
	const size_t topic_len = sizeof(TOPIC) - 1;
	const size_t var_len   = 2u + topic_len;
	const size_t rem_len   = var_len + OVERSIZED_PAYLOAD_LEN;

	for (size_t i = 0; i < OVERSIZED_PAYLOAD_LEN; ++i) {
		publish_payload[i] = (uint8_t)('A' + (i % 26));
	}

	uint8_t *p = publish_bytes;
	*p++       = 0x30; /* PUBLISH, QoS 0, no DUP/RETAIN */
	p += encode_remaining_length(p, (uint32_t)rem_len);
	*p++ = 0x00;
	*p++ = (uint8_t)topic_len;
	memcpy(p, TOPIC, topic_len);
	p += topic_len;
	memcpy(p, publish_payload, OVERSIZED_PAYLOAD_LEN);
	p += OVERSIZED_PAYLOAD_LEN;

	publish_bytes_len = (size_t)(p - publish_bytes);
	zassert_true(publish_bytes_len <= sizeof(publish_bytes), "fixture size mismatch");
}

/* ---- Fake broker: accept once, then push CONNACK + the oversized
 * PUBLISH + a trailing PINGRESP, all queued before the client ever
 * asks -- this suite only cares about the client-side reaction to an
 * oversized message arriving, not real broker handshake timing. ---- */

static int broker_listen_sock = -1;

static void send_all(int sock, const uint8_t *data, size_t len)
{
	while (len > 0) {
		ssize_t n = zsock_send(sock, data, len, 0);

		zassert_true(n > 0, "broker send failed, errno=%d", errno);
		data += n;
		len -= (size_t)n;
	}
}

static void broker_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int client_sock = zsock_accept(broker_listen_sock, NULL, NULL);

	zassert_true(client_sock >= 0, "broker accept failed, errno=%d", errno);

	send_all(client_sock, connack_bytes, sizeof(connack_bytes));
	send_all(client_sock, publish_bytes, publish_bytes_len);
	send_all(client_sock, pingresp_bytes, sizeof(pingresp_bytes));

	/* Deliberately leaves client_sock open for the rest of the test --
	 * closing it early would race the client's later reads of the
	 * bytes already queued above. */
}

#define BROKER_STACK_SIZE 2048
K_THREAD_STACK_DEFINE(broker_stack, BROKER_STACK_SIZE);
static struct k_thread broker_thread_data;

/* ---- alp_mqtt_* msg callback under test --------------------------- */

struct msg_capture {
	int     count;
	char    topic[32];
	uint8_t payload[CONFIG_ALP_SDK_MQTT_BUF_SIZE];
	size_t  len;
};

static struct msg_capture g_capture;

static void on_msg(const char *topic, const uint8_t *payload, size_t len, void *user)
{
	struct msg_capture *cap = (struct msg_capture *)user;

	cap->count++;
	strncpy(cap->topic, topic, sizeof(cap->topic) - 1);
	memcpy(cap->payload, payload, MIN(len, sizeof(cap->payload)));
	cap->len = len;
}

ZTEST_SUITE(alp_mqtt_peer_length, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_mqtt_peer_length, test_oversized_publish_does_not_wedge_the_connection)
{
	build_publish_packet();
	memset(&g_capture, 0, sizeof(g_capture));

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port   = htons(BROKER_PORT),
	};
	zassert_equal(zsock_inet_pton(AF_INET, "127.0.0.1", &bind_addr.sin_addr), 1);

	broker_listen_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	zassert_true(broker_listen_sock >= 0, "broker socket() failed, errno=%d", errno);

	int reuse = 1;
	(void)zsock_setsockopt(broker_listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	zassert_equal(zsock_bind(broker_listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)),
	              0,
	              "broker bind() failed, errno=%d",
	              errno);
	zassert_equal(
	    zsock_listen(broker_listen_sock, 1), 0, "broker listen() failed, errno=%d", errno);

	k_thread_create(&broker_thread_data,
	                broker_stack,
	                BROKER_STACK_SIZE,
	                broker_thread_entry,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_COOP(7),
	                0,
	                K_NO_WAIT);

	alp_mqtt_t *m = alp_mqtt_open(&(alp_mqtt_config_t){
	    .broker_uri    = "mqtt://127.0.0.1:" STRINGIFY(BROKER_PORT),
	    .client_id     = "alp-1645-test",
	    .keepalive_s   = 60,
	    .clean_session = true,
	});
	zassert_not_null(m, "alp_mqtt_open failed, alp_last_error=%d", (int)alp_last_error());

	alp_status_t connect_rc = alp_mqtt_connect(m, 3000);
	zassert_equal(
	    connect_rc, ALP_OK, "alp_mqtt_connect did not reach CONNACK, rc=%d", (int)connect_rc);

	zassert_equal(alp_mqtt_subscribe(m, TOPIC, ALP_MQTT_QOS_0, on_msg, &g_capture), ALP_OK);

	/* First loop() call: processes the oversized PUBLISH already
	 * queued by the broker thread. Pre-fix and post-fix both dispatch
	 * the (correctly bounded) truncated payload here -- the defect is
	 * entirely in what happens AFTER this call. */
	alp_status_t rc1 = alp_mqtt_loop(m, 2000);
	zassert_equal(rc1, ALP_OK, "first alp_mqtt_loop() failed: %d", (int)rc1);

	zassert_equal(g_capture.count, 1, "msg_cb should have fired exactly once");
	zassert_equal(strcmp(g_capture.topic, TOPIC), 0);
	zassert_equal(g_capture.len,
	              CONFIG_ALP_SDK_MQTT_BUF_SIZE,
	              "delivered length should be bounded to rx_buf, got %zu",
	              g_capture.len);
	zassert_mem_equal(g_capture.payload,
	                  publish_payload,
	                  CONFIG_ALP_SDK_MQTT_BUF_SIZE,
	                  "delivered bytes should be the payload's own prefix");

	/* THE regression check.  Zephyr's mqtt_client tracks
	 * internal.remaining_payload; the broker's PINGRESP is already
	 * sitting on the wire right behind the 24 undrained payload
	 * bytes. Pre-fix, client_read() sees remaining_payload > 0 and
	 * returns -EBUSY WITHOUT EVER TOUCHING THE SOCKET -- the PINGRESP
	 * is never even looked at, and every future alp_mqtt_loop() call
	 * fails the exact same way: the connection is wedged permanently,
	 * not just delayed. Post-fix, the drain added to
	 * alp_mqtt_evt_cb() zeroes remaining_payload before this call
	 * ever runs, so mqtt_input() proceeds to parse the queued
	 * PINGRESP and returns 0. */
	alp_status_t rc2 = alp_mqtt_loop(m, 2000);
	zassert_equal(rc2,
	              ALP_OK,
	              "second alp_mqtt_loop() got %d (ALP_ERR_BUSY == %d) -- the connection is "
	              "wedged: remaining_payload was left > 0 after the oversized publish (#1645)",
	              (int)rc2,
	              (int)ALP_ERR_BUSY);

	alp_mqtt_close(m);
}
