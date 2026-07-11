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

#include "alp_dispatch_cache.h"
#include "alp_slot_claim.h"
#include "backends/security/security_ops.h"

ALP_BACKEND_DEFINE_CLASS(security);
/* Pull the security registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(security);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_HASH_HANDLES
#define CONFIG_ALP_SDK_MAX_HASH_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_AEAD_HANDLES
#define CONFIG_ALP_SDK_MAX_AEAD_HANDLES 2
#endif

static struct alp_hash _hash_pool[CONFIG_ALP_SDK_MAX_HASH_HANDLES];
static struct alp_aead _aead_pool[CONFIG_ALP_SDK_MAX_AEAD_HANDLES];

static struct alp_hash *_alloc_hash(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_HASH_HANDLES; ++i) {
		/* Atomic claim (issue #629): only the winner of the flag flip
		 * may touch the slot's other fields -- in_use is the
		 * struct's last member, so zero everything before it,
		 * including lifecycle/active_ops. */
		if (alp_slot_try_claim(&_hash_pool[i].in_use)) {
			memset(&_hash_pool[i], 0, offsetof(struct alp_hash, in_use));
			return &_hash_pool[i];
		}
	}
	return NULL;
}

static void _free_hash(struct alp_hash *h)
{
	alp_slot_release(&h->in_use);
}

static struct alp_aead *_alloc_aead(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_AEAD_HANDLES; ++i) {
		if (alp_slot_try_claim(&_aead_pool[i].in_use)) {
			memset(&_aead_pool[i], 0, offsetof(struct alp_aead, in_use));
			return &_aead_pool[i];
		}
	}
	return NULL;
}

static void _free_aead(struct alp_aead *a)
{
	alp_slot_release(&a->in_use);
}

/* ------------------------------------------------------------------ */
/* Cached ops vtable for the stateless random fast path.              */
/*                                                                     */
/* Published through alp_dispatch_cache_{load,store}() (acquire load /
 * release store) -- both the normal path below AND the opportunistic
 * populate in alp_hash_open/alp_aead_open route through the same two
 * helpers, so every writer of _cached_ops uses the identical
 * synchronization primitive (issue #628: mixing a locked writer with
 * a plain one is still a data race even if each writer alone looks
 * safe). */
/* ------------------------------------------------------------------ */

static const alp_security_ops_t *_cached_ops = NULL;

/**
 * @brief Publish @p ops to the random-fast-path cache if nobody has yet.
 *
 * Shared by _get_ops() and the opportunistic populate in
 * alp_hash_open/alp_aead_open so every write goes through the same
 * acquire-load/release-store pair (issue #628).
 */
static void _publish_ops_if_absent(const alp_security_ops_t *ops)
{
	if (ops == NULL) {
		return;
	}
	if (alp_dispatch_cache_load((const void *const *)&_cached_ops) == NULL) {
		alp_dispatch_cache_store((const void **)&_cached_ops, (const void *)ops);
	}
}

static const alp_security_ops_t *_get_ops(void)
{
	const alp_security_ops_t *ops =
	    (const alp_security_ops_t *)alp_dispatch_cache_load((const void *const *)&_cached_ops);
	if (ops != NULL) {
		return ops;
	}
	const alp_backend_t *be = alp_backend_select("security", ALP_SOC_REF_STR);
	if (be == NULL) {
		return NULL;
	}
	ops = (const alp_security_ops_t *)be->ops;
	alp_dispatch_cache_store((const void **)&_cached_ops, (const void *)ops);
	return ops;
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
	/* Populate the random-fast-path cache opportunistically from the
     * top-ranked backend -- the same backend wires up all three
     * primitives, and its random path stays live even when it later
     * declines this particular hash alg.  Routed through the same
     * publish helper _get_ops() uses (issue #628). */
	_publish_ops_if_absent((const alp_security_ops_t *)be->ops);

	struct alp_hash *h = _alloc_hash();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}

	/* Fall-through selection (issue #239): a backend may decline an alg
     * it does not implement (e.g. the SE CryptoCell declines SHA-384/512)
     * by returning ALP_ERR_NOSUPPORT from hash_open.  Open time is the
     * only safe point to re-select -- no data has been consumed yet -- so
     * walk down the ranked candidates until one accepts.  Any error other
     * than NOSUPPORT is a real failure and is surfaced, not masked by a
     * lower-ranked backend. */
	alp_status_t rc = ALP_ERR_NOT_IMPLEMENTED;
	while (be != NULL) {
		const alp_security_ops_t *ops = (const alp_security_ops_t *)be->ops;
		if (ops != NULL && ops->hash_open != NULL) {
			memset(&h->state, 0, sizeof(h->state));
			h->backend              = be;
			h->state.ops            = ops;
			alp_capabilities_t caps = { .flags = be->base_caps };
			rc                      = ops->hash_open(alg, &h->state, &caps);
			if (rc == ALP_OK) {
				h->cached_caps = caps;
				alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
				return h;
			}
			if (rc != ALP_ERR_NOSUPPORT) {
				break;
			}
		}
		be = alp_backend_select_next("security", ALP_SOC_REF_STR, be);
	}
	_free_hash(h);
	alp_z_set_last_error(rc);
	return NULL;
}

alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *data, size_t len)
{
	if (h == NULL) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	/* Gate on the lifecycle byte, not in_use -- in_use is now touched
	 * only by the atomic claim/release in _alloc_hash/_free_hash, so
	 * every reader of the handle's "is it live" state goes through the
	 * same atomic (issue #629: mixing atomic in_use with a plain read
	 * elsewhere is still a data race). */
	if (!alp_handle_op_enter(&h->lifecycle, &h->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (h->state.ops == NULL || h->state.ops->hash_update == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->hash_update(&h->state, data, len);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t
alp_hash_finish(alp_hash_t *h, uint8_t *digest_out, size_t digest_cap, size_t *digest_len)
{
	if (h == NULL) return ALP_ERR_NOT_READY;
	if (digest_out == NULL || digest_cap == 0) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&h->lifecycle, &h->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (h->state.ops == NULL || h->state.ops->hash_finish == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = h->state.ops->hash_finish(&h->state, digest_out, digest_cap, digest_len);
	}
	alp_handle_op_leave(&h->active_ops);
	/* <alp/security.h> contract (GHSA-92c3-v48m-m5gg): only ALP_OK
     * implicitly closes the handle (releasing only the slot, not the
     * backend -- finish() itself already tore down whatever
     * hash_close would).  Every other result -- including
     * ALP_ERR_INVAL from a too-small digest buffer -- leaves the
     * handle open: backends report the required digest length on
     * that path and keep their own state valid for a correctly sized
     * retry or an explicit alp_hash_close().  Match that here so a
     * successful finish doesn't leak a slot -- routed through the
     * same begin_close() guard as alp_hash_close() so a racing
     * explicit close() and this implicit one can't both release the
     * slot (issue #629). */
	if (rc == ALP_OK && alp_handle_begin_close(&h->lifecycle, &h->active_ops)) {
		alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
		alp_slot_release(&h->in_use);
	}
	return rc;
}

void alp_hash_close(alp_hash_t *h)
{
	if (h == NULL) return;
	/* alp_handle_begin_close() gates out any new op and drains any
	 * in-flight one before we touch state.ops -- and CASes out a
	 * concurrent alp_hash_finish()'s implicit close (issue #629). */
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) return;
	if (h->state.ops != NULL && h->state.ops->hash_close != NULL) {
		h->state.ops->hash_close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_slot_release(&h->in_use);
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
	/* Same opportunistic populate as alp_hash_open above (issue #628). */
	_publish_ops_if_absent((const alp_security_ops_t *)be->ops);

	struct alp_aead *a = _alloc_aead();
	if (a == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}

	/* Fall-through selection on NOSUPPORT at open -- same shape as
     * alp_hash_open above (issue #239). */
	alp_status_t rc = ALP_ERR_NOT_IMPLEMENTED;
	while (be != NULL) {
		const alp_security_ops_t *ops = (const alp_security_ops_t *)be->ops;
		if (ops != NULL && ops->aead_open != NULL) {
			memset(&a->state, 0, sizeof(a->state));
			a->backend              = be;
			a->state.ops            = ops;
			alp_capabilities_t caps = { .flags = be->base_caps };
			rc                      = ops->aead_open(alg, key, key_len, &a->state, &caps);
			if (rc == ALP_OK) {
				a->cached_caps = caps;
				alp_lifecycle_set(&a->lifecycle, ALP_HANDLE_LC_OPEN);
				return a;
			}
			if (rc != ALP_ERR_NOSUPPORT) {
				break;
			}
		}
		be = alp_backend_select_next("security", ALP_SOC_REF_STR, be);
	}
	_free_aead(a);
	alp_z_set_last_error(rc);
	return NULL;
}

alp_status_t alp_aead_encrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         iv_len,
                              const uint8_t *aad,
                              size_t         aad_len,
                              const uint8_t *plain,
                              size_t         plain_len,
                              uint8_t       *cipher_out,
                              uint8_t       *tag_out,
                              size_t         tag_len)
{
	if (a == NULL) return ALP_ERR_NOT_READY;
	if (iv == NULL || cipher_out == NULL || tag_out == NULL) return ALP_ERR_INVAL;
	/* aad == NULL is legitimate only for the no-AAD case (aad_len == 0,
     * see <alp/security.h>); reject the contradictory combination here
     * so no backend ever dereferences or translates a NULL aad
     * (issue #245). */
	if (aad == NULL && aad_len > 0) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&a->lifecycle, &a->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (a->state.ops == NULL || a->state.ops->aead_encrypt == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = a->state.ops->aead_encrypt(
		    &a->state, iv, iv_len, aad, aad_len, plain, plain_len, cipher_out, tag_out, tag_len);
	}
	alp_handle_op_leave(&a->active_ops);
	return rc;
}

alp_status_t alp_aead_decrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         iv_len,
                              const uint8_t *aad,
                              size_t         aad_len,
                              const uint8_t *cipher,
                              size_t         cipher_len,
                              const uint8_t *tag,
                              size_t         tag_len,
                              uint8_t       *plain_out)
{
	if (a == NULL) return ALP_ERR_NOT_READY;
	if (iv == NULL || cipher == NULL || tag == NULL || plain_out == NULL) {
		return ALP_ERR_INVAL;
	}
	/* Same no-AAD contract as alp_aead_encrypt (issue #245). */
	if (aad == NULL && aad_len > 0) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&a->lifecycle, &a->active_ops)) return ALP_ERR_NOT_READY;
	alp_status_t rc;
	if (a->state.ops == NULL || a->state.ops->aead_decrypt == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = a->state.ops->aead_decrypt(
		    &a->state, iv, iv_len, aad, aad_len, cipher, cipher_len, tag, tag_len, plain_out);
	}
	alp_handle_op_leave(&a->active_ops);
	return rc;
}

void alp_aead_close(alp_aead_t *a)
{
	if (a == NULL) return;
	if (!alp_handle_begin_close(&a->lifecycle, &a->active_ops)) return;
	if (a->state.ops != NULL && a->state.ops->aead_close != NULL) {
		a->state.ops->aead_close(&a->state);
	}
	alp_lifecycle_set(&a->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_slot_release(&a->in_use);
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
