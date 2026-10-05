/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The coreMQTT TransportInterface_t adapter, driven by a real MQTTContext_t
 * over plain-TCP loopback.  The test plays the broker by hand: it reads the
 * PINGREQ coreMQTT sends and answers with a PINGRESP.
 */

#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include <core_mqtt.h>

#include "alp_cloud_transport_coremqtt.h"
#include "peer.h"

#define PORT_COREMQTT 4301

static const uint8_t PINGREQ[]  = { 0xC0, 0x00 };
static const uint8_t PINGRESP[] = { 0xD0, 0x00 };

static uint32_t now_ms(void)
{
	return k_uptime_get_32();
}

static void on_event(MQTTContext_t *ctx, MQTTPacketInfo_t *packet, MQTTDeserializedInfo_t *info)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(packet);
	ARG_UNUSED(info);
}

ZTEST(cloud_transport_coremqtt, test_ping_round_trip)
{
	static uint8_t               net_buf[128];
	uint8_t                      got[sizeof(PINGREQ)];
	TransportInterface_t         iface;
	NetworkContext_t             net_ctx;
	MQTTContext_t                mqtt;
	MQTTFixedBuffer_t            fixed = { .pBuffer = net_buf, .size = sizeof(net_buf) };
	alp_cloud_transport_t        t     = ALP_CLOUD_TRANSPORT_INIT;
	alp_cloud_transport_config_t cfg   = {
		.host            = PEER_HOST,
		.port            = PORT_COREMQTT,
		.plaintext       = true,
		.recv_timeout_ms = 50,
	};
	int lfd = peer_listen(PORT_COREMQTT);

	zassert_ok(alp_cloud_transport_connect(&t, &cfg));
	int broker = peer_accept(lfd);

	alp_cloud_transport_coremqtt(&iface, &net_ctx, &t);
	zassert_is_null(iface.writev, "coreMQTT must fall back to send");
	zassert_equal(MQTT_Init(&mqtt, &iface, now_ms, on_event, &fixed), MQTTSuccess);

	/* MQTT_Ping does not require a completed CONNECT; it only serialises
	 * and sends, which is exactly the path under test. */
	mqtt.connectStatus = MQTTConnected;
	zassert_equal(MQTT_Ping(&mqtt), MQTTSuccess);
	zassert_equal(zsock_recv(broker, got, sizeof(got), 0), sizeof(got));
	zassert_mem_equal(got, PINGREQ, sizeof(PINGREQ));
	zassert_true(mqtt.waitingForPingResp);

	/* Nothing from the broker yet: the adapter reports "no data" (0), which
	 * coreMQTT turns into a clean pass with the ping still outstanding --
	 * not an error, and not a spin. */
	zassert_equal(MQTT_ProcessLoop(&mqtt), MQTTSuccess);
	zassert_true(mqtt.waitingForPingResp);

	zassert_equal(zsock_send(broker, PINGRESP, sizeof(PINGRESP), 0), sizeof(PINGRESP));
	/* coreMQTT consumes a PINGRESP itself (no application callback) and
	 * clears the outstanding-ping flag: proof the bytes came through recv. */
	zassert_equal(MQTT_ProcessLoop(&mqtt), MQTTSuccess);
	zassert_false(mqtt.waitingForPingResp);

	/* Broker goes away: the adapter must surface it, so coreMQTT stops. */
	zsock_close(broker);
	zassert_equal(MQTT_ProcessLoop(&mqtt), MQTTRecvFailed);

	alp_cloud_transport_close(&t);
	zsock_close(lfd);
}

ZTEST(cloud_transport_coremqtt, test_bind_tolerates_null)
{
	TransportInterface_t iface = { 0 };
	NetworkContext_t     ctx   = { 0 };

	alp_cloud_transport_coremqtt(NULL, &ctx, NULL);
	alp_cloud_transport_coremqtt(&iface, NULL, NULL);
	zassert_is_null(iface.recv);
}

ZTEST_SUITE(cloud_transport_coremqtt, NULL, NULL, NULL, NULL, NULL);
