/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file security.h
 * @brief ALP SDK cryptography surface (re-export of MbedTLS + HW-accelerator hooks).
 *
 * v0.3 deliverable.  v0.1 ships only the public surface; every entry
 * point returns ALP_ERR_NOSUPPORT.
 *
 * Backends:
 *   - Zephyr   : MbedTLS (PSA Crypto API) on top of the SoC's TRNG /
 *                AES / SHA accelerator where present (Alif Ensemble +
 *                Renesas RZ/V2N both expose accelerated paths).
 *   - Yocto    : OpenSSL or MbedTLS via the distro packaging.
 *   - Baremetal: Software-only MbedTLS; no TLS/X.509 in v0.1 baremetal.
 *
 * The shape is deliberately small — hash, AEAD, signed PK, and TRNG are
 * the primitives `<alp/iot.h>` (TLS) and `<alp/ble.h>` (LE Secure
 * Connections) need; full asym-key generation lands in v0.4.
 */

#ifndef ALP_SECURITY_H
#define ALP_SECURITY_H

#include <stdint.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    ALP_HASH_SHA256 = 0,
    ALP_HASH_SHA384 = 1,
    ALP_HASH_SHA512 = 2
} alp_hash_alg_t;

typedef struct alp_hash alp_hash_t;

alp_hash_t  *alp_hash_open(alp_hash_alg_t alg);
alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *data, size_t len);
alp_status_t alp_hash_finish(alp_hash_t *h, uint8_t *digest_out, size_t digest_cap,
                             size_t *digest_len);
void         alp_hash_close(alp_hash_t *h);

/* ------------------------------------------------------------------ */
/* AEAD                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    ALP_AEAD_AES_128_GCM      = 0,
    ALP_AEAD_AES_256_GCM      = 1,
    ALP_AEAD_CHACHA20_POLY1305 = 2
} alp_aead_alg_t;

typedef struct alp_aead alp_aead_t;

alp_aead_t  *alp_aead_open(alp_aead_alg_t alg,
                           const uint8_t *key, size_t key_len);

/** Authenticated encrypt.  @p tag_out must be ≥ 16 bytes. */
alp_status_t alp_aead_encrypt(alp_aead_t *a,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher_out,
                              uint8_t *tag_out, size_t tag_len);

/** Authenticated decrypt.  Returns ALP_ERR_IO on tag-mismatch. */
alp_status_t alp_aead_decrypt(alp_aead_t *a,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *cipher, size_t cipher_len,
                              const uint8_t *tag, size_t tag_len,
                              uint8_t *plain_out);

void         alp_aead_close(alp_aead_t *a);

/* ------------------------------------------------------------------ */
/* TRNG                                                                */
/* ------------------------------------------------------------------ */

/** Fill @p out with @p len cryptographically random bytes.  Routes to
 *  the SoC's TRNG when present, otherwise to MbedTLS's CTR_DRBG seeded
 *  from a platform entropy source. */
alp_status_t alp_random_bytes(uint8_t *out, size_t len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_SECURITY_H */
