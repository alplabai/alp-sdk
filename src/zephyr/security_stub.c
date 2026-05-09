/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/security.h>.  Every entry point returns
 * ALP_ERR_NOSUPPORT; alp_hash_open / alp_aead_open return NULL.
 * Real MbedTLS + accelerator backend lands in v0.3 (see VERSIONS.md).
 */

#include "alp/security.h"

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */

alp_hash_t *alp_hash_open(alp_hash_alg_t alg) {
    (void)alg;
    return NULL;
}

alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *data, size_t len) {
    (void)h; (void)data; (void)len;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_hash_finish(alp_hash_t *h, uint8_t *digest_out, size_t digest_cap,
                             size_t *digest_len) {
    (void)h; (void)digest_out; (void)digest_cap;
    if (digest_len != NULL) *digest_len = 0;
    return ALP_ERR_NOSUPPORT;
}

void alp_hash_close(alp_hash_t *h) {
    (void)h;
}

/* ------------------------------------------------------------------ */
/* AEAD                                                                */
/* ------------------------------------------------------------------ */

alp_aead_t *alp_aead_open(alp_aead_alg_t alg,
                          const uint8_t *key, size_t key_len) {
    (void)alg; (void)key; (void)key_len;
    return NULL;
}

alp_status_t alp_aead_encrypt(alp_aead_t *a,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher_out,
                              uint8_t *tag_out, size_t tag_len) {
    (void)a; (void)iv; (void)iv_len; (void)aad; (void)aad_len;
    (void)plain; (void)plain_len; (void)cipher_out;
    (void)tag_out; (void)tag_len;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_aead_decrypt(alp_aead_t *a,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *cipher, size_t cipher_len,
                              const uint8_t *tag, size_t tag_len,
                              uint8_t *plain_out) {
    (void)a; (void)iv; (void)iv_len; (void)aad; (void)aad_len;
    (void)cipher; (void)cipher_len; (void)tag; (void)tag_len;
    (void)plain_out;
    return ALP_ERR_NOSUPPORT;
}

void alp_aead_close(alp_aead_t *a) {
    (void)a;
}

/* ------------------------------------------------------------------ */
/* TRNG                                                                */
/* ------------------------------------------------------------------ */

alp_status_t alp_random_bytes(uint8_t *out, size_t len) {
    (void)out; (void)len;
    return ALP_ERR_NOSUPPORT;
}
