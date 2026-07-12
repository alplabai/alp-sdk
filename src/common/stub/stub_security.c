/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Security (hash + AEAD + RNG) NOSUPPORT stubs -- <alp/security.h>.
 * Split out of the former src/common/stub_backend.c monolith (issue
 * #673); owns every `alp_hash_*` / `alp_aead_*` / `alp_random_bytes`
 * symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/security.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_SECURITY)
alp_hash_t *alp_hash_open(alp_hash_alg_t a)
{
	(void)a;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_hash_update(alp_hash_t *h, const uint8_t *d, size_t l)
{
	(void)h;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_hash_finish(alp_hash_t *h, uint8_t *o, size_t cap, size_t *ol)
{
	(void)h;
	(void)o;
	(void)cap;
	if (ol != NULL) *ol = 0;
	return ALP_ERR_NOSUPPORT;
}
void alp_hash_close(alp_hash_t *h)
{
	(void)h;
}
const alp_capabilities_t *alp_hash_capabilities(const alp_hash_t *h)
{
	(void)h;
	return NULL;
}
alp_aead_t *alp_aead_open(alp_aead_alg_t a, const uint8_t *k, size_t kl)
{
	(void)a;
	(void)k;
	(void)kl;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_aead_encrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         il,
                              const uint8_t *aad,
                              size_t         al,
                              const uint8_t *p,
                              size_t         pl,
                              uint8_t       *co,
                              uint8_t       *t,
                              size_t         tl)
{
	(void)a;
	(void)iv;
	(void)il;
	(void)aad;
	(void)al;
	(void)p;
	(void)pl;
	(void)co;
	(void)t;
	(void)tl;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_aead_decrypt(alp_aead_t    *a,
                              const uint8_t *iv,
                              size_t         il,
                              const uint8_t *aad,
                              size_t         al,
                              const uint8_t *c,
                              size_t         cl,
                              const uint8_t *t,
                              size_t         tl,
                              uint8_t       *po)
{
	(void)a;
	(void)iv;
	(void)il;
	(void)aad;
	(void)al;
	(void)c;
	(void)cl;
	(void)t;
	(void)tl;
	(void)po;
	return ALP_ERR_NOSUPPORT;
}
void alp_aead_close(alp_aead_t *a)
{
	(void)a;
}
const alp_capabilities_t *alp_aead_capabilities(const alp_aead_t *a)
{
	(void)a;
	return NULL;
}
alp_status_t alp_random_bytes(uint8_t *o, size_t l)
{
	(void)o;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
#endif /* !ALP_VENDOR_OVERRIDES_SECURITY */
