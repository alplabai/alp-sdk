/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * nanopb-encode-decode -- encode a small struct to a Protocol Buffers wire
 * blob with nanopb, then decode the blob back and assert the round trip.
 *
 * nanopb (https://github.com/nanopb/nanopb) is a small-code-size protobuf
 * implementation aimed at embedded targets: no heap by default (fields with
 * a bounded `max_size` become fixed-size struct members, not pointers),
 * ~2-10 KB of ROM depending on which encoders/decoders you pull in, and no
 * runtime schema reflection -- the wire format is fixed per-message at
 * compile time via a generated `<name>_msg` descriptor.
 *
 * `device_status.proto` is the schema (2 fields: an int32 + a bounded
 * string). `device_status.pb.h` / `device_status.pb.c` are its GENERATED
 * output -- checked in here because this build environment has no protoc.
 * See README.md for the exact command to regenerate them after editing the
 * .proto. Do not hand-edit the .pb.h/.pb.c -- they'd be silently clobbered
 * on the next `protoc --nanopb_out=...` run, same rule as any other
 * generated-file surface in this SDK.
 *
 * Why this matters for firmware: protobuf gives you a wire format that
 * tolerates schema drift (old firmware can skip fields it doesn't know,
 * new firmware can read old payloads) -- useful for OTA-versioned IPC
 * envelopes or cloud telemetry where the two ends don't always upgrade in
 * lockstep. alp-sdk's own <alp/mproc.h> IPC framing has a nanopb-shaped
 * design slot reserved for exactly this reason (see vendors/nanopb/).
 *
 * What success looks like:
 *
 *   [nanopb-encode-decode] encoded 14 bytes
 *   [nanopb-encode-decode] decoded uptime_s=42 device_id="e1m-aen801"
 *   [nanopb-encode-decode] round trip OK
 *   [nanopb-encode-decode] done
 */

#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "device_status.pb.h"

int main(void)
{
	/* The struct nanopb generated from device_status.proto.  Because
	 * `device_id` has a fixed `max_size` option, it's a plain
	 * `char[17]` member here -- no pb_callback_t, no heap, the whole
	 * struct is stack-allocatable and memcpy-safe. */
	DeviceStatus tx_msg = DeviceStatus_init_zero;

	tx_msg.uptime_s = 42;
	/* nanopb does NOT NUL-terminate for you on the encode side if you
	 * overrun -- strncpy + explicit terminator is the safe pattern for
	 * a bounded char[] field. */
	strncpy(tx_msg.device_id, "e1m-aen801", sizeof(tx_msg.device_id) - 1);
	tx_msg.device_id[sizeof(tx_msg.device_id) - 1] = '\0';

	/* pb_ostream_from_buffer() wraps a caller-owned buffer -- nanopb
	 * never allocates the output buffer itself, only tracks how far
	 * into it the encoder has written (stream.bytes_written). Sized
	 * from DeviceStatus_size, the generator's computed worst-case
	 * encoded length for this message. */
	uint8_t      wire_buf[DeviceStatus_size];
	pb_ostream_t ostream = pb_ostream_from_buffer(wire_buf, sizeof(wire_buf));

	/* `DeviceStatus_fields` is the generated field-descriptor table
	 * (`&DeviceStatus_msg`) that tells pb_encode() how to walk the
	 * struct -- field tags, wire types, offsets. This is the piece
	 * protoc-gen-nanopb computes for you; hand-rolling it is why the
	 * PB_BIND()-generated .pb.c exists instead of writing this by hand. */
	if (!pb_encode(&ostream, DeviceStatus_fields, &tx_msg)) {
		printf("[nanopb-encode-decode] encode failed: %s\n", PB_GET_ERROR(&ostream));
		return 1;
	}
	printf("[nanopb-encode-decode] encoded %u bytes\n", (unsigned)ostream.bytes_written);

	/* Decode straight back out of the same buffer into a FRESH struct --
	 * proves the wire bytes alone carry the message, not any leftover
	 * struct state. pb_istream_from_buffer() takes the encoder's actual
	 * bytes_written, not sizeof(wire_buf) -- protobuf messages are not
	 * self-delimiting by length prefix at this layer; the caller (or a
	 * framing layer) is responsible for knowing where the message ends. */
	DeviceStatus rx_msg  = DeviceStatus_init_zero;
	pb_istream_t istream = pb_istream_from_buffer(wire_buf, ostream.bytes_written);

	if (!pb_decode(&istream, DeviceStatus_fields, &rx_msg)) {
		printf("[nanopb-encode-decode] decode failed: %s\n", PB_GET_ERROR(&istream));
		return 1;
	}
	printf("[nanopb-encode-decode] decoded uptime_s=%d device_id=\"%s\"\n",
	       rx_msg.uptime_s,
	       rx_msg.device_id);

	/* The round-trip assertion a real firmware unit test would make --
	 * here it's a hard fail (not just a printed mismatch) so this
	 * example can never silently "pass" with wrong output. */
	if (rx_msg.uptime_s != tx_msg.uptime_s || strcmp(rx_msg.device_id, tx_msg.device_id) != 0) {
		printf("[nanopb-encode-decode] round trip MISMATCH\n");
		return 1;
	}
	printf("[nanopb-encode-decode] round trip OK\n");

	printf("[nanopb-encode-decode] done\n");
	return 0;
}
