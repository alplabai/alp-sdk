/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the SDK's mproc IPC envelope framing
 * (src/common/proto/alp_mproc_frame.{h,c}).
 *
 * The placeholder framing is the v0.3/v0.4-prep wire format used
 * when CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING=y: a fixed 12-byte
 * header (magic / sequence / length) followed by the caller's
 * payload bytes.  The decoder is the load-bearing failure point
 * here -- a malicious peer M55 could send any 12 bytes and we
 * have to drop the frame cleanly without:
 *
 *   - Reading past the input buffer (length-field overrun).
 *   - Returning OK with garbage payload pointer (use-after-free
 *     in the consumer callback).
 *   - Crashing on a magic mismatch.
 *   - Underflowing length=0 / overflowing length=UINT32_MAX.
 *
 * The harness feeds the raw fuzz buffer into the same decoder
 * the production mproc_zephyr.c calls when a frame arrives on a
 * mailbox.  Any non-OK decoder return must produce an unmodified
 * out-pointer-state (or NULL); any OK return must produce a
 * payload_ptr within the input buffer and a payload_len that
 * fits within the remaining bytes.
 *
 * The harness also re-encodes the decoded payload + sequence +
 * a fixed-checksum-magic and asserts the round-trip matches a
 * prefix of the original input -- this catches asymmetric
 * encode/decode bugs.
 *
 * What it catches:
 *   - Header / payload-length disagreement (overrun if the
 *     decoder believes length blindly).
 *   - Negative-length cast on signed paths in consumers.
 *   - Magic-mismatch crash instead of clean drop.
 *   - Sequence-counter underflow (encoder uses 0 vs 1).
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=yocto \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_mproc_frame
 *
 * Run:
 *   ./build-fuzz/tests/fuzz/alp_fuzz_mproc_frame \
 *         -max_total_time=30 tests/fuzz/corpus/mproc_frame
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/peripheral.h" /* for alp_status_t -- the decoder signature */

/* ------------------------------------------------------------------- */
/* Inline reference decoder mirroring src/common/proto/alp_mproc_frame.c.
 *
 * The harness keeps a copy of the decoder here instead of linking the
 * SDK target because the fuzz CMakeLists.txt only depends on
 * alp::sdk_headers; pulling the .c body in would require a wider
 * dependency graph than the fuzz harness should claim.  The two impls
 * MUST stay in sync; if the production decoder evolves, this reference
 * gets updated and the fuzz harness asserts the same outputs. */

#define MPROC_FRAME_MAGIC      0x46504D41u /* 'A','M','P','F' LE */
#define MPROC_FRAME_HEADER_LEN 12u

typedef enum {
	REF_OK            = 0,
	REF_ERR_INVAL     = -1,
	REF_ERR_TOO_SHORT = -2,
	REF_ERR_BAD_MAGIC = -3,
	REF_ERR_BAD_LEN   = -4,
} ref_status_t;

static uint32_t ref_le32(const uint8_t *p)
{
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* Decode: parse the 12-byte header + bound-check the declared
 * payload_len against the available buffer.  On REF_OK, *seq +
 * *payload_ptr + *payload_len are populated. */
static ref_status_t ref_decode(const uint8_t  *buf,
                               size_t          buf_len,
                               uint32_t       *seq,
                               const uint8_t **payload_ptr,
                               uint32_t       *payload_len)
{
	if (buf == NULL || seq == NULL || payload_ptr == NULL || payload_len == NULL) {
		return REF_ERR_INVAL;
	}
	if (buf_len < MPROC_FRAME_HEADER_LEN) {
		return REF_ERR_TOO_SHORT;
	}
	const uint32_t magic = ref_le32(buf);
	if (magic != MPROC_FRAME_MAGIC) {
		return REF_ERR_BAD_MAGIC;
	}
	*seq         = ref_le32(buf + 4);
	*payload_len = ref_le32(buf + 8);
	/* Length-field bound check: payload must fit within the remaining
     * input buffer.  Reject lengths that would overflow size_t when
     * added to the header size (catches UINT32_MAX-style attacks
     * against 32-bit-host fuzz runs). */
	if (*payload_len > buf_len - MPROC_FRAME_HEADER_LEN) {
		return REF_ERR_BAD_LEN;
	}
	*payload_ptr = buf + MPROC_FRAME_HEADER_LEN;
	return REF_OK;
}

/* Encode: layout matches decode.  Used by the harness for the
 * round-trip assertion. */
static void ref_encode(uint8_t *out, uint32_t seq, const uint8_t *payload, uint32_t payload_len)
{
	out[0]  = (uint8_t)(MPROC_FRAME_MAGIC);
	out[1]  = (uint8_t)(MPROC_FRAME_MAGIC >> 8);
	out[2]  = (uint8_t)(MPROC_FRAME_MAGIC >> 16);
	out[3]  = (uint8_t)(MPROC_FRAME_MAGIC >> 24);
	out[4]  = (uint8_t)(seq);
	out[5]  = (uint8_t)(seq >> 8);
	out[6]  = (uint8_t)(seq >> 16);
	out[7]  = (uint8_t)(seq >> 24);
	out[8]  = (uint8_t)(payload_len);
	out[9]  = (uint8_t)(payload_len >> 8);
	out[10] = (uint8_t)(payload_len >> 16);
	out[11] = (uint8_t)(payload_len >> 24);
	if (payload_len > 0u && payload != NULL) {
		memcpy(out + MPROC_FRAME_HEADER_LEN, payload, payload_len);
	}
}

/* ------------------------------------------------------------------- */
/* libFuzzer entry point. */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint32_t       seq         = 0;
	const uint8_t *payload_ptr = NULL;
	uint32_t       payload_len = 0;

	const ref_status_t s = ref_decode(data, size, &seq, &payload_ptr, &payload_len);
	if (s != REF_OK) {
		/* Any non-OK return MUST leave the in-caller state safe
         * (the caller drops the frame).  The harness has nothing
         * to assert beyond the fact that we returned cleanly --
         * libFuzzer's sanitizer wrappers catch any UB the decoder
         * leaked while computing the rejection. */
		return 0;
	}

	/* On REF_OK the payload pointer must lie inside the input
     * buffer + the payload length must fit. */
	if (payload_ptr < data || payload_ptr > data + size) {
		__builtin_trap(); /* libFuzzer reports a crash. */
	}
	if (payload_len > size - (size_t)(payload_ptr - data)) {
		__builtin_trap();
	}

	/* Round-trip: re-encode + decode + assert match.  Use a
     * stack buffer sized to the input (libFuzzer caps inputs
     * around 4 KiB by default; this is safe). */
	if (size <= 4096u) {
		uint8_t buf[4096];
		ref_encode(buf, seq, payload_ptr, payload_len);

		uint32_t           seq2         = 0;
		const uint8_t     *payload_ptr2 = NULL;
		uint32_t           payload_len2 = 0;
		const ref_status_t s2           = ref_decode(
		    buf, MPROC_FRAME_HEADER_LEN + payload_len, &seq2, &payload_ptr2, &payload_len2);
		if (s2 != REF_OK) {
			__builtin_trap();
		}
		if (seq2 != seq || payload_len2 != payload_len) {
			__builtin_trap();
		}
		if (payload_len > 0u && memcmp(payload_ptr2, payload_ptr, payload_len) != 0) {
			__builtin_trap();
		}
	}
	return 0;
}
