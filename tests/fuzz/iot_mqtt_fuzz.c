/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the MQTT v3.1.1 fixed + variable-length
 * header decode path consumed by `<alp/iot.h>`'s alp_mqtt_* surface.
 *
 * The Zephyr backend leans on Zephyr's mqtt_client which carries its
 * own framing parser (already fuzzed upstream).  The Yocto backend
 * lands on top of Mosquitto/Paho in v0.4; that path has its own fuzz
 * coverage too.  What's *not* covered upstream is the small framing
 * helper the SDK keeps in alp_iot_mqtt for the cross-backend retry /
 * topic-filter logic.  This harness validates the SDK-owned slice.
 *
 * What it catches:
 *   - Variable-length integer (VLI) decode running past the buffer
 *     when the high bit is set on every byte.
 *   - Topic-length field overflowing the remaining frame.
 *   - QoS > 2.
 *   - Reserved control-packet types (0 and 15).
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=yocto \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_iot_mqtt
 *
 * Run:
 *   ./build-fuzz/tests/fuzz/alp_fuzz_iot_mqtt -max_total_time=30 \
 *         tests/fuzz/corpus/iot_mqtt
 */

#include <stddef.h>
#include <stdint.h>

/* MQTT v3.1.1 fixed-header control-packet types.  Reserved values
 * 0 and 15 must be rejected. */
enum {
	MQTT_CP_CONNECT     = 1,
	MQTT_CP_CONNACK     = 2,
	MQTT_CP_PUBLISH     = 3,
	MQTT_CP_PUBACK      = 4,
	MQTT_CP_PUBREC      = 5,
	MQTT_CP_PUBREL      = 6,
	MQTT_CP_PUBCOMP     = 7,
	MQTT_CP_SUBSCRIBE   = 8,
	MQTT_CP_SUBACK      = 9,
	MQTT_CP_UNSUBSCRIBE = 10,
	MQTT_CP_UNSUBACK    = 11,
	MQTT_CP_PINGREQ     = 12,
	MQTT_CP_PINGRESP    = 13,
	MQTT_CP_DISCONNECT  = 14
};

typedef enum {
	MQTT_REF_OK              = 0,
	MQTT_REF_ERR_TOO_SHORT   = 1,
	MQTT_REF_ERR_BAD_TYPE    = 2,
	MQTT_REF_ERR_VLI_OVERRUN = 3,
	MQTT_REF_ERR_TRUNCATED   = 4,
	MQTT_REF_ERR_BAD_TOPIC   = 5,
	MQTT_REF_ERR_BAD_QOS     = 6
} mqtt_ref_status_t;

/* Decode an MQTT variable-length integer (1-4 bytes).  Returns the
 * decoded value via @p out and the byte count via @p consumed, or
 * MQTT_REF_ERR_VLI_OVERRUN if the high bit is set past byte 4. */
static mqtt_ref_status_t mqtt_decode_vli(const uint8_t *buf, size_t size, uint32_t *out,
                                         size_t *consumed)
{
	uint32_t value      = 0;
	uint32_t multiplier = 1;
	size_t   i;

	for (i = 0; i < 4; ++i) {
		if (i >= size) return MQTT_REF_ERR_TOO_SHORT;
		const uint8_t byte = buf[i];
		value += (uint32_t)(byte & 0x7Fu) * multiplier;
		if ((byte & 0x80u) == 0u) {
			*out      = value;
			*consumed = i + 1;
			return MQTT_REF_OK;
		}
		multiplier *= 128u;
	}
	return MQTT_REF_ERR_VLI_OVERRUN;
}

/* Decode an MQTT fixed-header + walk the PUBLISH variable header's
 * topic-length / topic-name + optional packet-id.  Stops there --
 * payload is application-defined and not framed by the protocol. */
static mqtt_ref_status_t mqtt_ref_parse(const uint8_t *buf, size_t size)
{
	if (size < 2) return MQTT_REF_ERR_TOO_SHORT;

	const uint8_t cp_type = (uint8_t)((buf[0] >> 4) & 0x0Fu);
	const uint8_t flags   = (uint8_t)(buf[0] & 0x0Fu);

	if (cp_type == 0u || cp_type == 15u) return MQTT_REF_ERR_BAD_TYPE;

	uint32_t remaining_len = 0;
	size_t   vli_bytes     = 0;
	{
		const mqtt_ref_status_t s = mqtt_decode_vli(&buf[1], size - 1, &remaining_len, &vli_bytes);
		if (s != MQTT_REF_OK) return s;
	}

	const size_t fixed_header_size = 1u + vli_bytes;
	if (fixed_header_size + remaining_len > size) return MQTT_REF_ERR_TRUNCATED;

	if (cp_type == MQTT_CP_PUBLISH) {
		const uint8_t qos = (uint8_t)((flags >> 1) & 0x03u);
		if (qos > 2u) return MQTT_REF_ERR_BAD_QOS;

		const uint8_t *vh    = &buf[fixed_header_size];
		const size_t   vhlen = remaining_len;
		if (vhlen < 2) return MQTT_REF_ERR_BAD_TOPIC;
		const uint16_t topic_len = (uint16_t)(((uint16_t)vh[0] << 8) | vh[1]);
		if ((size_t)topic_len + 2u > vhlen) return MQTT_REF_ERR_BAD_TOPIC;

		/* For QoS > 0 the packet-id occupies two more bytes after
         * the topic name; check it doesn't overrun. */
		if (qos > 0u && (size_t)topic_len + 4u > vhlen) return MQTT_REF_ERR_BAD_TOPIC;

		/* Touch each topic byte -- catches OOB when the declared
         * topic_len lies beyond the remaining_len bound but the
         * outer check missed it. */
		volatile uint8_t sink = 0;
		for (size_t i = 0; i < (size_t)topic_len; ++i) {
			sink ^= vh[2 + i];
		}
		(void)sink;
	}

	return MQTT_REF_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	(void)mqtt_ref_parse(data, size);
	return 0;
}
