/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the GD32 bridge frame parser shared between
 * the host-side driver (`<alp/chips/gd32g553.h>`) and the firmware
 * (`firmware/gd32-bridge/src/protocol.c`).  This file links against the
 * firmware-side protocol implementation + the stub HAL backend so
 * the fuzzer drives the *real* `protocol_dispatch` instead of a
 * reference parser -- divergence between the dispatcher's payload-
 * length math and the documented spec is now a fuzz failure, not a
 * harness-vs-production gap.
 *
 * Frame layout (per docs/gd32-bridge-protocol.md §4):
 *   SOF(1) | CMD|STATUS(1) | PAYLOAD(N) | CRC16(2)
 *
 * Driven input is split into:
 *   byte[0]    = opcode (CMD_*)
 *   byte[1..]  = candidate payload that the dispatcher consumes
 *
 * What this harness catches:
 *   - Off-by-one on opcode-derived payload-length math in any
 *     handler (per-handler request-length checks at the top of each
 *     `handle_*` function).
 *   - Reply-buffer overruns (the dispatcher writes to a fixed-size
 *     reply scratch; if a handler writes past `reply_payload_cap`
 *     AddressSanitizer trips).
 *   - Divergence between the firmware CRC (`crc16_ccitt_false`
 *     symbol linked from protocol.c) and the reference impl below
 *     (ref_crc16) -- both produce CRC over arbitrary fuzz bytes
 *     every iteration; mismatch aborts.
 *
 * What this harness does NOT yet catch:
 *   - Transport-framing bugs (SPI / I2C SOF + CRC trim).  The
 *     transports' parsers (`firmware/gd32-bridge/src/transport_spi.c` and
 *     `transport_i2c.c`) live inside ISR-driven byte buffers; a
 *     useful fuzz harness would need to model the CS / START
 *     transitions.  Tracked as future work in tests/fuzz/README.md.
 *   - Host-side reply validation (chips/gd32g553/gd32g553.c's
 *     `spi_xfer` / `i2c_xfer` reply parsers).  Would require
 *     extracting them as standalone non-static functions; see the
 *     same TODO note.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_gd32_bridge_frame
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  /* abort() on CRC divergence */

#include "protocol.h"   /* firmware-side; declares protocol_dispatch + crc16_ccitt_false */

/* Reference CRC-16/CCITT-FALSE -- intentionally a second
 * implementation, byte-for-byte expected to agree with the
 * firmware-side `crc16_ccitt_false` linked in via protocol.c.  The
 * fuzzer cross-checks both every iteration; if they ever disagree
 * on any input the harness aborts, surfacing the divergence as a
 * libFuzzer crash. */
static uint16_t ref_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)buf[i] << 8;
        for (unsigned b = 0; b < 8; ++b) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* CRC parity check: every iteration confirms the firmware CRC
     * symbol agrees with the reference impl.  Run on the whole
     * input -- a CRC drift on byte stream X exposes the bug. */
    {
        const uint16_t a = ref_crc16(data, size);
        const uint16_t b = crc16_ccitt_false(data, size);
        if (a != b) abort();
    }

    /* Frame dispatcher: opcode + payload pulled from the fuzz input.
     * The dispatcher's per-opcode handlers each have strict length
     * checks; mismatched lengths must reject with STATUS_INVAL, not
     * overrun the reply buffer. */
    if (size < 1u) return 0;
    const uint8_t  cmd          = data[0];
    const uint8_t *payload      = data + 1;
    const size_t   payload_len  = size - 1u;

    uint8_t reply_scratch[1u + (GD32_BRIDGE_ADC_MAX_SAMPLES * 2u)];
    size_t  reply_len           = 0u;
    (void)protocol_dispatch(cmd, payload, payload_len,
                            reply_scratch, sizeof reply_scratch,
                            &reply_len);
    /* Sanity: dispatcher must not claim more bytes than scratch had. */
    if (reply_len > sizeof reply_scratch) abort();
    return 0;
}
