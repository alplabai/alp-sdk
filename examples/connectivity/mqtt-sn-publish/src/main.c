/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mqtt-sn-publish -- coreMQTT-SN PUBLISH serialize/parse teaching
 * example.
 *
 * What this is supposed to show: build an MQTT-SN PUBLISH packet with
 * coreMQTT-SN's serializer, then parse it straight back out of the
 * same buffer and assert the topic id + payload survive the round
 * trip -- no socket, no gateway, no live transport.  That is exactly
 * how you'd unit-test a wire encoder before wiring it to UDP, BLE, or
 * a UART framing layer.
 *
 * [UNTESTED]: this workspace's pinned `modules/lib/coremqtt_sn`
 * checkout (west.yml's `freertos-org/coreMQTT-SN`, `extras-tier1`
 * group) is EMPTY -- `git log` on it shows an orphan `init_placeholder`
 * branch with zero commits fetched, i.e. the module fetch/checkout
 * failed on this machine.  There is therefore no
 * `core_mqttsn_serializer.h` on disk to `#include`, and no way to
 * build-verify the real library's call shape here.  A workspace where
 * `west update --group-filter +extras-tier1` succeeded would have the
 * real header; this file intentionally does NOT guess at its exact
 * path or symbol names in compiled code, to avoid presenting
 * unverified identifiers as fact.
 *
 * What we do instead: implement the identical OASIS MQTT-SN v1.2
 * PUBLISH wire format by hand (mqttsn_wire_encode_publish() /
 * mqttsn_wire_decode_publish() below) and round-trip through it.  The
 * on-wire bytes this produces are the same bytes coreMQTT-SN's
 * MQTTSN_SerializePublish()-shaped API would produce (coreMQTT-SN
 * mirrors AWS's sibling coreMQTT library's naming convention --
 * `core_mqttsn_serializer.h`, `MQTTSNStatus_t` returns, an
 * `MQTTSNPublishInfo_t` describing topic id/qos/retain/payload -- see
 * https://github.com/FreeRTOS/coreMQTT-SN) so the wire-format
 * teaching below stays useful; only the two function names below are
 * this example's stand-in, not upstream's.
 *
 * Swapping to the real library once fetched: replace the two
 * mqttsn_wire_*() calls in main() with coreMQTT-SN's
 * MQTTSN_SerializePublish() / MQTTSN_GetPublishPacketSize() to encode
 * and MQTTSN_DeserializePublish() to decode, populating an
 * MQTTSNPublishInfo_t from the same topic_id/msg_id/qos/retain/
 * payload locals used here.  Confirm exact struct field names and
 * signatures against the fetched `modules/lib/coremqtt_sn/source/
 * include` headers -- they are not verified against a local checkout
 * in this environment.
 *
 * What success looks like:
 *
 *   [mqtt-sn-publish] encoding PUBLISH: topic_id=0x0001 msg_id=0x0007 "23.5C"
 *   [mqtt-sn-publish] encoded 12 bytes
 *   [mqtt-sn-publish] decoded: topic_id=0x0001 msg_id=0x0007 qos=0 retain=0 payload="23.5C"
 *   [mqtt-sn-publish] round trip OK
 *   [mqtt-sn-publish] done
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <alp/peripheral.h>

/* MQTT-SN v1.2 message type octet for PUBLISH (OASIS MQTT-SN v1.2
 * spec, section 5.4.12 / Table 2). */
#define MQTTSN_MSG_TYPE_PUBLISH 0x0Cu

/* TopicIdType values packed into the low 2 bits of the Flags octet.
 * This example always publishes to a PREDEFINED topic id -- the
 * MQTT-SN feature that replaces MQTT's string topic names with a
 * 2-byte id both sides already agree on, which is the whole point of
 * running MQTT on constrained links. */
#define MQTTSN_TOPIC_TYPE_PREDEFINED 0x01u

/* Wire layout this function produces (OASIS MQTT-SN v1.2, Table 6):
 *
 *   byte 0      Length   -- total packet length, this example never
 *                            needs the 3-byte extended-length form
 *                            (Length[0] == 0x01), so 1 byte is enough
 *                            for payloads under ~248 bytes.
 *   byte 1      MsgType  -- 0x0C (PUBLISH)
 *   byte 2      Flags    -- DUP(1) | QoS(2) | Retain(1) | Will(1) |
 *                            CleanSession(1) | TopicIdType(2)
 *   byte 3-4    TopicId  -- big-endian
 *   byte 5-6    MsgId    -- big-endian
 *   byte 7..    Data     -- the payload, unmodified bytes
 *
 * Returns the number of bytes written, or 0 if buf_len is too small
 * or the packet would exceed the 1-byte Length encoding's 255-byte
 * ceiling. */
static size_t mqttsn_wire_encode_publish(uint8_t       *buf,
                                         size_t         buf_len,
                                         uint16_t       topic_id,
                                         uint16_t       msg_id,
                                         uint8_t        qos,
                                         bool           retain,
                                         const uint8_t *payload,
                                         size_t         payload_len)
{
	size_t total = 7u + payload_len;

	if (total > buf_len || total > 255u) {
		return 0;
	}

	buf[0] = (uint8_t)total;
	buf[1] = MQTTSN_MSG_TYPE_PUBLISH;
	/* QoS lives in bits 6-5; 0/1/2 map straight into that field for
	 * the QoS values this example cares about (QoS -1 would need
	 * 0b11, out of scope here). Retain is bit 4; DUP/Will/
	 * CleanSession are 0 for a fresh, non-retried PUBLISH. */
	buf[2] =
	    (uint8_t)(((qos & 0x3u) << 5) | (retain ? (1u << 4) : 0u) | MQTTSN_TOPIC_TYPE_PREDEFINED);
	buf[3] = (uint8_t)(topic_id >> 8);
	buf[4] = (uint8_t)(topic_id & 0xFFu);
	buf[5] = (uint8_t)(msg_id >> 8);
	buf[6] = (uint8_t)(msg_id & 0xFFu);
	memcpy(&buf[7], payload, payload_len);

	return total;
}

/* Inverse of mqttsn_wire_encode_publish() -- parses a buffer in place
 * (no copy) and points *payload_out at the payload bytes inside buf.
 * Returns false if the buffer is too short or the message type isn't
 * PUBLISH; both are the same "reject, don't guess" posture a real
 * deserializer takes on malformed input. */
static bool mqttsn_wire_decode_publish(const uint8_t  *buf,
                                       size_t          buf_len,
                                       uint16_t       *topic_id_out,
                                       uint16_t       *msg_id_out,
                                       uint8_t        *qos_out,
                                       bool           *retain_out,
                                       const uint8_t **payload_out,
                                       size_t         *payload_len_out)
{
	size_t total;

	if (buf_len < 7u) {
		return false;
	}

	total = buf[0];
	if (total > buf_len || total < 7u) {
		return false;
	}
	if (buf[1] != MQTTSN_MSG_TYPE_PUBLISH) {
		return false;
	}

	*qos_out         = (uint8_t)((buf[2] >> 5) & 0x3u);
	*retain_out      = ((buf[2] >> 4) & 0x1u) != 0u;
	*topic_id_out    = (uint16_t)((buf[3] << 8) | buf[4]);
	*msg_id_out      = (uint16_t)((buf[5] << 8) | buf[6]);
	*payload_out     = &buf[7];
	*payload_len_out = total - 7u;

	return true;
}

int main(void)
{
	/* A predefined topic id + a short telemetry payload -- the
	 * shape of a sensor node's periodic MQTT-SN PUBLISH on a
	 * constrained link (BLE, Sub-GHz, or a UART bridge instead of
	 * MQTT's usual TCP). */
	static const uint16_t topic_id    = 0x0001u;
	static const uint16_t msg_id      = 0x0007u;
	static const uint8_t  qos         = 0u;
	static const bool     retain      = false;
	static const uint8_t  payload[]   = "23.5C";
	static const size_t   payload_len = sizeof(payload) - 1u; /* drop NUL */

	uint8_t        buf[32];
	size_t         encoded_len;
	uint16_t       decoded_topic_id;
	uint16_t       decoded_msg_id;
	uint8_t        decoded_qos;
	bool           decoded_retain;
	const uint8_t *decoded_payload;
	size_t         decoded_payload_len;
	bool           ok;

	(void)alp_init();

	printf("[mqtt-sn-publish] encoding PUBLISH: topic_id=0x%04x msg_id=0x%04x \"%s\"\n",
	       topic_id,
	       msg_id,
	       payload);

	encoded_len = mqttsn_wire_encode_publish(
	    buf, sizeof(buf), topic_id, msg_id, qos, retain, payload, payload_len);
	if (encoded_len == 0u) {
		printf("[mqtt-sn-publish] encode failed (buffer too small)\n");
		printf("[mqtt-sn-publish] done\n");
		return 0;
	}
	printf("[mqtt-sn-publish] encoded %u bytes\n", (unsigned int)encoded_len);

	ok = mqttsn_wire_decode_publish(buf,
	                                encoded_len,
	                                &decoded_topic_id,
	                                &decoded_msg_id,
	                                &decoded_qos,
	                                &decoded_retain,
	                                &decoded_payload,
	                                &decoded_payload_len);
	if (!ok) {
		printf("[mqtt-sn-publish] decode failed\n");
		printf("[mqtt-sn-publish] done\n");
		return 0;
	}

	/* Printed payload isn't NUL-terminated in the buffer (it's raw
	 * wire bytes) -- %.*s bounds the print to decoded_payload_len
	 * instead of relying on a terminator that may not exist. */
	printf("[mqtt-sn-publish] decoded: topic_id=0x%04x msg_id=0x%04x qos=%u retain=%u "
	       "payload=\"%.*s\"\n",
	       decoded_topic_id,
	       decoded_msg_id,
	       (unsigned int)decoded_qos,
	       (unsigned int)decoded_retain,
	       (int)decoded_payload_len,
	       decoded_payload);

	if (decoded_topic_id == topic_id && decoded_msg_id == msg_id && decoded_qos == qos &&
	    decoded_retain == retain && decoded_payload_len == payload_len &&
	    memcmp(decoded_payload, payload, payload_len) == 0) {
		printf("[mqtt-sn-publish] round trip OK\n");
	} else {
		printf("[mqtt-sn-publish] round trip MISMATCH\n");
	}

	printf("[mqtt-sn-publish] done\n");
	return 0;
}
