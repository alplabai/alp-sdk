/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the SWD packet-header + 32-bit data parity
 * checks used by `chips/gd32_swd/`.
 *
 * The host-side bit-bang controller composes 8-bit request headers
 * + 32-bit data words with their parity bits per Arm ADIv5; the
 * receiver-side validation logic must reject malformed sequences
 * gracefully rather than tripping a UBSan bit-shift overflow or
 * indexing past a fixed buffer.
 *
 * Status: inline reference parser, not yet wired to production.
 * The host driver in `chips/gd32_swd/gd32_swd.c` composes + checks
 * parity inline inside `swd_xfer` (no separately-callable validator
 * to link).  Extracting `swd_compute_header_parity` /
 * `swd_compute_data_parity` as standalone non-static helpers is
 * tracked as a follow-up; once that lands the harness will replace
 * the impls below with calls to the production symbols + a CRC-style
 * cross-check against a second reference impl, matching the pattern
 * `gd32_bridge_frame_fuzz.c` already uses for the bridge CRC.
 *
 * Driven input is interpreted as:
 *   byte[0]   = candidate request header
 *   byte[1..4] = candidate 32-bit data (little-endian)
 *   byte[5]   = candidate parity (low bit only)
 *
 * Validation mirrors what gd32_swd's swd_xfer would do after the
 * bus phase: header parity must match the XOR of the request bits,
 * data parity must match XOR-reduce of the 32 data bits.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_swd_packet
 */

#include <stddef.h>
#include <stdint.h>

/* Pull a single bit out of `byte`. */
static int bit(uint8_t byte, unsigned i) { return (byte >> i) & 1u; }

/* Header layout, Arm DDI 0316C §5.3:
 *   bit 0   start (must be 1)
 *   bit 1   APnDP
 *   bit 2   RnW
 *   bit 3   A2
 *   bit 4   A3
 *   bit 5   parity = XOR(APnDP, RnW, A2, A3)
 *   bit 6   stop  (must be 0)
 *   bit 7   park  (must be 1)
 */
static int validate_header(uint8_t hdr)
{
    if (bit(hdr, 0) != 1) return -1; /* start */
    if (bit(hdr, 6) != 0) return -2; /* stop */
    if (bit(hdr, 7) != 1) return -3; /* park */
    const int parity = bit(hdr, 1) ^ bit(hdr, 2) ^ bit(hdr, 3) ^ bit(hdr, 4);
    if (bit(hdr, 5) != parity) return -4;
    return 0;
}

static int validate_data_parity(uint32_t data, uint8_t parity)
{
    uint32_t v = data;
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    const int computed = (int)(v & 1u);
    if ((parity & 1u) != computed) return -1;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 6u) return 0;
    const uint8_t  hdr = data[0];
    const uint32_t d   = (uint32_t)data[1]
                       | ((uint32_t)data[2] << 8)
                       | ((uint32_t)data[3] << 16)
                       | ((uint32_t)data[4] << 24);
    const uint8_t parity = data[5];

    (void)validate_header(hdr);
    (void)validate_data_parity(d, parity);
    return 0;
}
