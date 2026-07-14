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
 * send_msg_to_se path the RNG entry rides).  The compute ops are gated on
 * CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM, which DEFAULTS ON --
 * bench-validated on real E8 silicon (aen-se-crypto, Flow C RAM-run): the SE
 * backend binds at priority 110 and the SE CryptoCell computed SHA-256("abc")
 * to the NIST known-answer and the AES-128-GCM round-trip, both MATCH.  With
 * it OFF every hash/AEAD open declines with ALP_ERR_NOSUPPORT (as do
 * SHA-384/512, which the E8 SE does not implement) and the dispatcher
 * re-selects the next backend at open time (alp_backend_select_next, issue
 * #239), landing on PSA before any data is consumed.  An over-ceiling
 * SHA-256 input cannot fall through that way -- bytes have already been fed
 * to the bound backend -- so the SE hash handle instead migrates itself to
 * the PSA backend in place once the buffered byte count exceeds the
 * single-shot ceiling (see se_hash_update).
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
	bool in_use;
	/*
	 * Once the buffered byte count would exceed the single-shot SE SHA
	 * ceiling, this handle transparently switches to the portable PSA
	 * (zephyr_drv) backend in place: the already-buffered bytes are
	 * replayed into a PSA hash and every subsequent update/finish/close
	 * routes through `psa_ops` instead of the SE single-shot service.
	 * The dispatcher binds one backend at hash_open and cannot re-select
	 * mid-stream, so the SE backend owns the fall-through itself rather
	 * than returning NOSUPPORT after data has already been consumed.
	 */
	bool                      delegated;
	const alp_security_ops_t *psa_ops;
	alp_hash_backend_state_t  psa_state;
	size_t                    used;
	uint8_t                   buf[CONFIG_ALP_SDK_SECURITY_SE_HASH_BUF];
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

/*
 * Highest-priority security backend that is NOT this SE one -- i.e. the
 * portable PSA (zephyr_drv, "*"/100) path the SE hash falls through to for
 * inputs over the single-shot ceiling.  Walks the same per-class section the
 * dispatcher selects from; the section bounds are emitted by the security
 * dispatcher's ALP_BACKEND_DEFINE_CLASS(security).
 */
extern const alp_backend_t __start_alp_backends_security[];
extern const alp_backend_t __stop_alp_backends_security[];

static const alp_security_ops_t _ops; /* this backend's vtable, defined below */

static const alp_security_ops_t *se_hash_fallback_ops(void)
{
	const alp_backend_t *best = NULL;
	for (const alp_backend_t *be = __start_alp_backends_security; be < __stop_alp_backends_security;
	     ++be) {
		if (be->ops == &_ops) {
			continue; /* skip ourselves -- we only want the next path */
		}
		if (best == NULL || be->priority > best->priority) {
			best = be;
		}
	}
	return (best != NULL) ? (const alp_security_ops_t *)best->ops : NULL;
}

/*
 * Promote a buffering SE hash handle to the PSA backend: open a PSA hash for
 * the same alg and replay every byte buffered so far.  After this returns
 * ALP_OK the handle is `delegated` and all further update/finish/close route
 * through `psa_ops`.  Any failure leaves the handle untouched so the caller
 * sees a single hard error rather than a half-migrated stream.
 */
static alp_status_t se_hash_delegate(alp_hash_backend_state_t *state, struct se_hash_be *be)
{
	const alp_security_ops_t *psa = se_hash_fallback_ops();
	if (psa == NULL || psa->hash_open == NULL || psa->hash_update == NULL) {
		return ALP_ERR_NOSUPPORT;
	}

	alp_hash_backend_state_t ps   = { 0 };
	alp_capabilities_t       caps = { 0 };
	alp_status_t             s    = psa->hash_open(state->alg, &ps, &caps);
	if (s != ALP_OK) {
		return s;
	}
	if (be->used != 0u) {
		s = psa->hash_update(&ps, be->buf, be->used);
		if (s != ALP_OK) {
			if (psa->hash_close != NULL) {
				psa->hash_close(&ps);
			}
			return s;
		}
	}

	/* Buffered plaintext is now mirrored into the PSA context; wipe it. */
	memset(be->buf, 0, sizeof(be->buf));
	be->used      = 0u;
	be->psa_ops   = psa;
	be->psa_state = ps;
	be->delegated = true;
	return ALP_OK;
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

	/* Issue #239: with the send seam compiled out this backend cannot
	 * finish ANY digest, so declare the limit here at open time -- the
	 * dispatcher re-selects on NOSUPPORT (alp_backend_select_next) and
	 * binds the PSA backend before a single byte is consumed.  Failing
	 * later, at finish, would be a hard app error: no re-selection can
	 * replay data fed to a dead stream. */
	if (!IS_ENABLED(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM)) {
		return ALP_ERR_NOSUPPORT;
	}

	/* Only SHA-256 maps to the E8 CryptoCell mbedtls SHA service
	 * (MBEDTLS_HASH_SHA256).  Let SHA-384/512 fall through to PSA by
	 * declining here -- the dispatcher re-selects on NOSUPPORT at open
	 * (issue #239) and retries against the next backend. */
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
	if (be->delegated) {
		return be->psa_ops->hash_update(&be->psa_state, data, len);
	}
	if (be->used + len > sizeof(be->buf)) {
		/* Input exceeds the single-shot SE SHA buffer.  The SE public
		 * client exposes no streaming starts/update/finish service, and
		 * the dispatcher already bound this backend at hash_open, so
		 * migrate the handle to the portable PSA backend in place (it
		 * replays the buffered bytes) and feed this chunk through it. */
		alp_status_t s = se_hash_delegate(state, be);
		if (s != ALP_OK) {
			return s;
		}
		return be->psa_ops->hash_update(&be->psa_state, data, len);
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
		/* GHSA-92c3-v48m-m5gg: report the required length but do NOT
		 * touch `be` / `state->be_data` here.  <alp/security.h> only
		 * lets ALP_OK implicitly close the handle -- a short output
		 * buffer must leave the SE slot valid so the dispatcher's
		 * outer handle stays open for either a correctly sized retry
		 * (same buffered bytes, re-issue finish) or an explicit
		 * alp_hash_close().  Stranding be->in_use here (or freeing it
		 * while the dispatcher keeps the outer handle alive) is
		 * exactly the leak this backend used to have. */
		if (digest_len != NULL) {
			*digest_len = dlen;
		}
		return ALP_ERR_INVAL;
	}

	if (be->delegated) {
		/* Over-ceiling input was migrated to PSA; let it finish. */
		alp_status_t s =
		    be->psa_ops->hash_finish(&be->psa_state, digest_out, digest_cap, digest_len);
		/* Wipe the whole backend slot (buffered bytes, delegated PSA
		 * copy) before release rather than leaving it to the next
		 * se_hash_acquire() -- sensitive material shouldn't linger in
		 * a "free" slot any longer than necessary. */
		memset(be, 0, sizeof(*be));
		state->be_data = NULL;
		return s;
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
	if (digest_len != NULL) {
		/* Parity with the yocto backend: report the digest length on
		 * success, or 0 on the terminal-IO-failure path (this is not
		 * the short-buffer path -- that already returned above). */
		*digest_len = (s == ALP_OK) ? dlen : 0u;
	}
	/* Wipe the buffered plaintext before releasing the slot, on both
	 * the ALP_OK and the ALP_ERR_IO paths -- either way the dispatcher
	 * has already run/will run its own close-or-not decision on the
	 * PUBLIC handle; this backend's own resource is done with either
	 * outcome and must not hold sensitive bytes past this point. */
	memset(be, 0, sizeof(*be));
	state->be_data = NULL;
	return s;
#else
	/* Unreachable by construction: with the send seam off, se_hash_open
	 * declines at open (issue #239) and no SE hash handle exists.  Keep
	 * the defensive decline so the vtable entry still compiles. */
	memset(be, 0, sizeof(*be));
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
	/* Tear down the delegated PSA context before wiping our slot. */
	if (be->delegated && be->psa_ops != NULL && be->psa_ops->hash_close != NULL) {
		be->psa_ops->hash_close(&be->psa_state);
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

	/* Issue #239 (same shape as se_hash_open): without the send seam no
	 * encrypt/decrypt can ever complete, so decline at open and let the
	 * dispatcher re-select the PSA backend instead of handing out a
	 * handle whose every I/O op would hard-fail. */
	if (!IS_ENABLED(CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM)) {
		return ALP_ERR_NOSUPPORT;
	}

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

	/* Issue #246: the SE forwards send_iv_length / send_tag_length to its
	 * mbedtls core unchecked, so a short tag would silently downgrade the
	 * authentication strength.  Enforce the public contract before the
	 * packet is built: IV is 12 B for both AES-GCM and ChaCha20-Poly1305
	 * (<alp/security.h> @param iv), and the tag is exactly 16 B -- the
	 * only length the PSA sibling backend round-trips (zephyr_drv.c
	 * z_aead_encrypt rejects every other produced/tag_len combination),
	 * so behavior does not diverge by backend. */
	if (iv_len != 12u || tag_len != 16u) {
		return ALP_ERR_INVAL;
	}
	/* Issue #245: aad == NULL with aad_len == 0 is a legitimate
	 * "no associated data" call (<alp/security.h> @param aad), but
	 * local_to_global(NULL) would hand the SE a translated bogus global
	 * address.  Follow the packet convention for absent buffers -- the
	 * memset(&pkt, 0, ...) leaves unused addr fields 0 -- and reject the
	 * contradictory NULL-with-length combination outright. */
	if (aad == NULL && aad_len != 0u) {
		return ALP_ERR_INVAL;
	}
	const uint32_t aad_gaddr = (aad_len != 0u) ? local_to_global(aad) : 0u;

	if (state->alg == ALP_AEAD_CHACHA20_POLY1305) {
		/* Issue #237: the SE chachapoly service takes its 256-bit key
		 * ONLY via send_context_addr -- a 32-byte context buffer seeded
		 * with the key (alif-dfp services_test_crypto.c:1064-1095 does
		 * memcpy(context, key, 32) and passes the context address; the
		 * struct has no send_key_addr field, see
		 * mbedtls_chachapoly_crypt_svc_t in services_lib_protocol.h).
		 * Stage a disposable copy so an SE write-back to the context can
		 * never corrupt the stored key. */
		uint8_t se_ctx[32];
		memcpy(se_ctx, be->key, sizeof(se_ctx));

		/* mbedtls_chachapoly_crypt_svc_t (services_lib_protocol.h):
		 * send_crypt_type = MBEDTLS_CHACHAPOLY_ENCRYPT_AND_TAG (0). */
		mbedtls_chachapoly_crypt_svc_t pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.header.hdr_service_id = SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT; /* 408 */
		pkt.send_context_addr     = local_to_global(se_ctx);
		pkt.send_crypt_type       = MBEDTLS_CHACHAPOLY_ENCRYPT_AND_TAG; /* 0 */
		pkt.send_length           = (uint32_t)plain_len;
		pkt.send_nonce_addr       = local_to_global(iv);
		pkt.send_aad_addr         = aad_gaddr;
		pkt.send_aad_len          = (uint32_t)aad_len;
		pkt.send_tag_addr         = local_to_global(tag_out);
		pkt.send_input_addr       = local_to_global(plain);
		pkt.send_output_addr      = local_to_global(cipher_out);

		const int rc = alp_se_crypto_send_request(
		    &pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT);

		/* Wipe the staged key copy off the stack.  The barrier keeps the
		 * compiler from eliding the memset on the dying frame. */
		memset(se_ctx, 0, sizeof(se_ctx));
		__asm__ volatile("" ::: "memory");

		return (rc == 0) ? se_rc_to_alp((int)pkt.resp_error_code) : ALP_ERR_IO;
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
	pkt.send_add_addr         = aad_gaddr; /* 0 when no AAD (issue #245) */
	pkt.send_add_length       = (uint32_t)aad_len;
	pkt.send_input_addr       = local_to_global(plain);
	pkt.send_output_addr      = local_to_global(cipher_out);
	pkt.send_tag_addr         = local_to_global(tag_out);
	pkt.send_tag_length       = (uint32_t)tag_len;
	return (alp_se_crypto_send_request(&pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CCM_GCM) == 0)
	           ? se_rc_to_alp((int)pkt.resp_error_code)
	           : ALP_ERR_IO;
#else
	/* Unreachable by construction: with the send seam off, se_aead_open
	 * declines at open (issue #239) and no SE AEAD handle exists. */
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

	/* Issues #245/#246: same parameter validation as se_aead_encrypt --
	 * 12-B IV + 16-B tag per the <alp/security.h> contract and PSA-backend
	 * parity; NULL aad only with aad_len == 0, passed to the SE as
	 * address 0 per the all-zeros packet convention for absent buffers. */
	if (iv_len != 12u || tag_len != 16u) {
		return ALP_ERR_INVAL;
	}
	if (aad == NULL && aad_len != 0u) {
		return ALP_ERR_INVAL;
	}
	const uint32_t aad_gaddr = (aad_len != 0u) ? local_to_global(aad) : 0u;

	if (state->alg == ALP_AEAD_CHACHA20_POLY1305) {
		/* Issue #237: stage the 256-bit key into a disposable 32-byte SE
		 * context -- the chachapoly service's only key channel (see the
		 * encrypt path above for the alif-dfp reference). */
		uint8_t se_ctx[32];
		memcpy(se_ctx, be->key, sizeof(se_ctx));

		/* MBEDTLS_CHACHAPOLY_AUTH_DECRYPT (1, services_lib_api.h). */
		mbedtls_chachapoly_crypt_svc_t pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.header.hdr_service_id = SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT; /* 408 */
		pkt.send_context_addr     = local_to_global(se_ctx);
		pkt.send_crypt_type       = MBEDTLS_CHACHAPOLY_AUTH_DECRYPT; /* 1 */
		pkt.send_length           = (uint32_t)cipher_len;
		pkt.send_nonce_addr       = local_to_global(iv);
		pkt.send_aad_addr         = aad_gaddr;
		pkt.send_aad_len          = (uint32_t)aad_len;
		pkt.send_tag_addr         = local_to_global(tag);
		pkt.send_input_addr       = local_to_global(cipher);
		pkt.send_output_addr      = local_to_global(plain_out);

		const int rc = alp_se_crypto_send_request(
		    &pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT);

		/* Wipe the staged key copy (see encrypt path). */
		memset(se_ctx, 0, sizeof(se_ctx));
		__asm__ volatile("" ::: "memory");

		return (rc == 0) ? se_rc_to_alp((int)pkt.resp_error_code) : ALP_ERR_IO;
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
	pkt.send_add_addr         = aad_gaddr; /* 0 when no AAD (issue #245) */
	pkt.send_add_length       = (uint32_t)aad_len;
	pkt.send_input_addr       = local_to_global(cipher);
	pkt.send_output_addr      = local_to_global(plain_out);
	pkt.send_tag_addr         = local_to_global(tag);
	pkt.send_tag_length       = (uint32_t)tag_len;
	return (alp_se_crypto_send_request(&pkt, sizeof(pkt), SERVICE_CRYPTOCELL_MBEDTLS_CCM_GCM) == 0)
	           ? se_rc_to_alp((int)pkt.resp_error_code)
	           : ALP_ERR_IO;
#else
	/* Unreachable by construction (see se_aead_encrypt / issue #239). */
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
