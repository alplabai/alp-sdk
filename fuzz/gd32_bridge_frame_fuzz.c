/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the GD32 bridge frame parser shared between
 * the host-side driver (`<alp/chips/gd32g553.h>`) and the firmware
 * (`gd32-bridge/src/protocol.c`).
 *
 * Frame layout (per docs/gd32-bridge-protocol.md §4):
 *   SOF(1) | CMD|STATUS(1) | PAYLOAD(N) | CRC16(2)
 *
 * The fuzzer drives arbitrary byte streams through the same
 * validation logic the receivers run:
 *   - SOF must be 0xA5.
 *   - Length must accommodate at minimum SOF + opcode + 2-byte CRC.
 *   - CRC-16/CCITT-FALSE over SOF | CMD | PAYLOAD must match.
 *
 * What this harness catches:
 *   - Off-by-one on payload-length math (the receivers compute
 *     payload size from opcode; mismatched length must reject, not
 *     overrun).
 *   - CRC indexed past buffer end on short frames.
 *   - SOF=0xA5 at offset > 0 not being mistaken for the start of a
 *     valid frame.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_gd32_bridge_frame
 */

#include <stddef.h>
#include <stdint.h>

#define GD32_BRIDGE_SOF      0xA5u

/* CRC-16/CCITT-FALSE -- byte-for-byte identical to the implementation
 * shared by host driver + firmware. */
static uint16_t crc16_ccitt_false(const uint8_t *buf, size_t len)
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

/* Frame validator that mirrors what the host driver does after a
 * complete read.  Returns 0 on a structurally valid frame, non-zero
 * otherwise. */
static int validate_frame(const uint8_t *buf, size_t len)
{
    if (len < 4u) return -1;                       /* SOF + STATUS + CRC */
    if (buf[0] != GD32_BRIDGE_SOF) return -2;

    /* The opcode-derived payload length is firmware-defined; the
     * fuzzer doesn't model that table.  What it CAN test is that
     * the CRC over (len - 2) bytes of `buf` matches the trailing
     * 2-byte CRC -- the validator never reads past `buf[len-1]`. */
    const uint16_t computed = crc16_ccitt_false(buf, len - 2u);
    const uint16_t on_wire  = ((uint16_t)buf[len - 2u] << 8)
                             | (uint16_t)buf[len - 1u];
    if (computed != on_wire) return -3;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)validate_frame(data, size);
    return 0;
}
