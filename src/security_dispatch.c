/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * security class dispatcher.  Owns the public <alp/security.h>
 * surface -- two stateful handle types (alp_hash_t, alp_aead_t)
 * AND the stateless alp_random_bytes -- on top of the backend
 * registry mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers all three
 * primitives because a backend that implements one (e.g. PSA
 * Crypto) always implements the other two on the same SoC.  The
 * ops vtable in src/backends/security/security_ops.h carries
 * function pointers for every surface.
 *
 * Dispatch shape combines two patterns:
 *
 *   - hash + AEAD follow the mproc multi-handle pattern: each
 *     open() allocates from its own static pool, stores the ops
 *     pointer on the handle's state struct, and lets I/O ops
 *     dispatch through state.ops directly.  The capability
 *     getters return per-handle cached snapshots the registry
 *     produced at open() time.
 *
 *   - alp_random_bytes follows the TMU stateless-fast-path
 *     pattern: a single static const ops pointer caches the
 *     selected backend's vtable on first call; every subsequent
 *     alp_random_bytes routes through _cached_ops->random_bytes
 *     with no handle indirection.
 *
 * The same backend serves all three opens because they live in
 * one class registry -- alp_backend_select("security", ...) is
 * called for hash_open + aead_open and the cache populates as a
 * side-effect of the first call (whichever surface the customer
 * touches first).
 *
 * last_error stamping reuses the existing TLS slot via extern
 * forward decls so the dispatcher does not pull in the broader
 * handles.h header.  Probe() is not invoked for security -- the
 * v0.5 base_caps are sufficient for the three handle surfaces.
 *
 * Pool defaults: 2 hash, 2 aead (Kconfig-tunable via
 * CONFIG_ALP_SDK_MAX_{HASH,AEAD}_HANDLES).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/security.h>
#include <alp/soc_caps.h>

#include "backends/security/security_ops.h"

ALP_BACKEND_DEFINE_CLASS(security);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_HASH_HANDLES
#define CONFIG_ALP_SDK_MAX_HASH_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_AEAD_HANDLES
#define CONFIG_ALP_SDK_MAX_AEAD_HANDLES 2
#endif

static struct alp_hash  _hash_pool[CONFIG_ALP_SDK_MAX_HASH_HANDLES];
static struct alp_aead  _aead_pool[CONFIG_ALP_SDK_MAX_AEAD_HANDLES];

static struct alp_hash *_alloc_hash(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_HASH_HANDLES; ++i) {
        if (!_hash_pool[i].in_use) {
            memset(&_hash_pool[i], 0, sizeof(_hash_pool[i]));
            _hash_pool[i].in_use = true;
            return &_hash_pool[i];
        }
    }
    return NULL;
}

static void _free_hash(struct alp_hash *h) { h->in_use = false; }

static struct alp_aead *_alloc_aead(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_AEAD_HANDLES; ++i) {
        if (!_aead_pool[i].in_use) {
            memset(&_aead_pool[i], 0, sizeof(_aead_pool[i]));
            _aead_pool[i].in_use = true;
            return &_aead_pool[i];
        }
    }
    return NULL;
}

static void _free_aead(struct alp_aead *a) { a->in_use = false; }

/* ------------------------------------------------------------------ */
/* Cached ops vtable for the stateless random fast path.              */
/* ------------------------------------------------------------------ */

static const alp_security_ops_t *_cached_ops = NULL;

static const alp_security_ops_t *_get_ops(void)
{
    if (_cached_ops != NULL) {
        return _cached_ops;
    }
    const alp_backend_t *be = alp_backend_select("security", ALP_SOC_REF_STR);
    if (be == NULL) {
        return NULL;
    }
    _cached_ops = (const alp_security_ops_t *)be->ops;
    return _cached_ops;
}

/* ================================================================== */
/* Hash                                                                */
/* ================================================================== */

alp_hash_t *alp_hash_open(alp_hash_alg_t alg)
{
    alp_z_clear_last_error();
    /* Reject out-of-range algorithms before backend dispatch.  The
     * three supported enum values are SHA256/384/512 (0..2); the
     * cast via uint32_t dodges the -Wenum-compare-conditional
     * warning when callers pass a sentinel like 0xFFFFu. */
    if ((uint32_t)alg > (uint32_t)ALP_HASH_SHA512) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("security", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_security_ops_t *ops = (const alp_security_ops_t *)be->ops;
    if (ops == NULL || ops->hash_open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    /* Populate the random-fast-path cache opportunistically -- the
     * same backend wires up all three primitives. */
    if (_cached_ops == NULL) _cached_ops = ops;

    struct alp_hash *h = _alloc_hash();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend   = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    alp_status_t rc = ops->hash_open(alg, &h->state, &caps);
    if (rc != ALP_OK) {
        _free_hash(h);
        alp_z_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *data, size_t len)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    if (h->state.ops == NULL || h->state.ops->hash_update == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return h->state.ops->hash_update(&h->state, data, len);
}

alp_status_t alp_hash_finish(alp_hash_t *h, uint8_t *digest_out, size_t digest_cap,
                             size_t *digest_len)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (digest_out == NULL || digest_cap == 0) return ALP_ERR_INVAL;
    if (h->state.ops == NULL || h->state.ops->hash_finish == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    alp_status_t rc = h->state.ops->hash_finish(&h->state, digest_out, digest_cap,
                                                digest_len);
    /* Legacy contract: finish implicitly closes the handle on
     * success.  Match that here so callers don't leak a slot. */
    _free_hash(h);
    return rc;
}

void alp_hash_close(alp_hash_t *h)
{
    if (h == NULL || !h->in_use) return;
    if (h->state.ops != NULL && h->state.ops->hash_close != NULL) {
        h->state.ops->hash_close(&h->state);
    }
    _free_hash(h);
}

/* ================================================================== */
/* AEAD                                                                */
/* ================================================================== */

alp_aead_t *alp_aead_open(alp_aead_alg_t alg, const uint8_t *key, size_t key_len)
{
    alp_z_clear_last_error();
    if (key == NULL || key_len == 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("security", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_security_ops_t *ops = (const alp_security_ops_t *)be->ops;
    if (ops == NULL || ops->aead_open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    if (_cached_ops == NULL) _cached_ops = ops;

    struct alp_aead *a = _alloc_aead();
    if (a == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    a->backend   = be;
    a->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    alp_status_t rc = ops->aead_open(alg, key, key_len, &a->state, &caps);
    if (rc != ALP_OK) {
        _free_aead(a);
        alp_z_set_last_error(rc);
        return NULL;
    }
    a->cached_caps = caps;
    return a;
}

alp_status_t alp_aead_encrypt(alp_aead_t *a,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher_out,
                              uint8_t *tag_out, size_t tag_len)
{
    if (a == NULL || !a->in_use) return ALP_ERR_NOT_READY;
    if (iv == NULL || cipher_out == NULL || tag_out == NULL) return ALP_ERR_INVAL;
    if (a->state.ops == NULL || a->state.ops->aead_encrypt == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return a->state.ops->aead_encrypt(&a->state, iv, iv_len, aad, aad_len,
                                      plain, plain_len, cipher_out,
                                      tag_out, tag_len);
}

alp_status_t alp_aead_decrypt(alp_aead_t *a,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *cipher, size_t cipher_len,
                              const uint8_t *tag, size_t tag_len,
                              uint8_t *plain_out)
{
    if (a == NULL || !a->in_use) return ALP_ERR_NOT_READY;
    if (iv == NULL || cipher == NULL || tag == NULL || plain_out == NULL) {
        return ALP_ERR_INVAL;
    }
    if (a->state.ops == NULL || a->state.ops->aead_decrypt == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return a->state.ops->aead_decrypt(&a->state, iv, iv_len, aad, aad_len,
                                      cipher, cipher_len, tag, tag_len,
                                      plain_out);
}

void alp_aead_close(alp_aead_t *a)
{
    if (a == NULL || !a->in_use) return;
    if (a->state.ops != NULL && a->state.ops->aead_close != NULL) {
        a->state.ops->aead_close(&a->state);
    }
    _free_aead(a);
}

/* ================================================================== */
/* Random (stateless, no handle)                                       */
/* ================================================================== */

alp_status_t alp_random_bytes(uint8_t *out, size_t len)
{
    if (out == NULL || len == 0) return ALP_ERR_INVAL;
    const alp_security_ops_t *ops = _get_ops();
    if (ops == NULL || ops->random_bytes == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return ops->random_bytes(out, len);
}

/* ================================================================== */
/* Capability getters                                                  */
/* ================================================================== */

const alp_capabilities_t *alp_hash_capabilities(const alp_hash_t *h)
{
    return (h != NULL) ? &h->cached_caps : NULL;
}

const alp_capabilities_t *alp_aead_capabilities(const alp_aead_t *a)
{
    return (a != NULL) ? &a->cached_caps : NULL;
}
