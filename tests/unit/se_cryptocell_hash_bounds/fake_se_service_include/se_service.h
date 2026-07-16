/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only stand-in for hal_alif's <se_service.h>, used ONLY by this
 * test directory to compile the REAL src/backends/security/
 * se_cryptocell.c translation unit on native_sim, where the real header
 * (and the SE hardware it talks to) does not exist.
 *
 * Supplies exactly the symbols se_cryptocell.c references OUTSIDE the
 * CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM guard -- this test
 * leaves that seam undefined, so the AES/SHA/CMAC/CCM/GCM/ChaCha wire
 * paths (and their vendor packet structs) never compile:
 *
 *   - se_service_get_rnd_num() -- referenced unconditionally from
 *     se_random_bytes()'s body.  Defined (as a never-invoked stub) in
 *     src/test_se_hash_bounds.c so the link resolves.
 *   - MBEDTLS_AES_KEY_128 / MBEDTLS_AES_KEY_256 -- referenced
 *     unconditionally (not seam-gated) by se_aead_keybits().  Values
 *     copied from se_cryptocell.c's own inline citations ("128,
 *     services_lib_api.h" / "256, services_lib_api.h"), not invented
 *     here.
 */

#ifndef ALP_TEST_FAKE_SE_SERVICE_H
#define ALP_TEST_FAKE_SE_SERVICE_H

#include <stdint.h>

#define MBEDTLS_AES_KEY_128 128
#define MBEDTLS_AES_KEY_256 256

int se_service_get_rnd_num(uint8_t *out, uint16_t len);

#endif /* ALP_TEST_FAKE_SE_SERVICE_H */
