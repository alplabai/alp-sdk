/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * coap-client-get -- build a CoAP GET request PDU offline, then parse a
 * canned CoAP response PDU offline. No socket, no coap_context_t, no
 * coap_session_t -- this isolates the PDU codec half of libcoap (the part
 * worth exercising on native_sim) from the transport half (a real UDP
 * socket / DTLS session -- see README.md).
 *
 * libcoap (https://github.com/obgm/libcoap) is the full-featured CoAP
 * (RFC 7252) implementation most embedded Linux/RTOS CoAP clients and
 * servers are built on -- client + server, UDP/TCP/WS, optional DTLS via
 * mbedTLS/OpenSSL/etc. This build enables client-only, no-TLS (see
 * board.yaml + prj.conf: CONFIG_LIBCOAP_CLIENT_SUPPORT=y, DTLS backends
 * off), matching metadata/library-profiles/libcoap/hw-backends.yaml's
 * `sw_fallback: CONFIG_ALP_COAP_NO_TLS=y` floor.
 *
 * `coap_pdu_init()` gives you a `coap_pdu_t` with its OWN internal byte
 * buffer already reserved (the `size` argument) -- `coap_add_token()` /
 * `coap_add_option()` / `coap_add_data()` lay the token, options and
 * payload into that buffer in wire order. This example builds a PDU that
 * way and inspects it via the public getters (`coap_pdu_get_code()`,
 * `coap_pdu_get_token()`, the `coap_option_iterator_t` walk) rather than
 * poking at the struct directly -- `coap_pdu_t`'s fields are declared in
 * `coap_pdu_internal.h`, explicitly `@ingroup internal_api` upstream, and
 * not something app code should depend on (session-bound sending is what
 * turns this buffer into wire bytes for a real socket; see README.md).
 *
 * The response side works from real wire bytes -- `canned_response[]`
 * below is a hand-encoded RFC 7252 CoAP/UDP datagram (a 2.05 Content ACK,
 * echoing the request's token, carrying a tiny text payload), the same
 * shape a `recvfrom()` would hand you. `coap_pdu_parse()` is the public,
 * session-free entry point for turning those bytes into a `coap_pdu_t`.
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

#include <coap3/coap.h>

/* A GET on coap://<host>/sensor -- Uri-Path is the only option a minimal
 * request needs; host/port live in the (absent, here) transport layer, not
 * the PDU -- a real client adds COAP_OPTION_URI_HOST/_URI_PORT too when the
 * session's peer address doesn't already imply them. */
#define COAP_EXAMPLE_URI_PATH "sensor"

/* Fixed 4-byte token -- CoAP tokens correlate a response to its request
 * (like a request ID); a real client randomizes this per-request via
 * coap_session_new_token(). Fixed here so `canned_response[]` below can
 * echo the exact same bytes, the way a real server's ACK would. */
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
 * coap_pdu_parse() below is the public, session-free way to decode it. */
static const uint8_t canned_response[] = {
	0x64, 0x45, 0x00, 0x01, 0xa1, 0xb2, 0xc3, 0xd4, 0xff, '2', '3', '.', '5', 'C',
};

/* Builds the GET request PDU and prints it via the PUBLIC getters -- no
 * struct field access, no wire-buffer serialization (that step needs a
 * coap_session_t; see README.md). Returns 1 on success. */
static int build_and_print_get_request(void)
{
	/* type=CON (server must ACK), code=GET, mid=1 (a real client reads
	 * this from coap_new_message_id_lkd() on a live session), size=64
	 * (this PDU's own working-buffer reservation for token+options+
	 * payload -- unrelated to the wire-header space discussed above). */
	coap_pdu_t *pdu = coap_pdu_init(COAP_MESSAGE_CON, COAP_REQUEST_CODE_GET, 1, 64);

	if (!pdu) {
		return 0;
	}

	/* Token, then options in ascending option-number order (Uri-Path is
	 * the only one here, so ordering is moot) -- libcoap enforces that
	 * add-order at the API level (options after payload data is a
	 * documented failure case). */
	if (!coap_add_token(pdu, sizeof(kRequestToken), kRequestToken)) {
		coap_delete_pdu(pdu);
		return 0;
	}
	if (!coap_add_option(pdu,
	                     COAP_OPTION_URI_PATH,
	                     strlen(COAP_EXAMPLE_URI_PATH),
	                     (const uint8_t *)COAP_EXAMPLE_URI_PATH)) {
		coap_delete_pdu(pdu);
		return 0;
	}

	coap_bin_const_t token = coap_pdu_get_token(pdu);

	printf("[coap-client-get] request: code=0x%02x (GET), token=", coap_pdu_get_code(pdu));
	for (size_t i = 0; i < token.length; i++) {
		printf("%02x", token.s[i]);
	}

	/* Walk every option (COAP_OPT_ALL) the same way a request logger or
	 * a proxy re-targeting the request would -- coap_option_next()
	 * returns NULL once the option list is exhausted. */
	coap_opt_iterator_t opt_iter;

	coap_option_iterator_init(pdu, &opt_iter, COAP_OPT_ALL);
	coap_opt_t *opt;

	while ((opt = coap_option_next(&opt_iter))) {
		if (opt_iter.number == COAP_OPTION_URI_PATH) {
			printf(", Uri-Path=\"%.*s\"", (int)coap_opt_length(opt), coap_opt_value(opt));
		}
	}
	printf("\n");

	coap_delete_pdu(pdu);
	return 1;
}

/* Parses `canned_response[]` (bytes as if just handed back by recvfrom())
 * into a fresh PDU and prints the response code + payload. coap_pdu_parse()
 * is the exact function a real client calls right after recvfrom(); no
 * coap_session_t required for parsing. */
static int parse_canned_response(void)
{
	coap_pdu_t *pdu = coap_pdu_init(COAP_MESSAGE_ACK, 0, 0, sizeof(canned_response));

	if (!pdu) {
		return 0;
	}
	if (!coap_pdu_parse(COAP_PROTO_UDP, canned_response, sizeof(canned_response), pdu)) {
		printf("[coap-client-get] response parse failed\n");
		coap_delete_pdu(pdu);
		return 0;
	}

	coap_pdu_code_t code = coap_pdu_get_code(pdu);

	printf("[coap-client-get] response: %u bytes, code=0x%02x (2.05 Content)\n",
	       (unsigned)sizeof(canned_response),
	       code);

	size_t         data_len  = 0;
	const uint8_t *data      = NULL;
	int            have_data = coap_get_data(pdu, &data_len, &data);

	if (have_data) {
		printf("[coap-client-get] payload: \"%.*s\"\n", (int)data_len, data);
	}

	int ok = (code == COAP_RESPONSE_CODE_CONTENT) && have_data;

	coap_delete_pdu(pdu);
	return ok;
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
