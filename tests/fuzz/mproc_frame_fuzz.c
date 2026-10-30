/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the SDK's mproc IPC envelope framing
 * (src/common/proto/alp_mproc_frame.{h,c}).
 *
 * The placeholder framing is the interim wire format used (real
 * nanopb wire deferred, no committed version) when
 * CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING=y: a fixed 12-byte
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
 * This harness links the PRODUCTION src/common/proto/alp_mproc_frame.c
 * (see tests/fuzz/CMakeLists.txt), so it fuzzes the exact
 * alp_mproc_frame_decode that src/backends/mproc/zephyr_drv.c calls on
 * peer-supplied mailbox bytes -- there is no inline reference copy that
 * could drift from it.  Any non-OK return must leave the outputs zeroed;
 * any OK return must produce a payload pointer inside the input buffer
 * and a payload length that fits in the remaining bytes.
 *
 * The harness also re-encodes the decoded payload + sequence with the
 * production encoder and asserts the bytes match the original input
 * prefix -- this catches asymmetric encode/decode bugs.  It also decodes
 * with NULL output pointers, the form the Zephyr driver uses when it only
 * needs the validation verdict.
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
 *         -DALP_GD32_BRIDGE_FIRMWARE_DIR=<gd32-bridge-firmware checkout> \
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

#include "proto/alp_mproc_frame.h"

/* libFuzzer entry point. */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint32_t       seq         = 0xA5A5A5A5u;
	const uint8_t *payload_ptr = (const uint8_t *)(uintptr_t)1;
	size_t         payload_len = 0xDEADu;

	/* NULL outputs are legal (the Zephyr driver passes a NULL
	 * sequence_out); the verdict must not depend on them. */
	const alp_status_t sn = alp_mproc_frame_decode(data, size, NULL, NULL, NULL);

	const alp_status_t s = alp_mproc_frame_decode(data, size, &seq, &payload_ptr, &payload_len);
	if ((s == ALP_OK) != (sn == ALP_OK)) {
		__builtin_trap();
	}
	if (s != ALP_OK) {
		/* A rejected frame must not leak stale or partial outputs. */
		if (seq != 0u || payload_ptr != NULL || payload_len != 0u) {
			__builtin_trap();
		}
		return 0;
	}

	/* On ALP_OK the payload must lie inside the input buffer. */
	if (payload_ptr < data || payload_ptr > data + size) {
		__builtin_trap(); /* libFuzzer reports a crash. */
	}
	if (payload_len > size - (size_t)(payload_ptr - data)) {
		__builtin_trap();
	}

	/* Round-trip: re-encode with the production encoder and compare. */
	if (size <= 4096u) {
		uint8_t buf[4096];
		size_t  out_len = 0;
		if (alp_mproc_frame_encode(seq, payload_ptr, payload_len, buf, sizeof(buf), &out_len) !=
		    ALP_OK) {
			__builtin_trap();
		}
		if (out_len != ALP_MPROC_FRAME_HEADER_BYTES + payload_len ||
		    memcmp(buf, data, out_len) != 0) {
			__builtin_trap();
		}
	}
	return 0;
}
