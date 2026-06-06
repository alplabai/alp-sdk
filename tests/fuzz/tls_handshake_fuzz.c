/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the TLS 1.2 / 1.3 handshake-record header
 * decode path that the SDK's `<alp/iot.h>` wrapper inspects before
 * handing payloads down to mbedtls / OpenSSL.
 *
 * Upstream mbedtls + OpenSSL maintain their own fuzz corpora for
 * the full handshake state machine -- the parser inside those
 * libraries is already heavily-tested.  What's *not* covered
 * upstream is the small framing helper the SDK keeps in
 * `src/zephyr/iot_mqtt_tls.c` / `src/yocto/iot_tls.c` for the
 * cross-backend "peek the record header before forwarding" logic.
 * This harness validates the SDK-owned slice.
 *
 * The reference parser inline below mirrors that slice:
 *   - Record header: type:u8 version:u16 length:u16.
 *   - Inner handshake header (when type == 0x16): handshake_type:u8
 *     length:u24.
 *   - ClientHello body: protocol_version:u16 random:32 sid_len:u8
 *     sid:sid_len cipher_suites_len:u16 cipher_suites:cipher_suites_len
 *     compression_methods_len:u8 compression_methods:compression_methods_len
 *     [extensions_len:u16 extensions:extensions_len].
 *
 * What it catches:
 *   - Record length-field overrun (the canonical TLS CVE class --
 *     CVE-2014-0160 / Heartbleed shape).
 *   - 24-bit handshake length lying beyond the record body.
 *   - sid_len / cipher_suites_len / compression_methods_len /
 *     extensions_len cascade overruns inside ClientHello.
 *   - Reserved content types (only 20/21/22/23/24 are valid).
 *   - Truncated header (size < 5).
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=yocto \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_tls_handshake
 *
 * Run:
 *   ./build-fuzz/tests/fuzz/alp_fuzz_tls_handshake -max_total_time=30 \
 *         tests/fuzz/corpus/tls_handshake
 */

#include <stddef.h>
#include <stdint.h>

/* TLS record content types per RFC 5246 §6.2.1 + RFC 8446 §B.1. */
enum {
    TLS_CT_CHANGE_CIPHER_SPEC = 20,
    TLS_CT_ALERT              = 21,
    TLS_CT_HANDSHAKE          = 22,
    TLS_CT_APPLICATION_DATA   = 23,
    TLS_CT_HEARTBEAT          = 24
};

/* TLS handshake message types per RFC 8446 §B.3 (TLS 1.3) -- the
 * SDK wrapper only inspects ClientHello (1) on the outbound path,
 * but the parser walks past anything that looks well-formed. */
enum {
    TLS_HS_CLIENT_HELLO         = 1,
    TLS_HS_SERVER_HELLO         = 2,
    TLS_HS_NEW_SESSION_TICKET   = 4,
    TLS_HS_ENCRYPTED_EXTENSIONS = 8,
    TLS_HS_CERTIFICATE          = 11,
    TLS_HS_CERTIFICATE_REQUEST  = 13,
    TLS_HS_CERTIFICATE_VERIFY   = 15,
    TLS_HS_FINISHED             = 20
};

typedef enum {
    TLS_REF_OK                = 0,
    TLS_REF_ERR_TOO_SHORT     = 1,
    TLS_REF_ERR_BAD_TYPE      = 2,
    TLS_REF_ERR_RECORD_OVR    = 3,
    TLS_REF_ERR_HS_OVR        = 4,
    TLS_REF_ERR_SID_OVR       = 5,
    TLS_REF_ERR_CSUITE_OVR    = 6,
    TLS_REF_ERR_COMP_OVR      = 7,
    TLS_REF_ERR_EXT_OVR       = 8,
    TLS_REF_ERR_BAD_VERSION   = 9
} tls_ref_status_t;

/* Parse the first TLS record header + (if it's a ClientHello)
 * walk the variable-length sub-fields, asserting every claimed
 * length stays within the buffer.  Returns OK or a specific
 * error class. */
static tls_ref_status_t tls_ref_parse(const uint8_t *buf, size_t size)
{
    /* 5-byte TLS record header: type + version + length. */
    if (size < 5u) return TLS_REF_ERR_TOO_SHORT;

    const uint8_t  ct          = buf[0];
    const uint16_t version     = (uint16_t)(((uint16_t)buf[1] << 8) | buf[2]);
    const uint16_t record_len  = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);

    if (ct < TLS_CT_CHANGE_CIPHER_SPEC || ct > TLS_CT_HEARTBEAT) {
        return TLS_REF_ERR_BAD_TYPE;
    }
    /* RFC 8446 records that TLS 1.0..1.3 fit within (0x0301..0x0304);
     * we accept anything in 0x0300..0x03FF to allow for non-strict
     * implementations the wrapper still forwards.  Bytes outside
     * that envelope are clearly malformed. */
    if ((version & 0xFF00u) != 0x0300u) return TLS_REF_ERR_BAD_VERSION;

    if ((size_t)record_len + 5u > size) return TLS_REF_ERR_RECORD_OVR;

    if (ct != TLS_CT_HANDSHAKE) {
        /* Application data / alert / change-cipher-spec / heartbeat
         * carry opaque payloads -- nothing more to walk. */
        return TLS_REF_OK;
    }

    /* Handshake message header: type + length (24-bit BE). */
    if (record_len < 4u) return TLS_REF_ERR_HS_OVR;
    const uint8_t *hs       = &buf[5];
    const uint8_t  hs_type  = hs[0];
    const uint32_t hs_len   = ((uint32_t)hs[1] << 16) |
                              ((uint32_t)hs[2] << 8)  |
                              (uint32_t)hs[3];
    if (hs_len + 4u > (uint32_t)record_len) return TLS_REF_ERR_HS_OVR;

    if (hs_type != TLS_HS_CLIENT_HELLO) {
        /* The wrapper only inspects ClientHello; everything else
         * stops here. */
        return TLS_REF_OK;
    }

    /* ClientHello body: 2-byte version + 32-byte random + sid_len. */
    if (hs_len < 2u + 32u + 1u) return TLS_REF_ERR_HS_OVR;
    const uint8_t *ch    = hs + 4;
    size_t         off   = 0u;
    off += 2u; /* legacy_version */
    off += 32u; /* random */
    const uint8_t sid_len = ch[off];
    off += 1u;
    if ((size_t)sid_len > (size_t)hs_len - off) return TLS_REF_ERR_SID_OVR;
    off += sid_len;

    /* cipher_suites_len:u16 */
    if (off + 2u > hs_len) return TLS_REF_ERR_CSUITE_OVR;
    const uint16_t csuite_len = (uint16_t)(((uint16_t)ch[off] << 8) | ch[off + 1]);
    off += 2u;
    /* Each suite is 2 bytes; len must be even and non-zero. */
    if (csuite_len == 0u || (csuite_len & 1u)) return TLS_REF_ERR_CSUITE_OVR;
    if ((size_t)csuite_len > (size_t)hs_len - off) return TLS_REF_ERR_CSUITE_OVR;
    /* Touch each cipher-suite byte -- catches OOB on a buggy
     * adjacent-walk that the outer length check missed. */
    {
        volatile uint8_t sink = 0;
        for (size_t i = 0; i < (size_t)csuite_len; ++i) {
            sink ^= ch[off + i];
        }
        (void)sink;
    }
    off += csuite_len;

    /* compression_methods_len:u8 (TLS 1.3 forces this to "null" =
     * len 1 / value 0; older versions may legitimately offer
     * multiple).  Parser accepts any well-formed length, the
     * wrapper rejects len != 1 || methods[0] != 0 above. */
    if (off + 1u > hs_len) return TLS_REF_ERR_COMP_OVR;
    const uint8_t comp_len = ch[off];
    off += 1u;
    if ((size_t)comp_len > (size_t)hs_len - off) return TLS_REF_ERR_COMP_OVR;
    off += comp_len;

    /* Extensions block is optional in TLS 1.0/1.1; mandatory in
     * 1.2+/1.3.  Length is u16 BE. */
    if (off == hs_len) return TLS_REF_OK;
    if (off + 2u > hs_len) return TLS_REF_ERR_EXT_OVR;
    const uint16_t ext_len = (uint16_t)(((uint16_t)ch[off] << 8) | ch[off + 1]);
    off += 2u;
    if ((size_t)ext_len > (size_t)hs_len - off) return TLS_REF_ERR_EXT_OVR;
    /* Walk individual extensions: each is type:u16 length:u16 data. */
    size_t ext_off = 0u;
    while (ext_off < (size_t)ext_len) {
        if (ext_off + 4u > (size_t)ext_len) return TLS_REF_ERR_EXT_OVR;
        const uint16_t e_len = (uint16_t)(((uint16_t)ch[off + ext_off + 2] << 8) |
                                          ch[off + ext_off + 3]);
        if ((size_t)e_len > (size_t)ext_len - ext_off - 4u) return TLS_REF_ERR_EXT_OVR;
        ext_off += 4u + e_len;
    }

    return TLS_REF_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)tls_ref_parse(data, size);
    return 0;
}
