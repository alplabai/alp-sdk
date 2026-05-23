/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software security fallback.  Wildcard backend at priority 0 --
 * picked only when no higher-priority hardware-aware security
 * backend is linked into the build (the typical native_sim
 * trimmed-image case, or any build with CONFIG_ALP_SDK_SECURITY=n).
 *
 * Contract:
 *   - hash_open / aead_open  -> ALP_OK (the dispatcher hands out a
 *     handle the caller can subsequently address; no underlying PSA
 *     state stands behind it)
 *   - hash_update / hash_finish / aead_encrypt / aead_decrypt
 *     -> ALP_ERR_NOT_IMPLEMENTED
 *   - hash_close / aead_close -> no-op
 *   - random_bytes -> ALP_OK; fills the buffer with a deterministic
 *     counter+0xAA pattern so tests that need bytes get something
 *     predictable.  NOT cryptographically random -- the wildcard
 *     fallback exists for native_sim portability, not security.
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~300 B, zero RAM (no per-handle backend state --
 *      every state->be_data is left NULL).  No MbedTLS / PSA Crypto
 *      linkage required, so this backend compiles cleanly on
 *      native_sim trimmed-image builds where MbedTLS is absent.
 * @par Performance: O(1) per call; every op short-circuits to
 *      ALP_OK / ALP_ERR_NOT_IMPLEMENTED with no library touch.
 *      random_bytes is O(len) -- one store per byte through the
 *      deterministic generator; bit-identical to a portable
 *      counter+xor reference, which is exactly the property the
 *      unit tests in tests/unit/security_registry/ exercise.
 *      All ops are reentrant and lock-free.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/security.h>

#include "security_ops.h"

/* ---------- Hash ---------- */

static alp_status_t sw_hash_open(alp_hash_alg_t alg,
                                 alp_hash_backend_state_t *state,
                                 alp_capabilities_t *caps_out)
{
    state->alg      = alg;
    state->be_data  = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_hash_update(alp_hash_backend_state_t *state,
                                   const uint8_t *data, size_t len)
{
    (void)state;
    (void)data;
    (void)len;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_hash_finish(alp_hash_backend_state_t *state,
                                   uint8_t *digest_out, size_t digest_cap,
                                   size_t *digest_len)
{
    (void)state;
    (void)digest_out;
    (void)digest_cap;
    if (digest_len != NULL) *digest_len = 0;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static void sw_hash_close(alp_hash_backend_state_t *state)
{
    (void)state;
}

/* ---------- AEAD ---------- */

static alp_status_t sw_aead_open(alp_aead_alg_t alg,
                                 const uint8_t *key, size_t key_len,
                                 alp_aead_backend_state_t *state,
                                 alp_capabilities_t *caps_out)
{
    (void)key;
    (void)key_len;
    state->alg      = alg;
    state->be_data  = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_aead_encrypt(alp_aead_backend_state_t *state,
                                    const uint8_t *iv, size_t iv_len,
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *plain, size_t plain_len,
                                    uint8_t *cipher_out,
                                    uint8_t *tag_out, size_t tag_len)
{
    (void)state;
    (void)iv;
    (void)iv_len;
    (void)aad;
    (void)aad_len;
    (void)plain;
    (void)plain_len;
    (void)cipher_out;
    (void)tag_out;
    (void)tag_len;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_aead_decrypt(alp_aead_backend_state_t *state,
                                    const uint8_t *iv, size_t iv_len,
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *cipher, size_t cipher_len,
                                    const uint8_t *tag, size_t tag_len,
                                    uint8_t *plain_out)
{
    (void)state;
    (void)iv;
    (void)iv_len;
    (void)aad;
    (void)aad_len;
    (void)cipher;
    (void)cipher_len;
    (void)tag;
    (void)tag_len;
    (void)plain_out;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static void sw_aead_close(alp_aead_backend_state_t *state)
{
    (void)state;
}

/* ---------- Random ---------- */

static alp_status_t sw_random_bytes(uint8_t *out, size_t len)
{
    /* Deterministic counter+0xAA fill -- NOT cryptographically random.
     * Tests that need bytes get a predictable pattern; production
     * builds that need real randomness MUST link the zephyr_drv
     * backend (priority 100) which routes through PSA Crypto's DRBG. */
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)((i + 1u) ^ 0xAAu);
    }
    return ALP_OK;
}

/* ---------- Registration ---------- */

static const alp_security_ops_t _ops = {
    .hash_open    = sw_hash_open,
    .hash_update  = sw_hash_update,
    .hash_finish  = sw_hash_finish,
    .hash_close   = sw_hash_close,
    .aead_open    = sw_aead_open,
    .aead_encrypt = sw_aead_encrypt,
    .aead_decrypt = sw_aead_decrypt,
    .aead_close   = sw_aead_close,
    .random_bytes = sw_random_bytes,
};

ALP_BACKEND_REGISTER(security, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
