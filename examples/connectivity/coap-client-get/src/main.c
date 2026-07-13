/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * coap-client-get -- build a CoAP GET request PDU offline, then parse a
 * canned CoAP response PDU offline. No socket, no net_context, no
 * coap_client -- this isolates the PDU codec half of CoAP (the part worth
 * exercising on native_sim) from the transport half (a real UDP socket --
 * see README.md).
 *
 * CoAP (RFC 7252) is the constrained-device REST transport this SDK ships
 * as the IN-TREE Zephyr subsystem `subsys/net/lib/coap/` (board.yaml's
 * `libraries: [coap]` -- see metadata/libraries/coap.yaml -- emits
 * CONFIG_NETWORKING=y + CONFIG_COAP=y; there is no separate west module to
 * fetch). This build never opens CONFIG_COAP_CLIENT's socket-based client
 * (that's an [EXPERIMENTAL] higher layer over net sockets) -- everything
 * here is the low-level `coap_packet_*` / `coap_header_*` codec API in
 * <zephyr/net/coap.h>, which needs only CONFIG_COAP.
 *
 * `coap_packet_init()` binds a `struct coap_packet` to a caller-owned byte
 * buffer and writes the fixed 4-byte CoAP header + token into it directly
 * (no separate "buffer" vs "PDU" object the way some CoAP libraries split
 * it) -- `coap_packet_append_option()` / `coap_packet_append_payload()`
 * then append into that same buffer in wire order. This example builds a
 * request that way and inspects it via the public getters
 * (`coap_header_get_code()`, `coap_header_get_token()`, `coap_find_options()`)
 * rather than reading `struct coap_packet`'s fields directly -- those are
 * documented as "CoAP lib maintains" internal bookkeeping, not something
 * app code should depend on.
 *
 * The response side works from real wire bytes -- `canned_response[]`
 * below is a hand-encoded RFC 7252 CoAP/UDP datagram (a 2.05 Content ACK,
 * echoing the request's token, carrying a tiny text payload), the same
 * shape a `recvfrom()` would hand you. `coap_packet_parse()` is the public
 * entry point for turning those bytes into a `struct coap_packet` -- no
 * live session required.
 *
 * What success looks like:
 *
 *   [coap-client-get] request: code=0x01 (GET), token=a1b2c3d4, Uri-Path="sensor"
 *   [coap-client-get] response: 14 bytes, code=0x45 (2.05 Content)
 *   [coap-client-get] payload: "23.5C"
 *   [coap-client-get] done
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/net/coap.h>

/* A GET on coap://<host>/sensor -- Uri-Path is the only option a minimal
 * request needs; host/port live in the (absent, here) transport layer, not
 * the PDU -- a real client adds COAP_OPTION_URI_HOST/_URI_PORT too when the
 * session's peer address doesn't already imply them. */
#define COAP_EXAMPLE_URI_PATH "sensor"

/* Working buffer coap_packet_init() writes the request header/token/options
 * into. 32 bytes is generous for a 4-byte header + 4-byte token + one short
 * Uri-Path option. */
#define REQUEST_BUF_SIZE 32

/* Fixed 4-byte token -- CoAP tokens correlate a response to its request
 * (like a request ID); a real client randomizes this per-request via
 * coap_next_token(). Fixed here so `canned_response[]` below can echo the
 * exact same bytes, the way a real server's ACK would. */
static const uint8_t kRequestToken[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };

/* A real 2.05-Content CoAP/UDP response, hand-encoded per RFC 7252 sec 3:
 *
 *   byte 0: Ver=1(0b01) | Type=ACK(0b10) | TKL=4      -> 0b01_10_0100 = 0x64
 *   byte 1: Code 2.05    -> (class 2 << 5) | detail 05 = 0x45
 *   bytes 2-3: Message ID 0x0001 (echoes the request's mid=1 below)
 *   bytes 4-7: Token 0xa1 0xb2 0xc3 0xd4 (echoes kRequestToken)
 *   byte 8: Payload marker 0xFF (no options on this response)
 *   bytes 9-13: Payload "23.5C" (ASCII, no Content-Format option -> the
 *               default text/plain;charset=utf-8 applies)
 *
 * This is exactly what recvfrom() would place in your socket buffer --
 * coap_packet_parse() below is the public way to decode it. */
static const uint8_t canned_response[] = {
	0x64, 0x45, 0x00, 0x01, 0xa1, 0xb2, 0xc3, 0xd4, 0xff, '2', '3', '.', '5', 'C',
};

/* Builds the GET request PDU and prints it via the PUBLIC getters -- no
 * struct field access, no wire-buffer send (that step needs a live socket;
 * see README.md). Returns 1 on success. */
static int build_and_print_get_request(void)
{
	static uint8_t     request_buf[REQUEST_BUF_SIZE];
	struct coap_packet request;

	/* ver=1, type=CON (server must ACK), tkl=4, code=GET, id=1 (a real
	 * client reads a fresh id from coap_next_id() per request). */
	int ret = coap_packet_init(&request,
	                           request_buf,
	                           sizeof(request_buf),
	                           COAP_VERSION_1,
	                           COAP_TYPE_CON,
	                           sizeof(kRequestToken),
	                           kRequestToken,
	                           COAP_METHOD_GET,
	                           1);

	if (ret < 0) {
		return 0;
	}

	/* Options must be appended in ascending option-number order (Uri-Path
	 * is the only one here, so ordering is moot) -- coap_packet_append_
	 * option() enforces that add-order internally via its delta encoding. */
	ret = coap_packet_append_option(&request,
	                                COAP_OPTION_URI_PATH,
	                                (const uint8_t *)COAP_EXAMPLE_URI_PATH,
	                                strlen(COAP_EXAMPLE_URI_PATH));
	if (ret < 0) {
		return 0;
	}

	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t tkl = coap_header_get_token(&request, token);

	printf("[coap-client-get] request: code=0x%02x (GET), token=", coap_header_get_code(&request));
	for (uint8_t i = 0; i < tkl; i++) {
		printf("%02x", token[i]);
	}

	/* coap_find_options() re-parses the packet's own option list -- the
	 * same call a request logger or a proxy re-targeting the request would
	 * make -- and hands back every Uri-Path option found (there's exactly
	 * one here; a real multi-segment path repeats this option). */
	struct coap_option opt;
	int                n_opts = coap_find_options(&request, COAP_OPTION_URI_PATH, &opt, 1);

	if (n_opts > 0) {
		printf(", Uri-Path=\"%.*s\"", opt.len, opt.value);
	}
	printf("\n");

	return 1;
}

/* Parses `canned_response[]` (bytes as if just handed back by recvfrom())
 * into a fresh `struct coap_packet` and prints the response code + payload.
 * coap_packet_parse() is the exact function a real client calls right after
 * recvfrom(); no live socket/session required for parsing. */
static int parse_canned_response(void)
{
	/* coap_packet_parse() writes through `data`, so it must be a mutable
	 * copy, not the `static const` wire bytes above. */
	static uint8_t     response_buf[sizeof(canned_response)];
	struct coap_option options[1];
	struct coap_packet response;

	memcpy(response_buf, canned_response, sizeof(canned_response));

	int ret = coap_packet_parse(
	    &response, response_buf, sizeof(response_buf), options, ARRAY_SIZE(options));

	if (ret < 0) {
		printf("[coap-client-get] response parse failed\n");
		return 0;
	}

	uint8_t code = coap_header_get_code(&response);

	printf("[coap-client-get] response: %u bytes, code=0x%02x (2.05 Content)\n",
	       (unsigned)sizeof(canned_response),
	       code);

	uint16_t       payload_len = 0;
	const uint8_t *payload     = coap_packet_get_payload(&response, &payload_len);

	if (payload_len > 0) {
		printf("[coap-client-get] payload: \"%.*s\"\n", (int)payload_len, payload);
	}

	return (code == COAP_RESPONSE_CODE_CONTENT) && (payload_len > 0);
}

int main(void)
{
	if (!build_and_print_get_request()) {
		printf("[coap-client-get] request build failed\n");
		return 1;
	}

	if (!parse_canned_response()) {
		printf("[coap-client-get] round trip MISMATCH\n");
		return 1;
	}

	printf("[coap-client-get] done\n");
	return 0;
}
