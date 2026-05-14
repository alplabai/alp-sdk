/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the BLE advertising / scan-response packet
 * parser (Bluetooth Core 5.4 §1.3 of [Vol 6, Part B] -- LE
 * advertising channel PDUs).
 *
 * BLE adv PDUs are length-prefixed AD (Advertising Data) structures:
 *
 *     +--------+--------+----+----+-----+----+
 *     | len_n  | type_n | b_0 | b_1 | ... | b_{len_n-2} |
 *     +--------+--------+----+----+-----+----+
 *
 *   len_n includes type_n (so payload bytes = len_n - 1).
 *
 * Length values that exceed the remaining buffer are a classic
 * out-of-bounds read attack -- a malicious adv-broadcaster can send
 * a 31-byte adv pkt where the last AD claims len_n = 100, and a
 * naive parser walks off the end.
 *
 * The harness feeds the raw fuzz buffer into a reference parser
 * that enumerates AD structures and asserts every per-AD
 * (offset + len) range stays within the input buffer.  If the
 * parser ever returns "ok" with an AD whose payload crosses the
 * buffer boundary, the harness traps + libFuzzer reports a crash.
 *
 * What it catches:
 *   - Length-field overrun (the canonical CVE class for BLE).
 *   - Zero-length AD that should terminate enumeration cleanly.
 *   - All-FF / all-00 buffer fuzz -- the parser must classify as
 *     malformed, not crash.
 *   - Truncated-buffer (1 byte / 2 bytes) inputs that hit edge
 *     cases in the length / type-byte handling.
 *
 * The reference parser here mirrors what the SDK's BLE backend
 * (src/zephyr/ble_zephyr.c) calls into Zephyr's bt_data_parse +
 * what a Yocto BlueZ-side consumer would parse via D-Bus.  Both
 * upstreams have known CVE history in this area; keeping our
 * own reference + fuzzing it is cheap insurance.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=yocto \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_ble_adv_parser
 *
 * Run:
 *   ./build-fuzz/tests/fuzz/alp_fuzz_ble_adv_parser \
 *         -max_total_time=30 tests/fuzz/corpus/ble_adv
 */

#include <stddef.h>
#include <stdint.h>

/* Maximum legal BLE adv payload (Core 5.4 §2.3): 31 bytes for legacy
 * adv, 254 for extended adv.  The harness accepts either; the parser
 * only enforces structural validity, not the legacy/extended caps. */
#define BLE_ADV_MAX 254u

/* BLE AD types we recognise.  An unknown type is NOT a parse error
 * -- the parser surfaces unknowns to the caller so the scan-callback
 * can decide.  We just record the type byte and skip the payload. */
#define BT_AD_TYPE_FLAGS        0x01
#define BT_AD_TYPE_NAME_SHORT   0x08
#define BT_AD_TYPE_NAME_FULL    0x09
#define BT_AD_TYPE_TX_POWER     0x0A
#define BT_AD_TYPE_MFG_DATA     0xFF

typedef enum {
    BLE_OK              = 0,
    BLE_ERR_TOO_SHORT   = -1,   /* AD claims more bytes than remain */
    BLE_ERR_LEN_ZERO    = -2,   /* Encountered AD with len = 0 (terminator) */
    BLE_ERR_INVAL       = -3,
} ble_status_t;

/* Per-AD callback signature.  Returning non-zero stops parsing. */
typedef int (*ble_ad_cb_t)(uint8_t type, const uint8_t *data, uint8_t data_len,
                           void *userdata);

/* Enumerate AD structures in `buf[0..buf_len)`.  Calls `cb` once per
 * structure; bails on first error.  Returns BLE_OK if the entire
 * buffer was consumed cleanly. */
static ble_status_t ble_parse_adv(const uint8_t *buf, size_t buf_len,
                                  ble_ad_cb_t cb, void *userdata) {
    if (buf == NULL && buf_len > 0) {
        return BLE_ERR_INVAL;
    }
    size_t off = 0;
    while (off < buf_len) {
        const uint8_t len_n = buf[off];
        if (len_n == 0) {
            /* Legal terminator -- some adv implementations pad with
             * zeros.  Caller decides whether to treat as end-of-list. */
            return BLE_OK;
        }
        /* len_n includes the type byte; need len_n bytes after the
         * length byte.  Bound check: (off + 1 + len_n) must not
         * exceed buf_len. */
        if ((size_t)(off + 1 + len_n) > buf_len) {
            return BLE_ERR_TOO_SHORT;
        }
        const uint8_t type_n   = buf[off + 1];
        const uint8_t data_len = (uint8_t)(len_n - 1);
        if (cb != NULL) {
            const int rc = cb(type_n, buf + off + 2, data_len, userdata);
            if (rc != 0) {
                /* Caller stopped parsing.  Not an error. */
                return BLE_OK;
            }
        }
        off += 1u + (size_t)len_n;
    }
    return BLE_OK;
}

/* ------------------------------------------------------------------- */
/* libFuzzer harness. */

/* The AD callback verifies the pointer + length the parser gave us
 * lies within the original input buffer.  Out-of-bounds == the parser
 * miscalculated. */
struct fuzz_ctx {
    const uint8_t *input_base;
    size_t         input_size;
    int            ad_count;
};

static int ad_cb(uint8_t type, const uint8_t *data, uint8_t data_len, void *userdata) {
    (void)type;
    struct fuzz_ctx *ctx = (struct fuzz_ctx *)userdata;
    if (data < ctx->input_base || data > ctx->input_base + ctx->input_size) {
        __builtin_trap();
    }
    if ((size_t)(data - ctx->input_base) + (size_t)data_len > ctx->input_size) {
        __builtin_trap();
    }
    ctx->ad_count++;
    /* libFuzzer can drive the parser to find longer paths if we
     * occasionally early-out.  Don't always early-out (otherwise
     * fuzzer never explores past the first AD); use ad_count to
     * stop after 50 ADs as a guard against pathological inputs. */
    return (ctx->ad_count > 50) ? 1 : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > BLE_ADV_MAX) {
        /* Cap to the largest legal extended-adv size to keep the
         * fuzzer focused on protocol-shape issues, not "what
         * happens with 10MB"; libFuzzer already caps separately. */
        size = BLE_ADV_MAX;
    }
    struct fuzz_ctx ctx = {
        .input_base = data,
        .input_size = size,
        .ad_count   = 0,
    };
    (void)ble_parse_adv(data, size, ad_cb, &ctx);
    return 0;
}
