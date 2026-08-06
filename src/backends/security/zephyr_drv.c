/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr backend for the <alp/security.h> surface.  Wraps
 * MbedTLS PSA Crypto and owns all three primitives -- hash, AEAD,
 * random_bytes -- behind one ops vtable.
 *
 * PSA picks the active SoC's hardware accelerator transparently:
 *
 *   - Alif Ensemble crypto subsystem (E3/E5/E7/E8) -- when MbedTLS's
 *     driver layer registers it, hashes / AEAD / RNG route to HW.
 *   - Renesas RZ/V2N RSIP -- same story via the V2N driver wrapper.
 *   - Everything else -- MbedTLS reference software implementations.
 *
 * The portable-HW-offload audit rule is satisfied because the
 * chip-specific dispatch happens inside MbedTLS -- application code
 * never sees a vendor name in <alp/security.h>.
 *
 * V2N TRNG entropy source.  The mbedtls profile (under
 * metadata/library-profiles/mbedtls/) sets MBEDTLS_NO_PLATFORM_ENTROPY,
 * so mbedtls's entropy module asks the SDK to supply a hardware-poll
 * callback.  On V2N (CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY=y), we
 * route that callback through the supervisor's GD32G553 TRNG so the
 * portable alp_random_bytes() transparently picks up true randomness
 * the first time PSA's DRBG seeds itself.  The wire-level chip name
 * stays hidden behind the supervisor singleton.
 *
 * Backend-owned state moved into module-static pools indexed via
 * state->be_data:
 *   - struct hash_be (psa_hash_operation_t)
 *   - struct aead_be (psa_key_id_t)
 *
 * The dispatcher (src/security_dispatch.c) owns the public-facing
 * struct alp_hash / struct alp_aead pools; this backend carries
 * only the PSA-specific per-handle blobs.
 *
 * Registers as silicon_ref="*" at priority 100 -- mirrors the
 * mproc / TMU / USB / BLE / Wi-Fi / MQTT / RPC siblings.  Gated on
 * CONFIG_ALP_SDK_SECURITY -- when OFF, every I/O op returns
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with PSA
 * Crypto in the device-tree configuration.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/security.h>

#include "alp_slot_claim.h"
#include "security_ops.h"

#if defined(CONFIG_ALP_SDK_SECURITY)
#include <psa/crypto.h>
#endif

#if defined(CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY)
#include <mbedtls/entropy.h>

#include "alp/chips/gd32g553.h"
#include "v2n_supervisor.h"
#endif

#ifndef CONFIG_ALP_SDK_MAX_HASH_HANDLES
#define CONFIG_ALP_SDK_MAX_HASH_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_AEAD_HANDLES
#define CONFIG_ALP_SDK_MAX_AEAD_HANDLES 2
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_SECURITY)

/* in_use is the LAST member (see alp_slot_claim.h): the winning claimant
 * zeroes everything before it via offsetof, so lifecycle-relevant fields
 * never carry a prior tenant's stale value into a freshly claimed slot.
 * Round-2 dev review: every op's `!be->in_use` guard below now reads it
 * with __atomic_load_n(__ATOMIC_ACQUIRE) too, not a plain load -- it is
 * always written via alp_slot_try_claim()/alp_slot_release(), and mixing
 * a plain read with an atomic RMW/store on the same object is a data
 * race in the C memory model even though no real hardware miscompiles a
 * bool this way. */
struct hash_be {
	psa_hash_operation_t op;
	bool                 in_use;
};

struct aead_be {
	psa_key_id_t key_id;
	bool         in_use;
};

static struct hash_be g_hash_be_pool[CONFIG_ALP_SDK_MAX_HASH_HANDLES];
static struct aead_be g_aead_be_pool[CONFIG_ALP_SDK_MAX_AEAD_HANDLES];
/* One-time PSA init, atomically claimed -- see ensure_psa() (issue #1115). */
static bool g_psa_init_claimed;
static bool g_psa_inited;

/* issue #1115: this used to be a plain `if (!in_use) { in_use = true; }`
 * scan -- called from ops->open() only AFTER the FRONTEND dispatcher slot
 * (src/security_dispatch.c) was already claimed atomically, so two
 * threads opening concurrently could both win the SAME backend slot here
 * and alias one psa_hash_operation_t / psa_key_id_t between two live
 * alp_hash_t/alp_aead_t handles -- crypto cross-talk. Route the claim
 * through the same atomic primitive the dispatcher pools already use. */
static struct hash_be *hash_be_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_hash_be_pool); ++i) {
		if (alp_slot_try_claim(&g_hash_be_pool[i].in_use)) {
			memset(&g_hash_be_pool[i], 0, offsetof(struct hash_be, in_use));
			return &g_hash_be_pool[i];
		}
	}
	return NULL;
}

static struct aead_be *aead_be_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_aead_be_pool); ++i) {
		if (alp_slot_try_claim(&g_aead_be_pool[i].in_use)) {
			memset(&g_aead_be_pool[i], 0, offsetof(struct aead_be, in_use));
			return &g_aead_be_pool[i];
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

/* Digest byte length per alg -- mirrors the SE/yocto backends' own copies
 * (se_cryptocell.c::alp_hash_digest_len, yocto_drv.c::hash_alg_md) so the
 * short-buffer pre-check below can report the required length without a
 * doomed psa_hash_finish() call first. */
static size_t alp_hash_digest_len(alp_hash_alg_t a)
{
	switch (a) {
	case ALP_HASH_SHA256:
		return 32u;
	case ALP_HASH_SHA384:
		return 48u;
	case ALP_HASH_SHA512:
		return 64u;
	default:
		return 0u;
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

/* issue #1115: this used to be a plain check-then-set (`if (g_psa_inited)
 * return; ...; g_psa_inited = true;`), so two threads racing the first
 * alp_hash_open()/alp_aead_open()/alp_random_bytes() call could both see
 * g_psa_inited == false and both call psa_crypto_init() concurrently.
 * mbedtls's own psa_crypto_init() only serialises its internal
 * global_data state under MBEDTLS_THREADING_C, which the alp-sdk mbedtls
 * profile does not enable -- so on this build two unsynchronised callers
 * really do race the same mutable global state.  Elect exactly one
 * initialiser with the same atomic claim the sidecar pools above use;
 * every other caller waits for it to publish g_psa_inited.
 *
 * issue #1114 round-2 dev review: the first liveness fix waited on
 * `while (!g_psa_inited) k_sleep(...)`, but the elected initialiser's
 * FAILURE path releases g_psa_init_claimed and returns its error
 * WITHOUT ever setting g_psa_inited -- so every thread that lost the
 * race hung in that loop forever, a worse hang than the pre-fix
 * behaviour (which simply returned the error to each caller). Loop the
 * whole elect-or-wait attempt instead of waiting on a flag the failure
 * path never sets: once the failed initialiser releases its claim, one
 * waiter re-wins alp_slot_try_claim() and retries psa_crypto_init()
 * itself (matching the existing "let a later caller retry" contract),
 * so every caller's wait terminates in either success or a real PSA
 * error -- never a permanent hang. */
static alp_status_t ensure_psa(void)
{
	for (;;) {
		if (__atomic_load_n(&g_psa_inited, __ATOMIC_ACQUIRE)) {
			return ALP_OK;
		}
		if (alp_slot_try_claim(&g_psa_init_claimed)) {
			psa_status_t st = psa_crypto_init();
			if (st != PSA_SUCCESS) {
				/* Let a later caller (this thread or a waiter that
				 * re-wins the claim below) retry the one-time init. */
				alp_slot_release(&g_psa_init_claimed);
				return psa_to_alp(st);
			}
			__atomic_store_n(&g_psa_inited, true, __ATOMIC_RELEASE);
			return ALP_OK;
		}
		/* Lost the race: another thread is running psa_crypto_init() (or
		 * just failed it and released the claim) right now. Wait for it
		 * with a real sleep, not a spin (issue #1114) -- this backend
		 * runs under Zephyr's preemptive-priority scheduler, so a
		 * spinning loser at equal-or-higher priority than the
		 * initialiser could never let the scheduler run the initialiser
		 * back. Loop back to re-check g_psa_inited AND re-attempt the
		 * claim (not just re-check the flag) so a failed initialiser's
		 * released claim gets picked up. */
		k_sleep(K_TICKS(1));
	}
}

static alp_status_t aead_alg_meta(alp_aead_alg_t   a,
                                  psa_algorithm_t *out_alg,
                                  psa_key_type_t  *out_kt,
                                  size_t          *out_key_bits)
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

/* ================================================================== */
/* Hash ops                                                            */
/* ================================================================== */

static alp_status_t
z_hash_open(alp_hash_alg_t alg, alp_hash_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)caps_out;
#if defined(CONFIG_ALP_SDK_SECURITY)
	if (ensure_psa() != ALP_OK) return ALP_ERR_IO;
	psa_algorithm_t psa_alg = to_psa_hash(alg);
	if (psa_alg == 0) return ALP_ERR_INVAL;

	struct hash_be *be = hash_be_acquire();
	if (be == NULL) return ALP_ERR_NOMEM;

	be->op          = psa_hash_operation_init();
	psa_status_t st = psa_hash_setup(&be->op, psa_alg);
	if (st != PSA_SUCCESS) {
		alp_slot_release(&be->in_use);
		return psa_to_alp(st);
	}
	state->alg     = alg;
	state->be_data = be;
	return ALP_OK;
#else
	(void)alg;
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_hash_update(alp_hash_backend_state_t *state, const uint8_t *data, size_t len)
{
#if defined(CONFIG_ALP_SDK_SECURITY)
	struct hash_be *be = (struct hash_be *)state->be_data;
	if (be == NULL || !__atomic_load_n(&be->in_use, __ATOMIC_ACQUIRE)) return ALP_ERR_NOT_READY;
	return psa_to_alp(psa_hash_update(&be->op, data, len));
#else
	(void)state;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_hash_finish(alp_hash_backend_state_t *state,
                                  uint8_t                  *digest_out,
                                  size_t                    digest_cap,
                                  size_t                   *digest_len)
{
#if defined(CONFIG_ALP_SDK_SECURITY)
	struct hash_be *be = (struct hash_be *)state->be_data;
	if (be == NULL || !__atomic_load_n(&be->in_use, __ATOMIC_ACQUIRE)) return ALP_ERR_NOT_READY;

	const size_t required = alp_hash_digest_len(state->alg);
	if (digest_out == NULL || digest_cap < required) {
		/* GHSA-92c3-v48m-m5gg: report the required length but do NOT
		 * touch `be` / `state->be_data` here -- calling psa_hash_finish()
		 * on a too-small buffer would both destroy the PSA operation
		 * object AND return PSA_ERROR_BUFFER_TOO_SMALL (which psa_to_alp
		 * maps to ALP_ERR_IO, not the documented ALP_ERR_INVAL).
		 * <alp/security.h> only lets ALP_OK implicitly close the handle,
		 * so leave be->op intact and the slot claimed for either a
		 * correctly sized retry or an explicit alp_hash_close(). */
		if (digest_len != NULL) *digest_len = required;
		return ALP_ERR_INVAL;
	}

	size_t       got = 0;
	psa_status_t st  = psa_hash_finish(&be->op, digest_out, digest_cap, &got);
	if (digest_len != NULL) *digest_len = got;
	state->be_data = NULL;
	alp_slot_release(&be->in_use);
	return psa_to_alp(st);
#else
	(void)state;
	(void)digest_out;
	(void)digest_cap;
	if (digest_len != NULL) *digest_len = 0;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_hash_close(alp_hash_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_SECURITY)
	struct hash_be *be = (struct hash_be *)state->be_data;
	if (be == NULL || !__atomic_load_n(&be->in_use, __ATOMIC_ACQUIRE)) return;
	(void)psa_hash_abort(&be->op);
	state->be_data = NULL;
	alp_slot_release(&be->in_use);
#else
	(void)state;
#endif
}

/* ================================================================== */
/* AEAD ops                                                            */
/* ================================================================== */

static alp_status_t z_aead_open(alp_aead_alg_t            alg,
                                const uint8_t            *key,
                                size_t                    key_len,
                                alp_aead_backend_state_t *state,
                                alp_capabilities_t       *caps_out)
{
	(void)caps_out;
#if defined(CONFIG_ALP_SDK_SECURITY)
	if (ensure_psa() != ALP_OK) return ALP_ERR_IO;

	psa_algorithm_t psa_alg;
	psa_key_type_t  kt;
	size_t          key_bits;
	if (aead_alg_meta(alg, &psa_alg, &kt, &key_bits) != ALP_OK) {
		return ALP_ERR_INVAL;
	}
	if (key_len * 8 != key_bits) return ALP_ERR_INVAL;

	struct aead_be *be = aead_be_acquire();
	if (be == NULL) return ALP_ERR_NOMEM;

	psa_key_attributes_t attr = psa_key_attributes_init();
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, psa_alg);
	psa_set_key_type(&attr, kt);
	psa_set_key_bits(&attr, (size_t)key_bits);

	psa_status_t st = psa_import_key(&attr, key, key_len, &be->key_id);
	psa_reset_key_attributes(&attr);
	if (st != PSA_SUCCESS) {
		alp_slot_release(&be->in_use);
		return psa_to_alp(st);
	}
	state->alg     = alg;
	state->be_data = be;
	return ALP_OK;
#else
	(void)alg;
	(void)key;
	(void)key_len;
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_aead_encrypt(alp_aead_backend_state_t *state,
                                   const uint8_t            *iv,
                                   size_t                    iv_len,
                                   const uint8_t            *aad,
                                   size_t                    aad_len,
                                   const uint8_t            *plain,
                                   size_t                    plain_len,
                                   uint8_t                  *cipher_out,
                                   uint8_t                  *tag_out,
                                   size_t                    tag_len)
{
#if defined(CONFIG_ALP_SDK_SECURITY)
	struct aead_be *be = (struct aead_be *)state->be_data;
	if (be == NULL || !__atomic_load_n(&be->in_use, __ATOMIC_ACQUIRE)) return ALP_ERR_NOT_READY;

	/* tag_len must be exactly 16 B -- the only length every backend
     * (this one, yocto_drv.c, se_cryptocell.c) round-trips.  Check
     * before any PSA setup so a bad caller never burns a live
     * psa_aead_operation_t. */
	if (tag_len != 16u) return ALP_ERR_INVAL;

	psa_algorithm_t psa_alg;
	psa_key_type_t  kt;
	size_t          kb;
	/* state->alg was already validated by aead_alg_meta() in z_aead_open();
	 * check the return again rather than discard it, so psa_alg is never
	 * used unset if that invariant is ever broken. */
	if (aead_alg_meta(state->alg, &psa_alg, &kt, &kb) != ALP_OK) return ALP_ERR_INVAL;

	/* GHSA-7xh2-9pcg-r824: this used to buffer the whole ciphertext||tag
     * in a 4,112-byte automatic array, which overflows a 4 KiB caller
     * thread (a common CONFIG_MAIN_STACK_SIZE).  PSA's multipart AEAD
     * streams ciphertext straight into the caller's cipher_out buffer
     * instead.  The only stack state is the fixed-size `op` context
     * (independent of plain_len) -- no scratch buffer at all.
     *
     * PSA multipart conformance (adversarial-review defect #1): a
     * driver backing this PSA implementation (e.g. Alif SE / Renesas
     * RSIP, once routed through the mbedtls driver layer) is allowed
     * to hold back a partial block in psa_aead_update() and emit it
     * from psa_aead_finish() instead -- software mbedtls happens to
     * always emit the whole input in update(), but that is NOT a PSA
     * guarantee.  So: track a running offset into cipher_out, hand
     * psa_aead_update() the REMAINING capacity (== plain_len - off,
     * which for this single one-shot call is plain_len -- the PSA-
     * required per-update ceiling PSA_AEAD_UPDATE_OUTPUT_SIZE() can
     * never exceed the input length on a first/only update call, since
     * there is no prior held-back data to combine with), then let
     * psa_aead_finish() write its tail directly after the update's
     * output.  Total ciphertext length equals plaintext length for
     * these AEADs, so `off == plain_len` on success replaces the old
     * per-call `produced == len` / `final_len == 0` equality that a
     * holdback driver would spuriously fail. */
	psa_aead_operation_t op = psa_aead_operation_init();
	psa_status_t         st = psa_aead_encrypt_setup(&op, be->key_id, psa_alg);
	if (st != PSA_SUCCESS) {
		(void)psa_aead_abort(&op);
		return psa_to_alp(st);
	}

	st = psa_aead_set_nonce(&op, iv, iv_len);
	if (st != PSA_SUCCESS) {
		(void)psa_aead_abort(&op);
		return psa_to_alp(st);
	}

	if (aad_len > 0) {
		st = psa_aead_update_ad(&op, aad, aad_len);
		if (st != PSA_SUCCESS) {
			(void)psa_aead_abort(&op);
			return psa_to_alp(st);
		}
	}

	size_t off = 0;

	if (plain_len > 0) {
		size_t produced = 0;
		st = psa_aead_update(&op, plain, plain_len, cipher_out + off, plain_len - off, &produced);
		if (st != PSA_SUCCESS) {
			(void)psa_aead_abort(&op);
			return psa_to_alp(st);
		}
		off += produced;
	}

	size_t final_len        = 0;
	size_t produced_tag_len = 0;
	st                      = psa_aead_finish(
	    &op, cipher_out + off, plain_len - off, &final_len, tag_out, tag_len, &produced_tag_len);
	if (st != PSA_SUCCESS) {
		(void)psa_aead_abort(&op);
		return psa_to_alp(st);
	}
	off += final_len;
	if (off != plain_len || produced_tag_len != tag_len) return ALP_ERR_IO;
	return ALP_OK;
#else
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
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_aead_decrypt(alp_aead_backend_state_t *state,
                                   const uint8_t            *iv,
                                   size_t                    iv_len,
                                   const uint8_t            *aad,
                                   size_t                    aad_len,
                                   const uint8_t            *cipher,
                                   size_t                    cipher_len,
                                   const uint8_t            *tag,
                                   size_t                    tag_len,
                                   uint8_t                  *plain_out)
{
#if defined(CONFIG_ALP_SDK_SECURITY)
	struct aead_be *be = (struct aead_be *)state->be_data;
	if (be == NULL || !__atomic_load_n(&be->in_use, __ATOMIC_ACQUIRE)) return ALP_ERR_NOT_READY;

	/* tag_len must be exactly 16 B -- see z_aead_encrypt. */
	if (tag_len != 16u) return ALP_ERR_INVAL;

	psa_algorithm_t psa_alg;
	psa_key_type_t  kt;
	size_t          kb;
	/* See z_aead_encrypt: check the return rather than discard it. */
	if (aead_alg_meta(state->alg, &psa_alg, &kt, &kb) != ALP_OK) return ALP_ERR_INVAL;

	/* GHSA-7xh2-9pcg-r824: see z_aead_encrypt -- multipart AEAD streams
     * ciphertext straight from the caller's `cipher` into `plain_out`,
     * no combined ciphertext||tag scratch and no payload-size ceiling.
     *
     * PSA multipart conformance (adversarial-review defect #1): same
     * running-offset / remaining-capacity handling as z_aead_encrypt,
     * so a holdback-capable driver's psa_aead_verify() tail is not
     * mistaken for a spurious mismatch.  `off == cipher_len` on
     * success replaces the old `produced == len` / `verify_len == 0`
     * equality.
     *
     * Unverified-plaintext window: psa_aead_verify() writes its tail
     * bytes directly into plain_out + off *before* it has confirmed
     * the tag -- see <alp/security.h> alp_aead_decrypt() docs.  Every
     * failure path below (update failure, verify failure, and the
     * short-total defensive check) memsets the FULL cipher_len region
     * of plain_out, not just the bytes psa_aead_update() wrote, so
     * that transient tail is never left readable on a rejected
     * message. */
	psa_aead_operation_t op = psa_aead_operation_init();
	psa_status_t         st = psa_aead_decrypt_setup(&op, be->key_id, psa_alg);
	if (st != PSA_SUCCESS) {
		(void)psa_aead_abort(&op);
		return psa_to_alp(st);
	}

	st = psa_aead_set_nonce(&op, iv, iv_len);
	if (st != PSA_SUCCESS) {
		(void)psa_aead_abort(&op);
		return psa_to_alp(st);
	}

	if (aad_len > 0) {
		st = psa_aead_update_ad(&op, aad, aad_len);
		if (st != PSA_SUCCESS) {
			(void)psa_aead_abort(&op);
			return psa_to_alp(st);
		}
	}

	size_t off = 0;

	if (cipher_len > 0) {
		size_t produced = 0;
		st = psa_aead_update(&op, cipher, cipher_len, plain_out + off, cipher_len - off, &produced);
		if (st != PSA_SUCCESS) {
			(void)psa_aead_abort(&op);
			memset(plain_out, 0, cipher_len);
			return psa_to_alp(st);
		}
		off += produced;
	}

	size_t verify_len = 0;
	st = psa_aead_verify(&op, plain_out + off, cipher_len - off, &verify_len, tag, tag_len);
	if (st != PSA_SUCCESS) {
		/* Tampered tag (PSA_ERROR_INVALID_SIGNATURE) or any other
         * failure: wipe the full plain_out region -- update() already
         * wrote up to `off` bytes and verify() may have just written
         * its (unverified) tail past it -- so a tag mismatch never
         * leaves plaintext sitting in plain_out. */
		(void)psa_aead_abort(&op);
		memset(plain_out, 0, cipher_len);
		return psa_to_alp(st);
	}
	off += verify_len;
	if (off != cipher_len) {
		memset(plain_out, 0, cipher_len);
		return ALP_ERR_IO;
	}
	return ALP_OK;
#else
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
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_aead_close(alp_aead_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_SECURITY)
	struct aead_be *be = (struct aead_be *)state->be_data;
	if (be == NULL || !__atomic_load_n(&be->in_use, __ATOMIC_ACQUIRE)) return;
	(void)psa_destroy_key(be->key_id);
	state->be_data = NULL;
	alp_slot_release(&be->in_use);
#else
	(void)state;
#endif
}

/* ================================================================== */
/* Random (stateless)                                                  */
/* ================================================================== */

static alp_status_t z_random_bytes(uint8_t *out, size_t len)
{
#if defined(CONFIG_ALP_SDK_SECURITY)
	if (ensure_psa() != ALP_OK) return ALP_ERR_IO;
	return psa_to_alp(psa_generate_random(out, len));
#else
	(void)out;
	(void)len;
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
/* from this source on first use (and reseeds periodically), so the   */
/* portable alp_random_bytes() benefits transparently without app     */
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

/* ---------- Registration ---------- */

static const alp_security_ops_t _ops = {
	.hash_open    = z_hash_open,
	.hash_update  = z_hash_update,
	.hash_finish  = z_hash_finish,
	.hash_close   = z_hash_close,
	.aead_open    = z_aead_open,
	.aead_encrypt = z_aead_encrypt,
	.aead_decrypt = z_aead_decrypt,
	.aead_close   = z_aead_close,
	.random_bytes = z_random_bytes,
};

ALP_BACKEND_REGISTER(security,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
