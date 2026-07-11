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
 * The portable-HW-offload audit rule (memory/feedback_portable_hw_
 * offload_with_sw_fallback.md) is satisfied because the chip-specific
 * dispatch happens inside MbedTLS -- application code never sees a
 * vendor name in <alp/security.h>.
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
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/security.h>

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

struct hash_be {
	bool                 in_use;
	psa_hash_operation_t op;
};

struct aead_be {
	bool         in_use;
	psa_key_id_t key_id;
};

static struct hash_be g_hash_be_pool[CONFIG_ALP_SDK_MAX_HASH_HANDLES];
static struct aead_be g_aead_be_pool[CONFIG_ALP_SDK_MAX_AEAD_HANDLES];
static bool           g_psa_inited;

static struct hash_be *hash_be_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_hash_be_pool); ++i) {
		if (!g_hash_be_pool[i].in_use) {
			memset(&g_hash_be_pool[i], 0, sizeof(g_hash_be_pool[i]));
			g_hash_be_pool[i].in_use = true;
			return &g_hash_be_pool[i];
		}
	}
	return NULL;
}

static struct aead_be *aead_be_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_aead_be_pool); ++i) {
		if (!g_aead_be_pool[i].in_use) {
			memset(&g_aead_be_pool[i], 0, sizeof(g_aead_be_pool[i]));
			g_aead_be_pool[i].in_use = true;
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

static alp_status_t ensure_psa(void)
{
	if (g_psa_inited) return ALP_OK;
	psa_status_t st = psa_crypto_init();
	if (st != PSA_SUCCESS) return psa_to_alp(st);
	g_psa_inited = true;
	return ALP_OK;
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
		be->in_use = false;
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
	if (be == NULL || !be->in_use) return ALP_ERR_NOT_READY;
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
	if (be == NULL || !be->in_use) return ALP_ERR_NOT_READY;

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
	be->in_use     = false;
	state->be_data = NULL;
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
	if (be == NULL || !be->in_use) return;
	(void)psa_hash_abort(&be->op);
	be->in_use     = false;
	state->be_data = NULL;
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
		be->in_use = false;
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
	if (be == NULL || !be->in_use) return ALP_ERR_NOT_READY;

	psa_algorithm_t psa_alg;
	psa_key_type_t  kt;
	size_t          kb;
	(void)aead_alg_meta(state->alg, &psa_alg, &kt, &kb);

	/* PSA's aead_encrypt produces ciphertext || tag in one buffer.  We
     * copy the tag tail out into the caller's tag buffer to match the
     * public API's tag-out separation.  Stack scratch bounded at
     * 4096 + 16 -- larger blobs land in a heap-fallback follow-on. */
	if (plain_len > 4096) return ALP_ERR_NOSUPPORT;
	uint8_t      scratch[4096 + 16];
	size_t       produced = 0;
	psa_status_t st       = psa_aead_encrypt(be->key_id,
	                                         psa_alg,
	                                         iv,
	                                         iv_len,
	                                         aad,
	                                         aad_len,
	                                         plain,
	                                         plain_len,
	                                         scratch,
	                                         sizeof(scratch),
	                                         &produced);
	if (st != PSA_SUCCESS) return psa_to_alp(st);

	if (produced < tag_len || produced - tag_len != plain_len) return ALP_ERR_IO;
	memcpy(cipher_out, scratch, plain_len);
	memcpy(tag_out, scratch + plain_len, tag_len);
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
	if (be == NULL || !be->in_use) return ALP_ERR_NOT_READY;

	psa_algorithm_t psa_alg;
	psa_key_type_t  kt;
	size_t          kb;
	(void)aead_alg_meta(state->alg, &psa_alg, &kt, &kb);

	if (cipher_len > 4096) return ALP_ERR_NOSUPPORT;
	uint8_t scratch[4096 + 16];
	if (cipher_len + tag_len > sizeof(scratch)) return ALP_ERR_NOSUPPORT;
	memcpy(scratch, cipher, cipher_len);
	memcpy(scratch + cipher_len, tag, tag_len);

	size_t       produced = 0;
	psa_status_t st       = psa_aead_decrypt(be->key_id,
	                                         psa_alg,
	                                         iv,
	                                         iv_len,
	                                         aad,
	                                         aad_len,
	                                         scratch,
	                                         cipher_len + tag_len,
	                                         plain_out,
	                                         cipher_len,
	                                         &produced);
	return psa_to_alp(st);
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
	if (be == NULL || !be->in_use) return;
	(void)psa_destroy_key(be->key_id);
	be->in_use     = false;
	state->be_data = NULL;
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
