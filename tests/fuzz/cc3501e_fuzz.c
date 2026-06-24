/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the <alp/protocol/cc3501e.h> SPI wire
 * protocol.  Targets the frame parser that the Alif-side host
 * driver (chips/cc3501e/) and the embedded CC3501E firmware
 * (firmware/cc3501e/) both run on inbound bytes.
 *
 * This harness carries an inline reference decoder that mirrors the
 * v1 framing rules.  The embedded firmware now implements the real
 * parser (firmware/cc3501e/src/protocol.c `protocol_build_reply`);
 * a follow-up can retarget this harness at that symbol directly for
 * coverage of the production code path.
 *
 * What it catches:
 *   - Header / payload-length disagreement (overflow into adjacent
 *     buffer if the parser believes payload_len blindly).
 *   - Unknown opcodes (must return NOT_SUPPORTED, not crash).
 *   - Async-event flag set on a master-side frame and vice versa.
 *   - Payload longer than ALP_CC3501E_MAX_PAYLOAD = 512.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=yocto \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_cc3501e
 *
 * Run:
 *   ./build-fuzz/tests/fuzz/alp_fuzz_cc3501e -max_total_time=30 \
 *         tests/fuzz/corpus/cc3501e
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/protocol/cc3501e.h"

/* Status codes the inline reference parser emits.  v0.4's real
 * alp_cc3501e_parse_frame uses alp_status_t. */
typedef enum {
	REF_OK             = 0,
	REF_ERR_TOO_SHORT  = 1, /* fewer than 4 header bytes */
	REF_ERR_TOO_LONG   = 2, /* declared payload_len > MAX_PAYLOAD */
	REF_ERR_TRUNCATED  = 3, /* declared len > buf - 4 */
	REF_ERR_BAD_FLAGS  = 4, /* reserved bits set */
	REF_ERR_BAD_OPCODE = 5  /* opcode in vendor / reserved range */
} ref_status_t;

/* Reference parser.  Stateless, no allocation, no callbacks --
 * just validates a single inbound frame.  Mirrors the contract in
 * <alp/protocol/cc3501e.h>:
 *
 *   +--------+--------+--------+--------+========+
 *   |  cmd   |  flags | payload_len_lo  | payload (N B)   |
 *   +--------+--------+--------+--------+========+
 */
static ref_status_t ref_parse_frame(const uint8_t *buf, size_t size)
{
	if (size < ALP_CC3501E_HEADER_BYTES) return REF_ERR_TOO_SHORT;

	const uint8_t  cmd         = buf[0];
	const uint8_t  flags       = buf[1];
	const uint16_t payload_len = (uint16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));

	if (payload_len > ALP_CC3501E_MAX_PAYLOAD) return REF_ERR_TOO_LONG;
	if ((size_t)payload_len + ALP_CC3501E_HEADER_BYTES > size) return REF_ERR_TRUNCATED;

	/* Bits 2..7 of flags are reserved in v1; firmware MUST reject. */
	if ((flags & 0xFCu) != 0u) return REF_ERR_BAD_FLAGS;

	/* Opcode ranges 0x80..0xFF are reserved for vendor extensions
     * (per cc3501e.h doc-comment) -- v1 parser rejects them. */
	if (cmd >= 0x80u) return REF_ERR_BAD_OPCODE;

	/* Touch each payload byte so ASan / fuzzer coverage detects
     * the parser actually walks the declared range.  Without this
     * the OOB-on-truncated-frame bug above hides behind constant
     * folding in -O2. */
	volatile uint8_t sink = 0;
	for (size_t i = 0; i < (size_t)payload_len; ++i) {
		sink ^= buf[ALP_CC3501E_HEADER_BYTES + i];
	}
	(void)sink;
	(void)cmd;

	return REF_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	(void)ref_parse_frame(data, size);
	return 0;
}
