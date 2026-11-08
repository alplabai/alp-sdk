/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only stand-in for hal_alif's <se_service.h>, used ONLY by this test
 * directory to compile the REAL src/backends/security/se_cryptocell.c with
 * its SEND_SEAM wire paths on.  Carries just the packet fields and ids that
 * file touches; values mirror its own inline citations.
 */
#ifndef ALP_TEST_FAKE_SE_SERVICE_H
#define ALP_TEST_FAKE_SE_SERVICE_H

#include <stdint.h>

#define MBEDTLS_AES_KEY_128 128
#define MBEDTLS_AES_KEY_256 256

#define SERVICE_CRYPTOCELL_MBEDTLS_SHA             420
#define SERVICE_CRYPTOCELL_MBEDTLS_CHACHAPOLY_CRYPT 408
#define SERVICE_CRYPTOCELL_MBEDTLS_CCM_GCM         422
#define MBEDTLS_HASH_SHA256                        2
#define MBEDTLS_GCM_ENCRYPT_AND_TAG                4
#define MBEDTLS_GCM_AUTH_DECRYPT                   6
#define MBEDTLS_CHACHAPOLY_ENCRYPT_AND_TAG         0
#define MBEDTLS_CHACHAPOLY_AUTH_DECRYPT            1

typedef struct {
	uint16_t hdr_service_id;
} se_hdr_t;

typedef struct {
	se_hdr_t header;
	uint32_t send_sha_type, send_data_addr, send_data_length, send_shasum_addr;
	uint32_t resp_error_code;
} mbedtls_sha_single_svc_t;

typedef struct {
	se_hdr_t header;
	uint32_t send_context_addr, send_crypt_type, send_length, send_nonce_addr, send_aad_addr,
	    send_aad_len, send_tag_addr, send_input_addr, send_output_addr;
	uint32_t resp_error_code;
} mbedtls_chachapoly_crypt_svc_t;

typedef struct {
	se_hdr_t header;
	uint32_t send_crypt_type, send_key_addr, send_key_bits, send_length, send_iv_addr,
	    send_iv_length, send_add_addr, send_add_length, send_input_addr, send_output_addr,
	    send_tag_addr, send_tag_length;
	uint32_t resp_error_code;
} mbedtls_ccm_gcm_svc_t;

int se_service_get_rnd_num(uint8_t *out, uint16_t len);
int se_service_send_request(uint32_t *packet, uint32_t size);

#endif /* ALP_TEST_FAKE_SE_SERVICE_H */
