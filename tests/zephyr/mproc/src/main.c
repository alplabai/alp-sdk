/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for <alp/mproc.h> under native_sim.  No mbox device
 * present, no peer core; the wrapper falls back to NOSUPPORT and
 * we verify that contract plus every NULL-arg branch.
 */

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/mproc.h"
#include "proto/alp_mproc_frame.h"

ZTEST_SUITE(alp_mproc, NULL, NULL, NULL, NULL, NULL);

/* The "no backend" assertions below presume CONFIG_ALP_SDK_MPROC=n
 * so the wrapper shortcircuits to NOSUPPORT.  When MPROC is on (used
 * by the framing scenario below) the wrapper validates args and
 * walks the DT-alias lookup, which yields NOT_READY under native_sim
 * because no `alp-mbox0` / `alp_shmem0` aliases exist there.  Both
 * states are "no backend" -- the framing scenario covers the
 * framing path itself, not these no-backend assertions, so skipping
 * them under MPROC=y keeps each scenario laser-focused. */
#if !defined(CONFIG_ALP_SDK_MPROC)

ZTEST(alp_mproc, test_shmem_open_no_backend_returns_null)
{
    alp_shmem_config_t cfg = {.name = "alp_shmem0", .size = 4096, .cacheable = false};
    alp_shmem_t       *s   = alp_shmem_open(&cfg);
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_mproc, test_shmem_open_null_invalid)
{
    zassert_is_null(alp_shmem_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_mproc, test_shmem_view_null_handle_errors)
{
    void  *base = (void *)0xDEADBEEF;
    size_t size = 999;
    zassert_equal(alp_shmem_view(NULL, &base, &size), ALP_ERR_NOT_READY);
    zassert_is_null(base);
    zassert_equal(size, 0);
    alp_shmem_close(NULL);
}

ZTEST(alp_mproc, test_mbox_open_no_backend_returns_null)
{
    alp_mbox_config_t cfg = {.channel = 0, .peer = ALP_CORE_M55_HE};
    alp_mbox_t       *m   = alp_mbox_open(&cfg);
    zassert_is_null(m);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_mproc, test_mbox_lifecycle_null_handle_safe)
{
    uint8_t buf[4] = {0};
    zassert_equal(alp_mbox_send(NULL, buf, sizeof(buf), 100), ALP_ERR_NOT_READY);
    zassert_equal(alp_mbox_set_callback(NULL, NULL, NULL), ALP_ERR_NOT_READY);
    alp_mbox_close(NULL);
}

ZTEST(alp_mproc, test_hwsem_open_no_backend_returns_null)
{
    alp_hwsem_t *s = alp_hwsem_open(0);
    zassert_is_null(s);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_mproc, test_hwsem_lifecycle_null_handle_safe)
{
    zassert_equal(alp_hwsem_try_lock(NULL), ALP_ERR_NOT_READY);
    zassert_equal(alp_hwsem_lock(NULL, 100), ALP_ERR_NOT_READY);
    zassert_equal(alp_hwsem_unlock(NULL), ALP_ERR_NOT_READY);
    alp_hwsem_close(NULL);
}

#endif /* !CONFIG_ALP_SDK_MPROC */

/* ------------------------------------------------------------------ */
/* IPC envelope (src/common/proto/alp_mproc_frame.{h,c}) — v0.4 prep   */
/*                                                                     */
/* Placeholder binary framing exercised here directly because no real  */
/* mbox peer is available under native_sim.  When the v0.4-final       */
/* nanopb codec drops in, these tests get rewritten against the        */
/* generated pb_encode / pb_decode entry points and verify the same    */
/* round-trip contract.                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_mproc, test_frame_encode_decode_roundtrip)
{
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t  framed[64];
    size_t   framed_len = 0;
    zassert_equal(alp_mproc_frame_encode(42, payload, sizeof(payload),
                                          framed, sizeof(framed), &framed_len),
                  ALP_OK);
    zassert_equal(framed_len, ALP_MPROC_FRAME_HEADER_BYTES + sizeof(payload));

    uint32_t       seq         = 0;
    const uint8_t *payload_out = NULL;
    size_t         payload_len = 0;
    zassert_equal(alp_mproc_frame_decode(framed, framed_len,
                                          &seq, &payload_out, &payload_len),
                  ALP_OK);
    zassert_equal(seq, 42u);
    zassert_equal(payload_len, sizeof(payload));
    zassert_not_null(payload_out);
    zassert_equal(memcmp(payload_out, payload, sizeof(payload)), 0,
                  "payload bytes must round-trip unchanged");
}

ZTEST(alp_mproc, test_frame_encode_zero_payload_succeeds)
{
    uint8_t framed[ALP_MPROC_FRAME_HEADER_BYTES];
    size_t  framed_len = 0;
    zassert_equal(alp_mproc_frame_encode(1, NULL, 0,
                                          framed, sizeof(framed), &framed_len),
                  ALP_OK);
    zassert_equal(framed_len, ALP_MPROC_FRAME_HEADER_BYTES);
}

ZTEST(alp_mproc, test_frame_encode_null_out_invalid)
{
    size_t framed_len = 99;
    zassert_equal(alp_mproc_frame_encode(1, "abc", 3, NULL, 999, &framed_len),
                  ALP_ERR_INVAL);
    zassert_equal(framed_len, 0u, "out_len must be zeroed on failure");
}

ZTEST(alp_mproc, test_frame_encode_payload_null_with_nonzero_len)
{
    uint8_t framed[32];
    zassert_equal(alp_mproc_frame_encode(1, NULL, 4,
                                          framed, sizeof(framed), NULL),
                  ALP_ERR_INVAL);
}

ZTEST(alp_mproc, test_frame_encode_capacity_short)
{
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t       framed[ALP_MPROC_FRAME_HEADER_BYTES + 4]; /* too small for the 8-byte payload */
    zassert_equal(alp_mproc_frame_encode(1, payload, sizeof(payload),
                                          framed, sizeof(framed), NULL),
                  ALP_ERR_NOMEM);
}

ZTEST(alp_mproc, test_frame_decode_short_frame_errors)
{
    uint8_t frame[ALP_MPROC_FRAME_HEADER_BYTES - 1] = {0};
    zassert_equal(alp_mproc_frame_decode(frame, sizeof(frame), NULL, NULL, NULL),
                  ALP_ERR_IO);
}

ZTEST(alp_mproc, test_frame_decode_bad_magic_errors)
{
    /* Encode a valid frame, then corrupt the magic. */
    const uint8_t payload[] = {0xAB};
    uint8_t  framed[32];
    size_t   framed_len = 0;
    zassert_equal(alp_mproc_frame_encode(1, payload, sizeof(payload),
                                          framed, sizeof(framed), &framed_len),
                  ALP_OK);
    framed[0] ^= 0xFFu;
    zassert_equal(alp_mproc_frame_decode(framed, framed_len, NULL, NULL, NULL),
                  ALP_ERR_IO);
}

ZTEST(alp_mproc, test_frame_decode_declared_len_overflow_errors)
{
    /* Header claims a payload longer than the frame -- a peer-side
     * bug or a truncated transfer.  Receiver must drop. */
    uint8_t framed[ALP_MPROC_FRAME_HEADER_BYTES + 4];
    /* Magic */
    framed[0] = 0x41; framed[1] = 0x4D; framed[2] = 0x50; framed[3] = 0x46;
    /* Sequence */
    framed[4] = 1; framed[5] = 0; framed[6] = 0; framed[7] = 0;
    /* Declared length = 100, actual payload only 4 bytes */
    framed[8] = 100; framed[9] = 0; framed[10] = 0; framed[11] = 0;
    framed[12] = 0xDE; framed[13] = 0xAD; framed[14] = 0xBE; framed[15] = 0xEF;
    zassert_equal(alp_mproc_frame_decode(framed, sizeof(framed), NULL, NULL, NULL),
                  ALP_ERR_IO);
}

ZTEST(alp_mproc, test_frame_decode_null_frame_invalid)
{
    uint32_t       seq         = 99;
    const uint8_t *payload_out = (const uint8_t *)0xDEADBEEF;
    size_t         payload_len = 99;
    zassert_equal(alp_mproc_frame_decode(NULL, 0, &seq, &payload_out, &payload_len),
                  ALP_ERR_INVAL);
    zassert_equal(seq, 0u, "seq must be zeroed on failure");
    zassert_is_null(payload_out, "payload_out must be NULLed on failure");
    zassert_equal(payload_len, 0u, "payload_len must be zeroed on failure");
}

/* ------------------------------------------------------------------ */
/* Real backend (CONFIG_ALP_SDK_MPROC=y + DT overlay supplying        */
/* alp-shmem0..1 + the k_sem-backed hwsem fallback).                  */
/* ------------------------------------------------------------------ */
#if defined(CONFIG_ALP_SDK_MPROC) && DT_HAS_ALIAS(alp_shmem0)

ZTEST(alp_mproc, test_shmem_open_resolves_name)
{
    alp_shmem_config_t cfg = {.name = "alp_shmem0", .size = 0, .cacheable = false};
    alp_shmem_t       *s   = alp_shmem_open(&cfg);
    zassert_not_null(s, "open should resolve alp_shmem0 alias");

    void  *base = NULL;
    size_t size = 0;
    zassert_equal(alp_shmem_view(s, &base, &size), ALP_OK);
    zassert_equal((uintptr_t)base, (uintptr_t)DT_REG_ADDR(DT_ALIAS(alp_shmem0)),
                  "base must match DT reg-address");
    zassert_equal(size, DT_REG_SIZE(DT_ALIAS(alp_shmem0)),
                  "size must match DT reg-size");

    alp_shmem_close(s);
}

ZTEST(alp_mproc, test_shmem_open_unknown_name_returns_null)
{
    alp_shmem_config_t cfg = {.name = "nope_not_a_region",
                              .size = 0, .cacheable = false};
    zassert_is_null(alp_shmem_open(&cfg));
    zassert_equal(alp_last_error(), ALP_ERR_NOT_READY);
}

ZTEST(alp_mproc, test_shmem_open_two_regions)
{
    alp_shmem_config_t cfg0 = {.name = "alp_shmem0", .size = 0};
    alp_shmem_config_t cfg1 = {.name = "alp_shmem1", .size = 0};
    alp_shmem_t       *s0   = alp_shmem_open(&cfg0);
    alp_shmem_t       *s1   = alp_shmem_open(&cfg1);
    zassert_not_null(s0);
    zassert_not_null(s1);
    zassert_not_equal((void *)s0, (void *)s1, "distinct handles for distinct regions");

    void  *base0 = NULL, *base1 = NULL;
    size_t size0 = 0,    size1 = 0;
    zassert_equal(alp_shmem_view(s0, &base0, &size0), ALP_OK);
    zassert_equal(alp_shmem_view(s1, &base1, &size1), ALP_OK);
    zassert_not_equal((uintptr_t)base0, (uintptr_t)base1);

    alp_shmem_close(s0);
    alp_shmem_close(s1);
}

ZTEST(alp_mproc, test_shmem_open_exhausts_pool)
{
    /* This test specifically exercises the NOMEM path on the 3rd open.
     * If the pool grows, open the right number to exhaust it instead.
     */
    BUILD_ASSERT(CONFIG_ALP_SDK_MAX_SHMEM_HANDLES == 2,
                 "test assumes default pool of 2; revisit if Kconfig default changes");

    /* CONFIG_ALP_SDK_MAX_SHMEM_HANDLES = 2 by default; open both then
     * verify a third open with the same name fails with NOMEM. */
    alp_shmem_config_t cfg = {.name = "alp_shmem0", .size = 0};
    alp_shmem_t *a = alp_shmem_open(&cfg);
    alp_shmem_t *b = alp_shmem_open(&cfg);
    zassert_not_null(a);
    zassert_not_null(b);
    zassert_is_null(alp_shmem_open(&cfg));
    zassert_equal(alp_last_error(), ALP_ERR_NOMEM);
    alp_shmem_close(a);
    alp_shmem_close(b);
}

#endif /* CONFIG_ALP_SDK_MPROC && DT_HAS_ALIAS(alp_shmem0) */
