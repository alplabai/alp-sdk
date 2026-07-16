/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file security.h
 * @brief Alp SDK cryptography surface (re-export of MbedTLS + HW-accelerator hooks).
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
 *
 * Typical AEAD usage:
 * @code
 *     uint8_t key[16] = {...};
 *     alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof key);
 *     uint8_t iv[12], cipher[64], tag[16];
 *     alp_random_bytes(iv, sizeof iv);
 *     alp_aead_encrypt(a, iv, sizeof iv, NULL, 0,
 *                      plaintext, sizeof plaintext,
 *                      cipher, tag, sizeof tag);
 * @endcode
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.3 MbedTLS PSA Crypto wrapper.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_SECURITY_H
#define ALP_SECURITY_H

#include <stdint.h>
#include <stddef.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */

/** Supported hash algorithms.  Routes to HW where the SoC supports it. */
typedef enum { ALP_HASH_SHA256 = 0, ALP_HASH_SHA384 = 1, ALP_HASH_SHA512 = 2 } alp_hash_alg_t;

/** Opaque hash context.  Allocate via @ref alp_hash_open. */
typedef struct alp_hash alp_hash_t;

/**
 * @brief Begin a hash computation.
 *
 * @param[in] alg  Algorithm choice.
 * @return Open context on success; NULL if the backend doesn't
 *         support the requested algorithm.
 */
alp_hash_t *alp_hash_open(alp_hash_alg_t alg);

/**
 * @brief Feed @p len bytes of data into the running digest.
 *
 * Idempotent across multiple calls; the final digest is computed
 * by @ref alp_hash_finish.
 *
 * @param[in] h     Open context.
 * @param[in] data  Source bytes.
 * @param[in] len   Source length.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL.
 */
alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *data, size_t len);

/**
 * @brief Finalise the digest and write it to @p digest_out.
 *
 * Handle lifecycle contract (GHSA-92c3-v48m-m5gg): **only @c ALP_OK
 * implicitly closes @p h.**  Output length depends on alg: SHA-256 =
 * 32 B, SHA-384 = 48 B, SHA-512 = 64 B.
 *
 *   - @c ALP_OK: the digest is written and @p h is closed for you --
 *     do not call @ref alp_hash_close on it afterwards.
 *   - @c ALP_ERR_INVAL because @p digest_cap is smaller than the
 *     algorithm's digest length: @p digest_len (if non-NULL) is set
 *     to the required length and @p h is left open and unchanged --
 *     call again with a large-enough buffer, or call
 *     @ref alp_hash_close explicitly.
 *   - Any other failure: @p h is left safely closeable -- always
 *     follow up with @ref alp_hash_close (a redundant close is a
 *     harmless no-op).
 *
 * @param[in]  h           Open context.
 * @param[out] digest_out  Destination buffer.
 * @param[in]  digest_cap  Capacity of @p digest_out.
 * @param[out] digest_len  Receives the bytes written on success, or the
 *                         required digest length on a too-small-buffer
 *                         failure.  May be NULL.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL.
 */
alp_status_t
alp_hash_finish(alp_hash_t *h, uint8_t *digest_out, size_t digest_cap, size_t *digest_len);

/** @brief Release without finalising.  Use after a partial computation. */
void alp_hash_close(alp_hash_t *h);

/**
 * @brief Per-instance capability descriptor for an opened hash context.
 *
 * Populated by the backend's open path; returned by-pointer so callers
 * can inspect @c flags / etc. without a copy.  The pointer remains
 * valid until @ref alp_hash_close or @ref alp_hash_finish releases the
 * handle.
 *
 * @param[in] h  Open hash context, or NULL.
 * @return Pointer to the cached capabilities, or NULL when @p h is NULL.
 */
const alp_capabilities_t *alp_hash_capabilities(const alp_hash_t *h);

/* ------------------------------------------------------------------ */
/* AEAD                                                                */
/* ------------------------------------------------------------------ */

/** Supported authenticated-encryption algorithms. */
typedef enum {
	ALP_AEAD_AES_128_GCM       = 0,
	ALP_AEAD_AES_256_GCM       = 1,
	ALP_AEAD_CHACHA20_POLY1305 = 2
} alp_aead_alg_t;

/** Opaque AEAD context.  Allocate via @ref alp_aead_open. */
typedef struct alp_aead alp_aead_t;

/**
 * @brief Acquire an AEAD context with the given key.
 *
 * @param[in] alg      Algorithm choice.
 * @param[in] key      Key material.  Length must match the algorithm
 *                     (16 B for AES-128, 32 B for AES-256 / ChaCha20).
 * @param[in] key_len  Key length.
 * @return Open context, or NULL on bad key length / unsupported alg.
 */
alp_aead_t *alp_aead_open(alp_aead_alg_t alg, const uint8_t *key, size_t key_len);

/**
 * @brief Authenticated encrypt.
 *
 * @param[in]  a           AEAD context.
 * @param[in]  iv          Initialisation vector.  AES-GCM: 12 B; ChaCha20: 12 B.
 * @param[in]  iv_len      IV length.
 * @param[in]  aad         Associated data (authenticated, not encrypted).
 *                         May be NULL with @p aad_len = 0.
 * @param[in]  aad_len     AAD length.
 * @param[in]  plain       Plaintext input.
 * @param[in]  plain_len   Plaintext length.
 * @param[out] cipher_out  Ciphertext destination.  Same size as plaintext.
 * @param[out] tag_out     Authentication tag destination.
 * @param[in]  tag_len     Tag length.  Must be exactly 16 B -- the only
 *                         length every backend round-trips for the
 *                         algorithms above (AES-128/256-GCM,
 *                         ChaCha20-Poly1305); any other value is
 *                         rejected with @c ALP_ERR_INVAL before any
 *                         crypto runs.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL.
 */
alp_status_t alp_aead_encrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         iv_len,
                              const uint8_t *aad,
                              size_t         aad_len,
                              const uint8_t *plain,
                              size_t         plain_len,
                              uint8_t       *cipher_out,
                              uint8_t       *tag_out,
                              size_t         tag_len);

/**
 * @brief Authenticated decrypt.
 *
 * @param[in]  a           AEAD context.
 * @param[in]  iv          IV used for encryption.
 * @param[in]  iv_len      IV length.
 * @param[in]  aad         Associated data.
 * @param[in]  aad_len     AAD length.
 * @param[in]  cipher      Ciphertext input.
 * @param[in]  cipher_len  Ciphertext length.
 * @param[in]  tag         Authentication tag from encryption.
 * @param[in]  tag_len     Tag length.  Must be exactly 16 B -- see
 *                         @ref alp_aead_encrypt.
 * @param[out] plain_out   Plaintext destination.  Same size as ciphertext.
 *                         NOTE: during the call, @p plain_out transiently
 *                         holds plaintext that has not yet been verified
 *                         against @p tag (the backend streams it in before
 *                         the tag check completes) -- treat any bytes
 *                         observed there before this function returns as
 *                         unauthenticated, which matters for AMP shared-
 *                         memory buffers another core could poll.  Once
 *                         the backend has been invoked, any non-@c ALP_OK
 *                         return wipes the whole buffer per the discard
 *                         contract below.  Early parameter-validation
 *                         failures (@c ALP_ERR_INVAL / @c ALP_ERR_NOT_READY,
 *                         returned before the backend is ever called)
 *                         leave @p plain_out untouched -- there is nothing
 *                         of ours to discard in that case.
 * @return ALP_OK on success;
 *         ALP_ERR_IO on tag-mismatch (the message has been tampered
 *         with — @p plain_out content is undefined and MUST be discarded);
 *         ALP_ERR_NOT_READY / ALP_ERR_INVAL otherwise.
 */
alp_status_t alp_aead_decrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         iv_len,
                              const uint8_t *aad,
                              size_t         aad_len,
                              const uint8_t *cipher,
                              size_t         cipher_len,
                              const uint8_t *tag,
                              size_t         tag_len,
                              uint8_t       *plain_out);

/** @brief Release the AEAD context.  Wipes key material. */
void alp_aead_close(alp_aead_t *a);

/**
 * @brief Per-instance capability descriptor for an opened AEAD context.
 *
 * Populated by the backend's open path; returned by-pointer so callers
 * can inspect @c flags / etc. without a copy.  The pointer remains
 * valid until @ref alp_aead_close releases the handle.
 *
 * No matching getter exists for @ref alp_random_bytes — random is a
 * stateless op with no handle to attach capabilities to.
 *
 * @param[in] a  Open AEAD context, or NULL.
 * @return Pointer to the cached capabilities, or NULL when @p a is NULL.
 */
const alp_capabilities_t *alp_aead_capabilities(const alp_aead_t *a);

/* ------------------------------------------------------------------ */
/* TRNG                                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Fill @p out with @p len cryptographically random bytes.
 *
 * Routes to the SoC's TRNG when present, otherwise to MbedTLS's
 * CTR_DRBG seeded from a platform entropy source.  Suitable for
 * key generation, IVs, and nonces.  **Not** for non-cryptographic
 * randomness — use the system PRNG for that.
 *
 * @param[out] out  Destination buffer.
 * @param[in]  len  Bytes to generate.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO
 *         (entropy source failed).
 */
alp_status_t alp_random_bytes(uint8_t *out, size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_SECURITY_H */
