/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/security.h> -- MbedTLS PSA Crypto.
 *
 * The wrapper rides on PSA Crypto so MbedTLS automatically routes
 * to the SoC's HW accelerator when available (Alif Ensemble's
 * crypto subsystem on E7/E8, Renesas RZ/V2N's RSIP) without
 * code changes here.  Software fall-back uses MbedTLS's reference
 * implementations.
 *
 * Gated on CONFIG_ALP_SDK_SECURITY which depends on MBEDTLS +
 * MBEDTLS_PSA_CRYPTO_C.  When OFF the wrapper falls back to
 * NULL-with-NOSUPPORT.
 *
 * V2N TRNG entropy source.  The mbedtls profile (under
 * metadata/library-profiles/mbedtls/) sets MBEDTLS_NO_PLATFORM_ENTROPY,
 * so mbedtls's entropy module asks the SDK to supply a hardware-poll
 * callback.  On V2N (CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY=y), we
 * route that callback through the supervisor's GD32G553 TRNG so the
 * portable alp_random_bytes() transparently picks up true randomness
 * the first time PSA's DRBG seeds itself.  The wire-level chip name
 * stays hidden behind the supervisor singleton (per
 * memory/feedback_portable_hw_offload_with_sw_fallback.md).
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/security.h"
#include "handles.h"

#if defined(CONFIG_ALP_SDK_SECURITY)
#include <psa/crypto.h>
#endif

#if defined(CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY)
#include <mbedtls/entropy.h>

#include "alp/chips/gd32g553.h"
#include "v2n_supervisor.h"
#endif

#ifndef CONFIG_ALP_SDK_MAX_HASH_HANDLES
#define CONFIG_ALP_SDK_MAX_HASH_HANDLES 4
#endif
#ifndef CONFIG_ALP_SDK_MAX_AEAD_HANDLES
#define CONFIG_ALP_SDK_MAX_AEAD_HANDLES 4
#endif

/* ------------------------------------------------------------------ */
/* Internal handle structures                                          */
/* ------------------------------------------------------------------ */

struct alp_hash {
    bool in_use;
#if defined(CONFIG_ALP_SDK_SECURITY)
    psa_hash_operation_t op;
    alp_hash_alg_t       alg;
#endif
};

struct alp_aead {
    bool in_use;
#if defined(CONFIG_ALP_SDK_SECURITY)
    psa_key_id_t   key_id;
    alp_aead_alg_t alg;
#endif
};

#if defined(CONFIG_ALP_SDK_SECURITY)
static struct alp_hash  g_hash_pool[CONFIG_ALP_SDK_MAX_HASH_HANDLES];
static struct alp_aead  g_aead_pool[CONFIG_ALP_SDK_MAX_AEAD_HANDLES];
static bool             g_psa_inited;

static struct alp_hash *hash_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_hash_pool); ++i) {
        if (!g_hash_pool[i].in_use) {
            memset(&g_hash_pool[i], 0, sizeof(g_hash_pool[i]));
            g_hash_pool[i].in_use = true;
            return &g_hash_pool[i];
        }
    }
    return NULL;
}

static struct alp_aead *aead_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_aead_pool); ++i) {
        if (!g_aead_pool[i].in_use) {
            memset(&g_aead_pool[i], 0, sizeof(g_aead_pool[i]));
            g_aead_pool[i].in_use = true;
            return &g_aead_pool[i];
        }
    }
    return NULL;
}

static psa_algorithm_t to_psa_hash(alp_hash_alg_t a)
{
    switch (a) {
    case ALP_HASH_SHA256:
        return PSA_ALG_SHA_256;
    case ALP_HASH_SHA384:
        return PSA_ALG_SHA_384;
    case ALP_HASH_SHA512:
        return PSA_ALG_SHA_512;
    default:
        return 0;
    }
}

static alp_status_t psa_to_alp(psa_status_t st)
{
    switch (st) {
    case PSA_SUCCESS:
        return ALP_OK;
    case PSA_ERROR_INVALID_ARGUMENT:
        return ALP_ERR_INVAL;
    case PSA_ERROR_NOT_PERMITTED:
    case PSA_ERROR_NOT_SUPPORTED:
        return ALP_ERR_NOSUPPORT;
    case PSA_ERROR_INSUFFICIENT_MEMORY:
        return ALP_ERR_NOMEM;
    case PSA_ERROR_INVALID_SIGNATURE:
        return ALP_ERR_IO;
    default:
        return ALP_ERR_IO;
    }
}

static alp_status_t ensure_psa(void)
{
    if (g_psa_inited) return ALP_OK;
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) return psa_to_alp(st);
    g_psa_inited = true;
    return ALP_OK;
}

#endif /* CONFIG_ALP_SDK_SECURITY */

/* ================================================================== */
/* Hash                                                                */
/* ================================================================== */

alp_hash_t *alp_hash_open(alp_hash_alg_t alg)
{
    alp_z_clear_last_error();
#if defined(CONFIG_ALP_SDK_SECURITY)
    if (ensure_psa() != ALP_OK) {
        alp_z_set_last_error(ALP_ERR_IO);
        return NULL;
    }
    psa_algorithm_t psa_alg = to_psa_hash(alg);
    if (psa_alg == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    struct alp_hash *h = hash_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->op           = psa_hash_operation_init();
    h->alg          = alg;
    psa_status_t st = psa_hash_setup(&h->op, psa_alg);
    if (st != PSA_SUCCESS) {
        h->in_use = false;
        alp_z_set_last_error(psa_to_alp(st));
        return NULL;
    }
    return h;
#else
    (void)alg;
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *data, size_t len)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_SECURITY)
    return psa_to_alp(psa_hash_update(&h->op, data, len));
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_hash_finish(alp_hash_t *h, uint8_t *digest_out, size_t digest_cap,
                             size_t *digest_len)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (digest_out == NULL || digest_cap == 0) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_SECURITY)
    size_t       got = 0;
    psa_status_t st  = psa_hash_finish(&h->op, digest_out, digest_cap, &got);
    if (digest_len != NULL) *digest_len = got;
    h->in_use = false;
    return psa_to_alp(st);
#else
    if (digest_len != NULL) *digest_len = 0;
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_hash_close(alp_hash_t *h)
{
    if (h == NULL || !h->in_use) return;
#if defined(CONFIG_ALP_SDK_SECURITY)
    (void)psa_hash_abort(&h->op);
#endif
    h->in_use = false;
}

/* ================================================================== */
/* AEAD                                                                */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_SECURITY)

static alp_status_t aead_alg_meta(alp_aead_alg_t a, psa_algorithm_t *out_alg,
                                  psa_key_type_t *out_kt, size_t *out_key_bits)
{
    switch (a) {
    case ALP_AEAD_AES_128_GCM:
        *out_alg      = PSA_ALG_GCM;
        *out_kt       = PSA_KEY_TYPE_AES;
        *out_key_bits = 128;
        return ALP_OK;
    case ALP_AEAD_AES_256_GCM:
        *out_alg      = PSA_ALG_GCM;
        *out_kt       = PSA_KEY_TYPE_AES;
        *out_key_bits = 256;
        return ALP_OK;
    case ALP_AEAD_CHACHA20_POLY1305:
        *out_alg      = PSA_ALG_CHACHA20_POLY1305;
        *out_kt       = PSA_KEY_TYPE_CHACHA20;
        *out_key_bits = 256;
        return ALP_OK;
    default:
        return ALP_ERR_INVAL;
    }
}

#endif /* CONFIG_ALP_SDK_SECURITY */

alp_aead_t *alp_aead_open(alp_aead_alg_t alg, const uint8_t *key, size_t key_len)
{
    alp_z_clear_last_error();
    if (key == NULL || key_len == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
#if defined(CONFIG_ALP_SDK_SECURITY)
    if (ensure_psa() != ALP_OK) {
        alp_z_set_last_error(ALP_ERR_IO);
        return NULL;
    }
    psa_algorithm_t psa_alg;
    psa_key_type_t  kt;
    size_t          key_bits;
    if (aead_alg_meta(alg, &psa_alg, &kt, &key_bits) != ALP_OK) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (key_len * 8 != key_bits) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    struct alp_aead *a = aead_pool_acquire();
    if (a == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, psa_alg);
    psa_set_key_type(&attr, kt);
    psa_set_key_bits(&attr, (size_t)key_bits);

    psa_status_t st = psa_import_key(&attr, key, key_len, &a->key_id);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        a->in_use = false;
        alp_z_set_last_error(psa_to_alp(st));
        return NULL;
    }
    a->alg = alg;
    return a;
#else
    (void)alg;
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_aead_encrypt(alp_aead_t *a, const uint8_t *iv, size_t iv_len, const uint8_t *aad,
                              size_t aad_len, const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher_out, uint8_t *tag_out, size_t tag_len)
{
    if (a == NULL || !a->in_use) return ALP_ERR_NOT_READY;
    if (iv == NULL || cipher_out == NULL || tag_out == NULL) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_SECURITY)
    psa_algorithm_t psa_alg;
    psa_key_type_t  kt;
    size_t          kb;
    (void)aead_alg_meta(a->alg, &psa_alg, &kt, &kb);

    /* PSA's aead_encrypt produces ciphertext || tag in one buffer.  We
     * copy the tag tail out into the caller's tag buffer to match the
     * public API's tag-out separation. */
    uint8_t *blob_out      = cipher_out; /* overlap is fine -- PSA's API supports it */
    size_t   blob_out_size = plain_len + 16;
    size_t   produced      = 0;

    /* Need a scratch big enough for ciphertext || tag.  Allocate on
     * the stack for v0.3 -- caller's plaintext is bounded.  Heap
     * fall-back lands in v0.3.x for very large blobs. */
    if (plain_len > 4096) return ALP_ERR_NOSUPPORT;
    uint8_t      scratch[4096 + 16];
    psa_status_t st = psa_aead_encrypt(a->key_id, psa_alg, iv, iv_len, aad, aad_len, plain,
                                       plain_len, scratch, sizeof(scratch), &produced);
    (void)blob_out;
    (void)blob_out_size;
    if (st != PSA_SUCCESS) return psa_to_alp(st);

    if (produced < tag_len || produced - tag_len != plain_len) return ALP_ERR_IO;
    memcpy(cipher_out, scratch, plain_len);
    memcpy(tag_out, scratch + plain_len, tag_len);
    return ALP_OK;
#else
    (void)iv;
    (void)iv_len;
    (void)aad;
    (void)aad_len;
    (void)plain;
    (void)plain_len;
    (void)tag_len;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_aead_decrypt(alp_aead_t *a, const uint8_t *iv, size_t iv_len, const uint8_t *aad,
                              size_t aad_len, const uint8_t *cipher, size_t cipher_len,
                              const uint8_t *tag, size_t tag_len, uint8_t *plain_out)
{
    if (a == NULL || !a->in_use) return ALP_ERR_NOT_READY;
    if (iv == NULL || cipher == NULL || tag == NULL || plain_out == NULL) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_SECURITY)
    psa_algorithm_t psa_alg;
    psa_key_type_t  kt;
    size_t          kb;
    (void)aead_alg_meta(a->alg, &psa_alg, &kt, &kb);

    if (cipher_len > 4096) return ALP_ERR_NOSUPPORT;
    uint8_t scratch[4096 + 16];
    if (cipher_len + tag_len > sizeof(scratch)) return ALP_ERR_NOSUPPORT;
    memcpy(scratch, cipher, cipher_len);
    memcpy(scratch + cipher_len, tag, tag_len);

    size_t       produced = 0;
    psa_status_t st       = psa_aead_decrypt(a->key_id, psa_alg, iv, iv_len, aad, aad_len, scratch,
                                             cipher_len + tag_len, plain_out, cipher_len, &produced);
    return psa_to_alp(st);
#else
    (void)iv;
    (void)iv_len;
    (void)aad;
    (void)aad_len;
    (void)cipher;
    (void)cipher_len;
    (void)tag;
    (void)tag_len;
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_aead_close(alp_aead_t *a)
{
    if (a == NULL || !a->in_use) return;
#if defined(CONFIG_ALP_SDK_SECURITY)
    (void)psa_destroy_key(a->key_id);
#endif
    a->in_use = false;
}

/* ================================================================== */
/* TRNG                                                                */
/* ================================================================== */

alp_status_t alp_random_bytes(uint8_t *out, size_t len)
{
    if (out == NULL || len == 0) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_SECURITY)
    if (ensure_psa() != ALP_OK) return ALP_ERR_IO;
    return psa_to_alp(psa_generate_random(out, len));
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

/* ================================================================== */
/* MbedTLS hardware entropy poll -- V2N GD32G553 TRNG                  */
/*                                                                     */
/* The SDK's mbedtls profile sets MBEDTLS_NO_PLATFORM_ENTROPY (see     */
/* metadata/library-profiles/mbedtls/mbedtls_config.h), so mbedtls     */
/* expects the integrator to supply mbedtls_hardware_poll().  On the   */
/* V2N family we drain bytes from the GD32G553's NIST SP800-90B        */
/* pre-certified TRNG through the supervisor singleton, chunking at    */
/* the bridge's per-call ceiling.  PSA Crypto's CTR_DRBG seeds itself  */
/* from this source on first use (and reseeds periodically), so the    */
/* portable alp_random_bytes() benefits transparently without app      */
/* code mentioning the GD32 name.                                      */
/*                                                                     */
/* mbedtls contract: return 0 on success, MBEDTLS_ERR_ENTROPY_SOURCE_  */
/* FAILED on a hard failure.  Partial fills are allowed (the caller    */
/* loops on *olen) but we always try to drain the full request -- the  */
/* GD32 TRNG is fast enough that there's no benefit to short returns.  */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY)

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;

    if (output == NULL || olen == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    *olen = 0u;
    if (len == 0u) return 0;

    /* Drain in <= GD32G553_BRIDGE_TRNG_MAX_BYTES chunks under a single
     * supervisor acquire each.  Holding the mutex across the whole loop
     * would serialise other peripheral ops behind the entropy fill;
     * one chunk at a time keeps the bridge mutex contention windows
     * short (~1 ms typical per chunk on SPI, ~5 ms on I2C). */
    size_t produced = 0u;
    while (produced < len) {
        const size_t remaining = len - produced;
        const size_t chunk     = (remaining > (size_t)GD32G553_BRIDGE_TRNG_MAX_BYTES)
                                     ? (size_t)GD32G553_BRIDGE_TRNG_MAX_BYTES
                                     : remaining;

        gd32g553_t  *ctx = NULL;
        alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
        if (s != ALP_OK) {
            /* If we produced at least one chunk already, surface a
             * short return so mbedtls can fold it into its entropy
             * accumulator and retry later.  A zero-progress failure
             * is reported as a hard source-failed. */
            if (produced > 0u) {
                *olen = produced;
                return 0;
            }
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        s = gd32g553_trng_read(ctx, output + produced, chunk);
        alp_z_v2n_supervisor_release();

        if (s != ALP_OK) {
            if (produced > 0u) {
                *olen = produced;
                return 0;
            }
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        produced += chunk;
    }

    *olen = produced;
    return 0;
}

#endif /* CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY */
