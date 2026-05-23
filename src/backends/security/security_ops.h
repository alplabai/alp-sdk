/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_security dispatcher and per-backend
 * implementations.  The security class is unusual: it carries THREE
 * primitives behind a single header (<alp/security.h>):
 *
 *   - alp_hash_t      -- stateful hash context (sha256 / 384 / 512)
 *   - alp_aead_t      -- stateful AEAD context (AES-128/256-GCM,
 *                                                ChaCha20-Poly1305)
 *   - alp_random_bytes  -- stateless random fill (no handle)
 *
 * Per the design spec Section 4: ONE class registry covers all three
 * primitives because a backend that implements one (e.g. PSA Crypto)
 * always implements the other two on the same SoC.  The ops vtable
 * carries function pointers for every surface; the dispatcher owns
 * two separate handle pools (hash + AEAD) and a cached ops pointer
 * for the stateless random fast path.
 *
 * Mirrors the multi-handle shape of mproc_ops.h (shmem + mbox + hwsem)
 * but with hash/AEAD carrying their own alg field on the state struct
 * so backends can specialise without an extra parameter on every op.
 *
 * NOT a public header.
 */

#ifndef ALP_BACKENDS_SECURITY_OPS_H
#define ALP_BACKENDS_SECURITY_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/security.h>

typedef struct alp_security_ops alp_security_ops_t;

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

typedef struct alp_hash_backend_state {
    alp_hash_alg_t            alg;
    void                     *be_data;
    const alp_security_ops_t *ops;
} alp_hash_backend_state_t;

typedef struct alp_aead_backend_state {
    alp_aead_alg_t            alg;
    void                     *be_data;
    const alp_security_ops_t *ops;
} alp_aead_backend_state_t;

/* ------------------------------------------------------------------ */
/* Combined ops vtable -- one entry per primitive op                   */
/* ------------------------------------------------------------------ */

struct alp_security_ops {
    /* ---- Hash ---- */
    alp_status_t (*hash_open)(alp_hash_alg_t alg,
                              alp_hash_backend_state_t *state,
                              alp_capabilities_t *caps_out);
    alp_status_t (*hash_update)(alp_hash_backend_state_t *state,
                                const uint8_t *data, size_t len);
    alp_status_t (*hash_finish)(alp_hash_backend_state_t *state,
                                uint8_t *digest_out, size_t digest_cap,
                                size_t *digest_len);
    void         (*hash_close)(alp_hash_backend_state_t *state);

    /* ---- AEAD ---- */
    alp_status_t (*aead_open)(alp_aead_alg_t alg,
                              const uint8_t *key, size_t key_len,
                              alp_aead_backend_state_t *state,
                              alp_capabilities_t *caps_out);
    alp_status_t (*aead_encrypt)(alp_aead_backend_state_t *state,
                                 const uint8_t *iv, size_t iv_len,
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *plain, size_t plain_len,
                                 uint8_t *cipher_out,
                                 uint8_t *tag_out, size_t tag_len);
    alp_status_t (*aead_decrypt)(alp_aead_backend_state_t *state,
                                 const uint8_t *iv, size_t iv_len,
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *cipher, size_t cipher_len,
                                 const uint8_t *tag, size_t tag_len,
                                 uint8_t *plain_out);
    void         (*aead_close)(alp_aead_backend_state_t *state);

    /* ---- Random (stateless, no handle) ---- */
    alp_status_t (*random_bytes)(uint8_t *out, size_t len);
};

/* ------------------------------------------------------------------ */
/* Public handle layouts -- owned by the dispatcher pools              */
/* ------------------------------------------------------------------ */

struct alp_hash {
    alp_hash_backend_state_t   state;
    const alp_backend_t       *backend;
    alp_capabilities_t         cached_caps;
    bool                       in_use;
};

struct alp_aead {
    alp_aead_backend_state_t   state;
    const alp_backend_t       *backend;
    alp_capabilities_t         cached_caps;
    bool                       in_use;
};

#endif /* ALP_BACKENDS_SECURITY_OPS_H */
