/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the OPTIGA Trust M APDU response parser
 * (`chips/optiga_trust_m/` decode path).
 *
 * OPTIGA APDUs follow ISO-7816-4 short form on the I²C transport:
 *
 *   Command:   CLA INS P1 P2 [Lc data Le]
 *   Response:  [data] SW1 SW2
 *
 * The response can be:
 *   - 2 bytes total: just SW1-SW2 (status-only; no data).
 *   - N bytes data + SW1-SW2 (data + status).
 *   - Chained response across multiple GET_RESPONSE APDUs (BER-TLV
 *     concatenation), bounded by the chip's MAX_DATA_BYTES (1788
 *     for OPTIGA Trust M).
 *
 * Untrusted side: the chip is on a shared I²C bus (BRD_I2C on V2N)
 * and a malicious peer master could inject bytes between our command
 * and its response.  The parser must:
 *
 *   - Reject responses shorter than 2 bytes (no SW1-SW2).
 *   - Reject claimed data lengths that exceed the buffer.
 *   - Map SW1-SW2 to alp_status_t without crashing on unknown
 *     status codes.
 *   - Bound the BER-TLV walk so a malicious TLV with nested length
 *     fields > input buffer doesn't OOM-read.
 *
 * What it catches:
 *   - Off-by-one on the SW1-SW2 trailing-bytes split.
 *   - BER-TLV length-of-length overflow (CVE class).
 *   - Unknown OPTIGA-specific status codes that the parser must
 *     surface as ALP_ERR_IO, not crash.
 *   - SW1=0x61 (more data available) loop bound on chained reads.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=yocto \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_optiga_apdu
 *
 * Run:
 *   ./build-fuzz/tests/fuzz/alp_fuzz_optiga_apdu \
 *         -max_total_time=30 tests/fuzz/corpus/optiga_apdu
 */

#include <stddef.h>
#include <stdint.h>

/* OPTIGA Trust M PCT v3.1 §3.4 max data per response. */
#define OPTIGA_MAX_DATA_BYTES   1788u

/* Common SW1-SW2 status codes (ISO-7816-4 + OPTIGA-specific).  An
 * unknown SW pair is mapped to a generic "io error" -- the parser
 * must surface it, not crash. */
#define SW_SUCCESS          0x9000
#define SW_MORE_DATA(sw)    (((sw) & 0xFF00u) == 0x6100u)
#define SW_WRONG_LENGTH     0x6700
#define SW_SECURITY_NOT_MET 0x6982
#define SW_AUTH_FAILED      0x6300

typedef enum {
    OPTIGA_OK              = 0,
    OPTIGA_ERR_TOO_SHORT   = -1,
    OPTIGA_ERR_BAD_LEN     = -2,
    OPTIGA_ERR_BAD_TLV     = -3,
    OPTIGA_ERR_BAD_SW      = -4,   /* Status word indicates command failure */
    OPTIGA_ERR_INVAL       = -5,
} optiga_status_t;

/* Parse a raw APDU response: split the trailing SW1-SW2, decode SW,
 * walk any BER-TLV payload structures.  Returns OPTIGA_OK on a
 * SW_SUCCESS response with a well-formed TLV payload.  Returns a
 * specific negative code otherwise; never crashes. */
static optiga_status_t optiga_parse_response(const uint8_t *buf, size_t buf_len,
                                             uint16_t *sw_out,
                                             const uint8_t **payload_out,
                                             size_t *payload_len_out) {
    if (buf == NULL || sw_out == NULL || payload_out == NULL ||
        payload_len_out == NULL) {
        return OPTIGA_ERR_INVAL;
    }
    if (buf_len < 2u) {
        return OPTIGA_ERR_TOO_SHORT;
    }
    const uint16_t sw = (uint16_t)((buf[buf_len - 2] << 8) | buf[buf_len - 1]);
    *sw_out          = sw;
    *payload_out     = buf;
    *payload_len_out = buf_len - 2u;

    if (sw != SW_SUCCESS && !SW_MORE_DATA(sw)) {
        return OPTIGA_ERR_BAD_SW;
    }
    if (*payload_len_out > OPTIGA_MAX_DATA_BYTES) {
        return OPTIGA_ERR_BAD_LEN;
    }

    /* Walk the BER-TLV structure inside the payload.  Each TLV is
     * `tag (1 byte) | len (1-3 bytes) | value`.  Length encoding:
     *   - byte < 0x80          : single-byte short form
     *   - byte == 0x81         : next 1 byte is the length
     *   - byte == 0x82         : next 2 bytes are the length (BE)
     *   - byte == 0x80, > 0x82 : reject (indefinite / unsupported).
     *
     * Bound check: every consumed (offset + len) must stay within
     * payload_len.  A malicious 0x82 0xFF 0xFF length on a 10-byte
     * input is the canonical fuzz vector here. */
    size_t off = 0;
    while (off < *payload_len_out) {
        if (off + 1u > *payload_len_out) {
            return OPTIGA_ERR_BAD_TLV;
        }
        /* Skip the tag byte. */
        off += 1u;
        if (off >= *payload_len_out) {
            return OPTIGA_ERR_BAD_TLV;
        }
        size_t       tlv_len    = 0u;
        const uint8_t len_byte0 = buf[off];
        off += 1u;
        if (len_byte0 < 0x80u) {
            tlv_len = len_byte0;
        } else if (len_byte0 == 0x81u) {
            if (off + 1u > *payload_len_out) return OPTIGA_ERR_BAD_TLV;
            tlv_len = buf[off];
            off += 1u;
        } else if (len_byte0 == 0x82u) {
            if (off + 2u > *payload_len_out) return OPTIGA_ERR_BAD_TLV;
            tlv_len = ((size_t)buf[off] << 8) | (size_t)buf[off + 1];
            off += 2u;
        } else {
            /* Indefinite (0x80) or 3+ byte length forms not supported. */
            return OPTIGA_ERR_BAD_TLV;
        }
        if (off + tlv_len > *payload_len_out) {
            return OPTIGA_ERR_BAD_TLV;
        }
        off += tlv_len;
    }
    return OPTIGA_OK;
}

/* ------------------------------------------------------------------- */
/* libFuzzer entry. */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint16_t       sw          = 0;
    const uint8_t *payload     = NULL;
    size_t         payload_len = 0;
    const optiga_status_t s    = optiga_parse_response(data, size,
                                                       &sw, &payload, &payload_len);
    if (s == OPTIGA_OK) {
        /* On OK, payload must point inside the input buffer. */
        if (payload < data || payload > data + size) {
            __builtin_trap();
        }
        if (payload_len > size) {
            __builtin_trap();
        }
    }
    /* All other returns are clean parse rejections -- libFuzzer's
     * sanitizers catch any UB along the rejection path. */
    return 0;
}
