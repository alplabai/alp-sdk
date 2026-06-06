/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/OpenSSL security backend
 * (src/yocto/security_yocto.c).
 *
 * Covers SHA-256 KAT, AES-128-GCM + ChaCha20-Poly1305 round-trip,
 * tag-mismatch detection on decrypt, NULL-arg refusal, and
 * alp_random_bytes basic correctness (length + zero-len pass).
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_security_openssl
 *   ctest --test-dir build -R alp_test_security_openssl
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/security.h"
#include "alp/peripheral.h"

#include "test_assert.h"

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */

static void test_sha256_known_answer(void)
{
    /* NIST FIPS 180-4 test vector: SHA-256("abc") =
     * ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad */
    static const uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    alp_hash_t *h = alp_hash_open(ALP_HASH_SHA256);
    ALP_ASSERT_TRUE(h != NULL);
    ALP_ASSERT_EQ_INT(alp_hash_update(h, (const uint8_t *)"abc", 3), ALP_OK);
    uint8_t got[32];
    size_t  glen = 0;
    ALP_ASSERT_EQ_INT(alp_hash_finish(h, got, sizeof(got), &glen), ALP_OK);
    ALP_ASSERT_EQ_INT(glen, 32);
    ALP_ASSERT_TRUE(memcmp(got, expected, 32) == 0);
}

static void test_sha384_length(void)
{
    alp_hash_t *h = alp_hash_open(ALP_HASH_SHA384);
    ALP_ASSERT_TRUE(h != NULL);
    ALP_ASSERT_EQ_INT(alp_hash_update(h, (const uint8_t *)"x", 1), ALP_OK);
    uint8_t got[48];
    size_t  glen = 0;
    ALP_ASSERT_EQ_INT(alp_hash_finish(h, got, sizeof(got), &glen), ALP_OK);
    ALP_ASSERT_EQ_INT(glen, 48);
}

static void test_sha512_length(void)
{
    alp_hash_t *h = alp_hash_open(ALP_HASH_SHA512);
    ALP_ASSERT_TRUE(h != NULL);
    ALP_ASSERT_EQ_INT(alp_hash_update(h, (const uint8_t *)"x", 1), ALP_OK);
    uint8_t got[64];
    size_t  glen = 0;
    ALP_ASSERT_EQ_INT(alp_hash_finish(h, got, sizeof(got), &glen), ALP_OK);
    ALP_ASSERT_EQ_INT(glen, 64);
}

static void test_hash_short_digest_buffer_refused(void)
{
    alp_hash_t *h = alp_hash_open(ALP_HASH_SHA256);
    ALP_ASSERT_TRUE(h != NULL);
    uint8_t short_buf[16]; /* SHA-256 needs 32. */
    size_t  glen = 99;
    ALP_ASSERT_EQ_INT(alp_hash_finish(h, short_buf, sizeof(short_buf), &glen),
                      ALP_ERR_INVAL);
    ALP_ASSERT_EQ_INT(glen, 0);
    alp_hash_close(h);
}

static void test_hash_invalid_alg_returns_null(void)
{
    alp_hash_t *h = alp_hash_open((alp_hash_alg_t)99);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_hash_update_on_null_returns_not_ready(void)
{
    ALP_ASSERT_EQ_INT(alp_hash_update(NULL, (const uint8_t *)"x", 1),
                      ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* AEAD                                                                */
/* ------------------------------------------------------------------ */

static void test_aes_128_gcm_roundtrip(void)
{
    uint8_t key[16];
    uint8_t iv[12];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)i;
    for (size_t i = 0; i < sizeof(iv);  ++i) iv[i]  = (uint8_t)(i + 100);
    const uint8_t plain[] = "secret payload";
    const uint8_t aad[]   = "metadata";

    alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));
    ALP_ASSERT_TRUE(a != NULL);

    uint8_t cipher[sizeof(plain)];
    uint8_t tag[16];
    ALP_ASSERT_EQ_INT(alp_aead_encrypt(a, iv, sizeof(iv),
                                        aad, sizeof(aad) - 1,
                                        plain, sizeof(plain) - 1,
                                        cipher, tag, sizeof(tag)),
                      ALP_OK);

    uint8_t recovered[sizeof(plain)];
    memset(recovered, 0, sizeof(recovered));
    ALP_ASSERT_EQ_INT(alp_aead_decrypt(a, iv, sizeof(iv),
                                        aad, sizeof(aad) - 1,
                                        cipher, sizeof(plain) - 1,
                                        tag, sizeof(tag),
                                        recovered),
                      ALP_OK);
    ALP_ASSERT_TRUE(memcmp(recovered, plain, sizeof(plain) - 1) == 0);

    alp_aead_close(a);
}

static void test_chacha20_poly1305_roundtrip(void)
{
    uint8_t key[32];
    uint8_t iv[12];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i * 3);
    for (size_t i = 0; i < sizeof(iv);  ++i) iv[i]  = (uint8_t)(i * 7);
    const uint8_t plain[] = "another payload";

    alp_aead_t *a = alp_aead_open(ALP_AEAD_CHACHA20_POLY1305, key, sizeof(key));
    ALP_ASSERT_TRUE(a != NULL);

    uint8_t cipher[sizeof(plain)];
    uint8_t tag[16];
    ALP_ASSERT_EQ_INT(alp_aead_encrypt(a, iv, sizeof(iv),
                                        NULL, 0,
                                        plain, sizeof(plain) - 1,
                                        cipher, tag, sizeof(tag)),
                      ALP_OK);

    uint8_t recovered[sizeof(plain)];
    memset(recovered, 0, sizeof(recovered));
    ALP_ASSERT_EQ_INT(alp_aead_decrypt(a, iv, sizeof(iv),
                                        NULL, 0,
                                        cipher, sizeof(plain) - 1,
                                        tag, sizeof(tag),
                                        recovered),
                      ALP_OK);
    ALP_ASSERT_TRUE(memcmp(recovered, plain, sizeof(plain) - 1) == 0);

    alp_aead_close(a);
}

static void test_aead_tag_mismatch_rejects(void)
{
    /* Encrypt, then corrupt one byte of the tag.  Decrypt must
     * return ALP_ERR_IO per the header contract.  This is the
     * integrity-failure path. */
    uint8_t key[16] = {0};
    uint8_t iv[12]  = {0};
    const uint8_t plain[] = "ABCDEFGHIJ";

    alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));
    ALP_ASSERT_TRUE(a != NULL);
    uint8_t cipher[sizeof(plain)];
    uint8_t tag[16];
    ALP_ASSERT_EQ_INT(alp_aead_encrypt(a, iv, sizeof(iv),
                                        NULL, 0,
                                        plain, sizeof(plain) - 1,
                                        cipher, tag, sizeof(tag)),
                      ALP_OK);
    tag[0] ^= 0xFFu;
    uint8_t recovered[sizeof(plain)];
    ALP_ASSERT_EQ_INT(alp_aead_decrypt(a, iv, sizeof(iv),
                                        NULL, 0,
                                        cipher, sizeof(plain) - 1,
                                        tag, sizeof(tag),
                                        recovered),
                      ALP_ERR_IO);
    alp_aead_close(a);
}

static void test_aead_bad_key_length_refused(void)
{
    uint8_t short_key[8]; /* AES-128 needs 16. */
    alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, short_key, sizeof(short_key));
    ALP_ASSERT_NULL(a);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_aead_null_key_refused(void)
{
    alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, NULL, 16);
    ALP_ASSERT_NULL(a);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_aead_unsupported_alg_refused(void)
{
    uint8_t key[16] = {0};
    alp_aead_t *a = alp_aead_open((alp_aead_alg_t)99, key, sizeof(key));
    ALP_ASSERT_NULL(a);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_aead_close_null_is_safe(void)
{
    alp_aead_close(NULL);
    alp_hash_close(NULL);
    ALP_TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* TRNG                                                                */
/* ------------------------------------------------------------------ */

static void test_random_bytes_zero_len_is_ok(void)
{
    ALP_ASSERT_EQ_INT(alp_random_bytes(NULL, 0), ALP_OK);
}

static void test_random_bytes_null_with_nonzero_refused(void)
{
    ALP_ASSERT_EQ_INT(alp_random_bytes(NULL, 16), ALP_ERR_INVAL);
}

static void test_random_bytes_fills_buffer(void)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    ALP_ASSERT_EQ_INT(alp_random_bytes(buf, sizeof(buf)), ALP_OK);
    /* Vanishingly unlikely the OS RNG returns 64 zero bytes -- if it
     * does, something's seriously wrong with the entropy source. */
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != 0) { all_zero = 0; break; }
    }
    ALP_ASSERT_TRUE(all_zero == 0);
}

int main(void)
{
    test_sha256_known_answer();
    test_sha384_length();
    test_sha512_length();
    test_hash_short_digest_buffer_refused();
    test_hash_invalid_alg_returns_null();
    test_hash_update_on_null_returns_not_ready();

    test_aes_128_gcm_roundtrip();
    test_chacha20_poly1305_roundtrip();
    test_aead_tag_mismatch_rejects();
    test_aead_bad_key_length_refused();
    test_aead_null_key_refused();
    test_aead_unsupported_alg_refused();
    test_aead_close_null_is_safe();

    test_random_bytes_zero_len_is_ok();
    test_random_bytes_null_with_nonzero_refused();
    test_random_bytes_fills_buffer();

    ALP_TEST_SUMMARY();
}
