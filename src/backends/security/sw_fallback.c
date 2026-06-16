/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software security fallback.  Wildcard backend at priority 0 --
 * picked only when no higher-priority hardware-aware security
 * backend is linked into the build (the typical native_sim
 * trimmed-image case, or any build with CONFIG_ALP_SDK_SECURITY=n).
 *
 * Contract: NOSUPPORT stub.  It registers (priority 0, "*") only so
 * the `security` class section is never empty -- that keeps the
 * linker emitting the registry's __start_/__stop_ bounds.  It backs
 * no crypto, so every surface reports unsupported:
 *   - hash_open / aead_open  -> ALP_ERR_NOSUPPORT (the dispatcher
 *     relays this as a NULL handle + last_error = NOSUPPORT)
 *   - random_bytes           -> ALP_ERR_NOSUPPORT
 *   - hash_update / finish / aead_encrypt / decrypt
 *     -> ALP_ERR_NOT_IMPLEMENTED (unreachable -- no handle handed out)
 *   - hash_close / aead_close -> no-op
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~300 B, zero RAM (no per-handle backend state --
 *      every state->be_data is left NULL).  No MbedTLS / PSA Crypto
 *      linkage required, so this backend compiles cleanly on
 *      native_sim trimmed-image builds where MbedTLS is absent.
 * @par Performance: O(1) per call; every op short-circuits to
 *      ALP_ERR_NOSUPPORT / NOT_IMPLEMENTED with no library touch.
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

static alp_status_t
sw_hash_open(alp_hash_alg_t alg, alp_hash_backend_state_t *state, alp_capabilities_t *caps_out)
{
	/* NOSUPPORT stub: no software hash on native_sim.  The dispatcher
     * relays this as a NULL handle + last_error = NOSUPPORT. */
	(void)alg;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_hash_update(alp_hash_backend_state_t *state, const uint8_t *data, size_t len)
{
	(void)state;
	(void)data;
	(void)len;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_hash_finish(alp_hash_backend_state_t *state,
                                   uint8_t                  *digest_out,
                                   size_t                    digest_cap,
                                   size_t                   *digest_len)
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

static alp_status_t sw_aead_open(alp_aead_alg_t            alg,
                                 const uint8_t            *key,
                                 size_t                    key_len,
                                 alp_aead_backend_state_t *state,
                                 alp_capabilities_t       *caps_out)
{
	/* NOSUPPORT stub: no software AEAD on native_sim. */
	(void)alg;
	(void)key;
	(void)key_len;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_aead_encrypt(alp_aead_backend_state_t *state,
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
	/* NOSUPPORT stub: no entropy source on native_sim.  Production
     * builds link the zephyr_drv backend (priority 100), which routes
     * through PSA Crypto's DRBG and wins selection ahead of this. */
	(void)out;
	(void)len;
	return ALP_ERR_NOSUPPORT;
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

ALP_BACKEND_REGISTER(security,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
