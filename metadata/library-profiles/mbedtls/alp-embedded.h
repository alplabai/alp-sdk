/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * MbedTLS configuration header for the ALP SDK's embedded targets.
 *
 * MbedTLS has ~400 compile-time switches in mbedtls_config.h
 * controlling cipher / KDF / TLS feature presence.  This profile
 * enables a **minimal-but-modern** subset that covers the SDK's
 * actual use cases:
 *
 *   - <alp/security.h> hash + AEAD + TRNG
 *   - <alp/iot.h> MQTT-over-TLS 1.3
 *   - MCUboot signature verification (RSA-2048 + ECDSA-P256)
 *   - OTA payload signature verification
 *
 * What's IN this profile:
 *   - SHA-256, SHA-384, SHA-512 (no MD5 / SHA-1 -- they're broken)
 *   - AES-128, AES-256 with GCM + CCM (no CBC, no plain ECB)
 *   - HMAC, HKDF
 *   - ECDH, ECDSA on P-256 + P-384
 *   - RSA-2048 verify-only (no key gen)
 *   - TLS 1.3 client; no SSL3/TLS1.0/TLS1.1 legacy
 *   - HMAC_DRBG with platform entropy source
 *
 * What's OUT (consumers re-enable in their own override):
 *   - All deprecated ciphers (DES, 3DES, RC4, ARIA, ChaCha-Poly
 *     stays in for IoT compat? -- off by default; toggle if
 *     your peer requires it)
 *   - X.509 cert generation (we only validate, not sign)
 *   - PKCS#11, PKCS#7
 *   - SSL/TLS server role (clients only on E1M)
 *   - Filesystem-backed key storage (we use OPTIGA Trust M or
 *     in-memory)
 *
 * Override in your own mbedtls_config.h at the app include root
 * if your application needs anything outside this minimum.
 */

#ifndef ALP_MBEDTLS_CONFIG_H_
#define ALP_MBEDTLS_CONFIG_H_

/* ----------------------------------------------------------------- */
/* Platform                                                            */
/* ----------------------------------------------------------------- */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS /* no libc heap on hot path */

/* ----------------------------------------------------------------- */
/* Entropy / RNG                                                       */
/* ----------------------------------------------------------------- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY /* SDK supplies the source */

/* ----------------------------------------------------------------- */
/* Hashes -- modern only                                               */
/* ----------------------------------------------------------------- */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
/* DELIBERATELY OMITTED: MBEDTLS_MD5_C, MBEDTLS_SHA1_C
 * Both are cryptographically broken; we don't ship them. */

/* ----------------------------------------------------------------- */
/* Symmetric ciphers -- AEAD modes only                                */
/* ----------------------------------------------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C
#define MBEDTLS_CIPHER_C
/* DELIBERATELY OMITTED: MBEDTLS_DES_C, MBEDTLS_ARC4_C
 * Plus MBEDTLS_CIPHER_MODE_CBC -- AEAD-only on TLS 1.3 makes CBC
 * unnecessary; apps that need legacy CBC enable in their override. */

/* ----------------------------------------------------------------- */
/* HMAC, HKDF                                                          */
/* ----------------------------------------------------------------- */
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_HKDF_C

/* ----------------------------------------------------------------- */
/* Public-key crypto                                                   */
/* ----------------------------------------------------------------- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* ----------------------------------------------------------------- */
/* TLS -- 1.3 client only                                              */
/* ----------------------------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_3
/* DELIBERATELY OMITTED: TLS1_0 / TLS1_1 / TLS1_2 (the latter could
 * be re-enabled for legacy-server compat -- toggle in override).
 * MBEDTLS_SSL_SRV_C also out -- E1M-class is client only. */

/* ----------------------------------------------------------------- */
/* X.509 -- parse + validate, no generation                            */
/* ----------------------------------------------------------------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ----------------------------------------------------------------- */
/* PSA Crypto -- the front-end <alp/security.h> uses                   */
/* ----------------------------------------------------------------- */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CONFIG
#define MBEDTLS_USE_PSA_CRYPTO

/* ----------------------------------------------------------------- */
/* Misc                                                                */
/* ----------------------------------------------------------------- */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ERROR_C

#endif /* ALP_MBEDTLS_CONFIG_H_ */
