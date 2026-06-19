/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SE-CryptoCell HARDWARE-CRYPTO backend for the <alp/security.h> surface
 * on the Alif Ensemble E8 (AEN801).
 *
 * Where zephyr_drv.c runs hash / AEAD / random on the M55 core via
 * MbedTLS-PSA, this backend pushes the work INTO the Secure Enclave's
 * CryptoCell (CC-3xx) over the RTSS-HE <-> SE MHUv2 mailbox -- the very
 * same transport the read-only se_service client already drives live on
 * the bench (examples/aen/aen-se-service-query proved heartbeat + LCS read
 * + se_service_get_rnd_num round-trip rc=0).  The compute then happens on
 * dedicated silicon with the SE's key isolation, not on the application
 * core.
 *
 * It registers at silicon_ref="alif:ensemble:e8" priority 110, one step
 * above the silicon_ref="*" priority-100 MbedTLS-PSA zephyr_drv backend, so
 * on the E8 the portable surface transparently picks the SE path; every
 * other SoC (and the E8 with this backend Kconfig'd off) keeps the PSA
 * backend.  Nothing in <alp/security.h> names the SE -- the vendor name
 * stays behind the dispatcher.
 *
 * ------------------------------------------------------------------ *
 * RIDES THE EXISTING SE SERVICE -- DOES NOT VENDOR THE DFP HOST LIB    *
 * ------------------------------------------------------------------ *
 * ADR-0017 (alp-sdk over the vendor SDK): the SE CryptoCell host library
 * (alif-dfp se_services/source/services_host_cryptocell.c) is PROPRIETARY
 * and is NOT vendored.  Instead this backend transcribes, clean-room, only
 * the wire facts it needs from the Apache-2.0 hal_alif headers that are
 * already on the build's include path:
 *
 *   - SERVICE_CRYPTOCELL_* request IDs (400..422)
 *       <- modules/hal/alif/se_services/include/services_lib_ids.h
 *   - the request/response packet structs (get_rnd_svc_t, mbedtls_sha_
 *       single_svc_t, mbedtls_aes_svc_t, mbedtls_ccm_gcm_svc_t,
 *       mbedtls_chachapoly_crypt_svc_t, mbedtls_cmac_svc_t, ...)
 *       <- modules/hal/alif/se_services/include/services_lib_protocol.h
 *   - the MbedTLS mode/type/keybits constants (MBEDTLS_HASH_SHA256,
 *       MBEDTLS_OP_ENCRYPT, MBEDTLS_AES_CRYPT_*, MBEDTLS_GCM_*, ...)
 *       <- modules/hal/alif/se_services/include/services_lib_api.h
 *   - local_to_global() address translation for the embedded send_*_addr
 *       fields the SE DMA dereferences
 *       <- modules/hal/alif/common/include/soc_memory_map.h
 *
 * Those headers are pulled in transitively by <se_service.h>, so no DFP
 * source is copied -- we re-use the vendor's own declarations.  The call
 * sequence mirrors alif-dfp se_services/templates/services_test_crypto.c
 * (the single-shot SHA / AES / CCM-GCM / ChaCha-Poly / CMAC paths).
 *
 * ------------------------------------------------------------------ *
 * WHAT IS LIVE TODAY vs. WHAT NEEDS THE SEND SEAM                      *
 * ------------------------------------------------------------------ *
 * RNG / TRNG -- LIVE.  alp_random_bytes() calls the PUBLIC hal_alif
 * se_service_get_rnd_num(), which sends SERVICE_CRYPTOCELL_GET_RND and
 * returns the SE TRNG bytes.  This is the same call the query example pulls
 * 8 bytes through rc=0.
 *
 * AES / SHA / CMAC / CCM / GCM / ChaCha20 / Poly1305 -- WIRED, GATED.
 * These build a fully populated CryptoCell request packet (correct ID,
 * struct layout, MbedTLS constants, and local_to_global() on every
 * send_*_addr) and hand it to alp_se_crypto_send_request(), which calls the
 * PUBLIC se_service_send_request() -- a generic SE-packet transport added to
 * hal_alif by zephyr/patches/hal_alif/0002-se-service-add-public-send-request
 * .patch (it runs the same se_service_ensure_ready + svc_mutex +
 * send_msg_to_se path the RNG entry rides).  The compute ops stay gated on
 * CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM, which builds + links but
 * defaults OFF until the on-silicon round-trip is bench-validated (turning it
 * ON makes the SE the default E8 crypto path ahead of PSA).  With it OFF they
 * return ALP_ERR_NOSUPPORT and the dispatcher falls through to the PSA
 * backend; flip the Kconfig on the bench and the wire requests below go live
 * unchanged.
 *
 * @par Address translation: every send_*_addr the SE dereferences is a
 *      GLOBAL address (the SE's view of M55-local memory), produced by
 *      local_to_global() (soc_memory_map.h), which reads the itcm/dtcm
 *      `global_base` DT props.  The board overlay must supply those (the
 *      alif,itcm/alif,dtcm bindings) or the translation is identity and
 *      TCM-resident buffers are unreachable by the SE.
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

#if defined(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL)

/*
 * hal_alif SE-service client (Apache-2.0).  Transitively provides
 * services_lib_ids.h (SERVICE_CRYPTOCELL_* IDs), services_lib_protocol.h
 * (the *_svc_t request structs), and services_lib_api.h (the MbedTLS
 * mode/type constants).  se_service_get_rnd_num() is the public RNG entry.
 */
#include <se_service.h>

#if defined(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM)
/* local_to_global() for the embedded send_*_addr fields.  Only needed on
 * the compute paths; the RNG path goes through the public client which does
 * its own translation internally. */
#include <soc_memory_map.h>
#endif

#ifndef CONFIG_ALP_SDK_MAX_HASH_HANDLES
#define CONFIG_ALP_SDK_MAX_HASH_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_AEAD_HANDLES
#define CONFIG_ALP_SDK_MAX_AEAD_HANDLES 2
#endif

/*
 * SE TRNG single-pull ceiling.  hal_alif's get_rnd_svc_t carries a fixed
 * resp_rnd[MAX_RND_LENGTH] inline response buffer; MAX_RND_LENGTH = 256
 * (services_lib_protocol.h: "MBEDTLS_CTR_DRBG_MAX_REQUEST in cryptocell-rt
 * is 1024, it was decided that we will use a smaller buffer").  Larger
 * alp_random_bytes() requests chunk under this ceiling.
 */
#define SE_RND_MAX_CHUNK 256u

/* ------------------------------------------------------------------ */
/* errno (hal_alif transport) -> alp_status_t                          */
/*                                                                     */
/* se_service_* return 0 on success, a negative errno for the transport */
/* (-EAGAIN timeout, -EBUSY SE busy, -EINVAL bad arg), or a POSITIVE SE */
/* firmware error code for a serviced-but-rejected request.            */
/* ------------------------------------------------------------------ */

static alp_status_t se_rc_to_alp(int rc)
{
	if (rc == 0) {
		return ALP_OK;
	}
	switch (rc) {
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EAGAIN:
	case -EBUSY:
		return ALP_ERR_NOT_READY;
	default:
		/* Negative non-handled errno or a positive SE firmware error
		 * code: report as an I/O failure (the SE answered but the
		 * operation did not complete). */
		return ALP_ERR_IO;
	}
}

/* ================================================================== */
/* Backend-owned per-handle state                                      */
/* ================================================================== */

/*
 * The SE CryptoCell SHA path is a STREAMING (starts / update / finish)
 * service that lives in the SE keeping the running context SE-side, OR a
 * single-shot mbedtls_sha over a contiguous buffer.  alp_hash is itself a
 * streaming API (open / update* / finish), but the single-shot SE service
 * needs all the data contiguous at finish time.  To bridge the two without
 * an SE-side context handle (which the public client does not expose), we
 * buffer the fed bytes locally and issue ONE single-shot
 * SERVICE_CRYPTOCELL_MBEDTLS_SHA at finish.  Bounded buffer; larger inputs
 * need the streaming starts/update/finish services once the send seam is
 * live (see notes at the bottom).
 */
#ifndef CONFIG_ALP_SDK_SECURITY_SE_HASH_BUF
#define CONFIG_ALP_SDK_SECURITY_SE_HASH_BUF 512
#endif

struct se_hash_be {
	bool    in_use;
	size_t  used;
	uint8_t buf[CONFIG_ALP_SDK_SECURITY_SE_HASH_BUF];
};

struct se_aead_be {
	bool    in_use;
	uint8_t key[32]; /* up to 256-bit AES / ChaCha20 key */
	size_t  key_len;
};

static struct se_hash_be g_se_hash_pool[CONFIG_ALP_SDK_MAX_HASH_HANDLES];
static struct se_aead_be g_se_aead_pool[CONFIG_ALP_SDK_MAX_AEAD_HANDLES];

static struct se_hash_be *se_hash_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_se_hash_pool); ++i) {
		if (!g_se_hash_pool[i].in_use) {
			memset(&g_se_hash_pool[i], 0, sizeof(g_se_hash_pool[i]));
			g_se_hash_pool[i].in_use = true;
			return &g_se_hash_pool[i];
		}
	}
	return NULL;
}

static struct se_aead_be *se_aead_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_se_aead_pool); ++i) {
		if (!g_se_aead_pool[i].in_use) {
			memset(&g_se_aead_pool[i], 0, sizeof(g_se_aead_pool[i]));
			g_se_aead_pool[i].in_use = true;
			return &g_se_aead_pool[i];
		}
	}
	return NULL;
}

/* SHA byte length per alg.  Only SHA-256 is offered by the E8 CryptoCell
 * (services_lib_api.h declares MBEDTLS_HASH_SHA1/SHA224/SHA256 only -- no
 * SHA-384/512), so 384/512 fall through to the PSA backend. */
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

/* ================================================================== */
/* Generic SE crypto send seam                                         */
/*                                                                     */
/* All the AES/SHA/CMAC/CCM/GCM/ChaCha/Poly requests need a transport   */
/* that sends a fully populated CryptoCell packet over the MHUv2 pair   */
/* and blocks for the SE response.  hal_alif's send_msg_to_se() does    */
/* exactly that but is static; no public generic-send wrapper exists.   */
/* This is the single seam to wire when that lands.  Until then it is   */
/* unimplemented and every compute op is compiled out behind            */
/* CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM.                     */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM)

/*
 * @brief Send a fully populated CryptoCell request packet to the SE and
 *        block for the response.
 *
 * Binds to the public hal_alif se_service_send_request() transport (added by
 * zephyr/patches/hal_alif/0002-se-service-add-public-send-request.patch),
 * which ensures the SE is ready, serialises on the SE service mutex, and
 * hands the packet to the static send_msg_to_se() over the RTSS-HE <-> SE
 * MHUv2 pair -- the identical path the public se_service_get_rnd_num() rides.
 *
 * @param[in,out] packet   Packet whose header.hdr_service_id is set and
 *                          whose send_* fields are populated (with every
 *                          send_*_addr already run through
 *                          local_to_global()).  On return the resp_*
 *                          fields are filled by the SE.
 * @param[in]     size      sizeof the concrete *_svc_t.
 * @param[in]     svc_id    SERVICE_CRYPTOCELL_* id; already carried in the
 *                          packet header, kept for call-site readability.
 * @return 0 on a completed transport round-trip (the caller then reads the
 *         packet's resp_error_code); a negative errno on transport failure.
 */
static int alp_se_crypto_send_request(void *packet, size_t size, uint16_t svc_id)
{
	(void)svc_id; /* the id is already in packet->header.hdr_service_id */
	return se_service_send_request((uint32_t *)packet, (uint32_t)size);
}

#endif /* CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM */

/* ================================================================== */
/* Hash ops                                                            */
/* ================================================================== */

static alp_status_t
se_hash_open(alp_hash_alg_t alg, alp_hash_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)caps_out;

	/* Only SHA-256 maps to the E8 CryptoCell mbedtls SHA service
	 * (MBEDTLS_HASH_SHA256).  Let SHA-384/512 fall through to PSA by
	 * declining here -- the dispatcher relays NOSUPPORT and the caller
	 * retries against the next backend. */
	if (alg != ALP_HASH_SHA256) {
		return ALP_ERR_NOSUPPORT;
	}

	struct se_hash_be *be = se_hash_acquire();
	if (be == NULL) {
		return ALP_ERR_NOMEM;
	}
	state->alg     = alg;
	state->be_data = be;
	return ALP_OK;
}

static alp_status_t se_hash_update(alp_hash_backend_state_t *state, const uint8_t *data, size_t len)
{
	struct se_hash_be *be = (struct se_hash_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}
	if (data == NULL && len != 0u) {
		return ALP_ERR_INVAL;
	}
	if (be->used + len > sizeof(be->buf)) {
		/* Single-shot buffer overflow.  The SE streaming SHA
		 * (starts/update/finish) removes this ceiling once the send
		 * seam is live; for now decline so the caller can pick PSA. */
		return ALP_ERR_NOSUPPORT;
	}
	if (len != 0u) {
		memcpy(be->buf + be->used, data, len);
		be->used += len;
	}
	return ALP_OK;
}

static alp_status_t se_hash_finish(alp_hash_backend_state_t *state,
                                   uint8_t                  *digest_out,
                                   size_t                    digest_cap,
                                   size_t                   *digest_len)
{
	struct se_hash_be *be = (struct se_hash_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}

	const size_t dlen = alp_hash_digest_len(state->alg); /* 32 for SHA-256 */
	if (digest_out == NULL || digest_cap < dlen) {
		return ALP_ERR_INVAL;
	}

#if defined(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM)
	/* Single-shot SE SHA over the buffered bytes.  Wire layout per
	 * mbedtls_sha_single_svc_t (services_lib_protocol.h):
	 *   send_sha_type    = MBEDTLS_HASH_SHA256 (=2, services_lib_api.h)
	 *   send_data_addr   = local_to_global(input buffer)
	 *   send_data_length = byte count
	 *   send_shasum_addr = local_to_global(32-byte digest out)
	 * Call sequence mirrors services_test_crypto.c test_mbedtls_sha(). */
	mbedtls_sha_single_svc_t pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.hdr_service_id = SERVICE_CRYPTOCELL_MBEDTLS_SHA; /* 420, services_lib_ids.h */
	pkt.send_sha_type         = MBEDTLS_HASH_SHA256;            /* 2, services_lib_api.h */
	pkt.send_data_addr        = local_to_global(be->buf);
	pkt.send_data_length      = (uint32_t)be->used;
	pkt.send_shasum_addr      = local_to_global(digest_out);

	alp_status_t s =
	    (alp_se_crypto_send_request(&pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_SHA) == 0)
	        ? se_rc_to_alp((int)pkt.resp_error_code)
	        : ALP_ERR_IO;
	be->in_use     = false;
	state->be_data = NULL;
	if (s == ALP_OK && digest_len != NULL) {
		*digest_len = dlen;
	}
	return s;
#else
	/* Send seam not wired: decline so the dispatcher falls through to
	 * the PSA backend, which finishes the digest on the M55. */
	be->in_use     = false;
	state->be_data = NULL;
	if (digest_len != NULL) {
		*digest_len = 0u;
	}
	return ALP_ERR_NOSUPPORT;
#endif
}

static void se_hash_close(alp_hash_backend_state_t *state)
{
	struct se_hash_be *be = (struct se_hash_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return;
	}
	/* Wipe buffered plaintext before releasing the slot. */
	memset(be, 0, sizeof(*be));
	state->be_data = NULL;
}

/* ================================================================== */
/* AEAD ops                                                            */
/* ================================================================== */

/* alp AEAD alg -> SE key bits + ChaCha flag.  AES-GCM uses the CCM/GCM
 * service (MBEDTLS_GCM_KEY); ChaCha20-Poly1305 uses the chachapoly
 * service.  Key-length validation matches the public API contract
 * (<alp/security.h>: 16 B AES-128, 32 B AES-256 / ChaCha20). */
static alp_status_t se_aead_keybits(alp_aead_alg_t a, size_t key_len, uint32_t *out_key_bits)
{
	switch (a) {
	case ALP_AEAD_AES_128_GCM:
		if (key_len != 16u) {
			return ALP_ERR_INVAL;
		}
		*out_key_bits = MBEDTLS_AES_KEY_128; /* 128, services_lib_api.h */
		return ALP_OK;
	case ALP_AEAD_AES_256_GCM:
		if (key_len != 32u) {
			return ALP_ERR_INVAL;
		}
		*out_key_bits = MBEDTLS_AES_KEY_256; /* 256, services_lib_api.h */
		return ALP_OK;
	case ALP_AEAD_CHACHA20_POLY1305:
		if (key_len != 32u) {
			return ALP_ERR_INVAL;
		}
		*out_key_bits = 256u; /* ChaCha20 key is fixed 256-bit */
		return ALP_OK;
	default:
		return ALP_ERR_INVAL;
	}
}

static alp_status_t se_aead_open(alp_aead_alg_t            alg,
                                 const uint8_t            *key,
                                 size_t                    key_len,
                                 alp_aead_backend_state_t *state,
                                 alp_capabilities_t       *caps_out)
{
	(void)caps_out;

	uint32_t key_bits;
	if (se_aead_keybits(alg, key_len, &key_bits) != ALP_OK) {
		return ALP_ERR_INVAL;
	}
	if (key == NULL || key_len > sizeof(((struct se_aead_be *)0)->key)) {
		return ALP_ERR_INVAL;
	}

	struct se_aead_be *be = se_aead_acquire();
	if (be == NULL) {
		return ALP_ERR_NOMEM;
	}
	memcpy(be->key, key, key_len);
	be->key_len    = key_len;
	state->alg     = alg;
	state->be_data = be;
	return ALP_OK;
}

static alp_status_t se_aead_encrypt(alp_aead_backend_state_t *state,
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
	struct se_aead_be *be = (struct se_aead_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}

#if defined(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM)
	uint32_t key_bits;
	(void)se_aead_keybits(state->alg, be->key_len, &key_bits);

	if (state->alg == ALP_AEAD_CHACHA20_POLY1305) {
		/* mbedtls_chachapoly_crypt_svc_t (services_lib_protocol.h):
		 * send_crypt_type = MBEDTLS_CHACHAPOLY_ENCRYPT_AND_TAG (0). */
		mbedtls_chachapoly_crypt_svc_t pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.header.hdr_service_id = SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT; /* 408 */
		pkt.send_crypt_type       = MBEDTLS_CHACHAPOLY_ENCRYPT_AND_TAG;          /* 0 */
		pkt.send_length           = (uint32_t)plain_len;
		pkt.send_nonce_addr       = local_to_global(iv);
		pkt.send_aad_addr         = local_to_global(aad);
		pkt.send_aad_len          = (uint32_t)aad_len;
		pkt.send_tag_addr         = local_to_global(tag_out);
		pkt.send_input_addr       = local_to_global(plain);
		pkt.send_output_addr      = local_to_global(cipher_out);
		/* send_context_addr is the SE-side chachapoly context; the
		 * single-shot path leaves it 0 (set up SE-side). */
		(void)iv_len;
		(void)tag_len;
		return (alp_se_crypto_send_request(
		            &pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT) == 0)
		           ? se_rc_to_alp((int)pkt.resp_error_code)
		           : ALP_ERR_IO;
	}

	/* AES-GCM via the single-shot CCM/GCM service.  mbedtls_ccm_gcm_svc_t
	 * (services_lib_protocol.h):
	 *   send_crypt_type = MBEDTLS_GCM_ENCRYPT_AND_TAG (4, services_lib_api.h)
	 *   send_key_addr   = local_to_global(key); send_key_bits = 128/256
	 *   send_iv/add/input/output/tag addrs = local_to_global(...) */
	mbedtls_ccm_gcm_svc_t pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.hdr_service_id = SERVICE_CRYPTOCELL_MBEDTLS_CCM_GCM; /* 422 */
	pkt.send_crypt_type       = MBEDTLS_GCM_ENCRYPT_AND_TAG;        /* 4 */
	pkt.send_key_addr         = local_to_global(be->key);
	pkt.send_key_bits         = key_bits;
	pkt.send_length           = (uint32_t)plain_len;
	pkt.send_iv_addr          = local_to_global(iv);
	pkt.send_iv_length        = (uint32_t)iv_len;
	pkt.send_add_addr         = local_to_global(aad);
	pkt.send_add_length       = (uint32_t)aad_len;
	pkt.send_input_addr       = local_to_global(plain);
	pkt.send_output_addr      = local_to_global(cipher_out);
	pkt.send_tag_addr         = local_to_global(tag_out);
	pkt.send_tag_length       = (uint32_t)tag_len;
	return (alp_se_crypto_send_request(&pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CCM_GCM) == 0)
	           ? se_rc_to_alp((int)pkt.resp_error_code)
	           : ALP_ERR_IO;
#else
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

static alp_status_t se_aead_decrypt(alp_aead_backend_state_t *state,
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
	struct se_aead_be *be = (struct se_aead_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return ALP_ERR_NOT_READY;
	}

#if defined(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM)
	uint32_t key_bits;
	(void)se_aead_keybits(state->alg, be->key_len, &key_bits);

	if (state->alg == ALP_AEAD_CHACHA20_POLY1305) {
		/* MBEDTLS_CHACHAPOLY_AUTH_DECRYPT (1, services_lib_api.h). */
		mbedtls_chachapoly_crypt_svc_t pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.header.hdr_service_id = SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT; /* 408 */
		pkt.send_crypt_type       = MBEDTLS_CHACHAPOLY_AUTH_DECRYPT;             /* 1 */
		pkt.send_length           = (uint32_t)cipher_len;
		pkt.send_nonce_addr       = local_to_global(iv);
		pkt.send_aad_addr         = local_to_global(aad);
		pkt.send_aad_len          = (uint32_t)aad_len;
		pkt.send_tag_addr         = local_to_global(tag);
		pkt.send_input_addr       = local_to_global(cipher);
		pkt.send_output_addr      = local_to_global(plain_out);
		(void)iv_len;
		(void)tag_len;
		return (alp_se_crypto_send_request(
		            &pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT) == 0)
		           ? se_rc_to_alp((int)pkt.resp_error_code)
		           : ALP_ERR_IO;
	}

	/* AES-GCM auth-decrypt: MBEDTLS_GCM_AUTH_DECRYPT (6, services_lib_api.h).
	 * A tag mismatch surfaces as a positive SE error -> ALP_ERR_IO, which
	 * the public contract documents as the tamper signal. */
	mbedtls_ccm_gcm_svc_t pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.hdr_service_id = SERVICE_CRYPTOCELL_MBEDTLS_CCM_GCM; /* 422 */
	pkt.send_crypt_type       = MBEDTLS_GCM_AUTH_DECRYPT;           /* 6 */
	pkt.send_key_addr         = local_to_global(be->key);
	pkt.send_key_bits         = key_bits;
	pkt.send_length           = (uint32_t)cipher_len;
	pkt.send_iv_addr          = local_to_global(iv);
	pkt.send_iv_length        = (uint32_t)iv_len;
	pkt.send_add_addr         = local_to_global(aad);
	pkt.send_add_length       = (uint32_t)aad_len;
	pkt.send_input_addr       = local_to_global(cipher);
	pkt.send_output_addr      = local_to_global(plain_out);
	pkt.send_tag_addr         = local_to_global(tag);
	pkt.send_tag_length       = (uint32_t)tag_len;
	return (alp_se_crypto_send_request(&pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CCM_GCM) == 0)
	           ? se_rc_to_alp((int)pkt.resp_error_code)
	           : ALP_ERR_IO;
#else
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

static void se_aead_close(alp_aead_backend_state_t *state)
{
	struct se_aead_be *be = (struct se_aead_be *)state->be_data;
	if (be == NULL || !be->in_use) {
		return;
	}
	/* Wipe key material before releasing the slot. */
	memset(be, 0, sizeof(*be));
	state->be_data = NULL;
}

/* ================================================================== */
/* Random (stateless) -- LIVE via the public hal_alif RNG entry        */
/* ================================================================== */

static alp_status_t se_random_bytes(uint8_t *out, size_t len)
{
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}
	if (len == 0u) {
		return ALP_OK;
	}

	/* se_service_get_rnd_num() sends SERVICE_CRYPTOCELL_GET_RND (400) and
	 * copies the SE TRNG bytes into out.  The inline response buffer is
	 * MAX_RND_LENGTH (256) wide, so chunk larger requests.  This is the
	 * same public call the read-only query example pulls 8 bytes through
	 * rc=0 on the bench. */
	size_t done = 0u;
	while (done < len) {
		const size_t   remaining = len - done;
		const uint16_t chunk =
		    (uint16_t)((remaining > SE_RND_MAX_CHUNK) ? SE_RND_MAX_CHUNK : remaining);

		const int rc = se_service_get_rnd_num(out + done, chunk);
		if (rc != 0) {
			return se_rc_to_alp(rc);
		}
		done += chunk;
	}
	return ALP_OK;
}

/* ---------- Registration ---------- */

static const alp_security_ops_t _ops = {
	.hash_open    = se_hash_open,
	.hash_update  = se_hash_update,
	.hash_finish  = se_hash_finish,
	.hash_close   = se_hash_close,
	.aead_open    = se_aead_open,
	.aead_encrypt = se_aead_encrypt,
	.aead_decrypt = se_aead_decrypt,
	.aead_close   = se_aead_close,
	.random_bytes = se_random_bytes,
};

/*
 * silicon_ref pinned to the E8 (the only Ensemble part whose SE CryptoCell
 * services this backend has been wired + bench-characterised for, via the
 * live se_service transport).  priority 110 outranks the "*" priority-100
 * PSA zephyr_drv, so on the E8 the SE path wins selection; everywhere else
 * the PSA backend stays selected.
 */
ALP_BACKEND_REGISTER(security,
                     se_cryptocell,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 110,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL */
