/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Links the west-pinned AWS IoT (coreMQTT + coreJSON) and Azure IoT
 * (az_core + hub client) sources through the vendors/ glue and calls one
 * pure function from each.  No network, no TLS.
 */

#include <zephyr/ztest.h>

#include <azure/core/az_span.h>
#include <azure/iot/az_iot_hub_client.h>
#include <core_json.h>
#include <core_mqtt_serializer.h>

ZTEST(cloud_sdks, test_aws_corejson_validate)
{
	static const char doc[] = "{\"a\":1}";

	zassert_equal(JSON_Validate(doc, sizeof(doc) - 1), JSONSuccess);
	zassert_equal(JSON_Validate("{", 1), JSONPartial);
}

ZTEST(cloud_sdks, test_aws_coremqtt_serializer)
{
	size_t packet;

	/* PINGREQ is a fixed 2-byte packet. */
	zassert_equal(MQTT_GetPingreqPacketSize(&packet), MQTTSuccess);
	zassert_equal(packet, 2);
	zassert_equal(MQTT_GetIncomingPacketTypeAndLength(NULL, NULL, NULL), MQTTBadParameter);
}

ZTEST(cloud_sdks, test_azure_span_and_hub_client)
{
	az_span s = AZ_SPAN_FROM_STR("hub.azure-devices.net");
	az_iot_hub_client client;
	char topic[64];
	size_t len;

	zassert_equal(az_span_size(s), 21);
	zassert_true(az_result_succeeded(az_iot_hub_client_init(
		&client, s, AZ_SPAN_FROM_STR("dev1"), NULL)));
	zassert_true(az_result_succeeded(az_iot_hub_client_telemetry_get_publish_topic(
		&client, NULL, topic, sizeof(topic), &len)));
	zassert_str_equal(topic, "devices/dev1/messages/events/");
}

ZTEST_SUITE(cloud_sdks, NULL, NULL, NULL, NULL, NULL);
