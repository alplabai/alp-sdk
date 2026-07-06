/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-se-crypto -- exercise the portable <alp/security.h> surface on the
 * E1M-AEN801 (Ensemble E8, M55-HE) when it is backed by the Secure
 * Enclave's CryptoCell HARDWARE crypto.
 *
 * This is the companion to aen-se-service-query: that app proves the
 * READ-ONLY SE transport binds and answers (heartbeat + LCS + 8-byte TRNG);
 * THIS app drives the *compute* surface that rides the same MHUv2 -> SE
 * mailbox -- a known-answer SHA-256, an AES-128-GCM encrypt/decrypt
 * round-trip, and a true-random pull -- all through the vendor-clean
 * alp_hash_* / alp_aead_* / alp_random_bytes API.  Nothing here names the
 * SE: the SE-CryptoCell backend (src/backends/security/se_cryptocell.c)
 * registers at silicon_ref="alif:ensemble:e8" priority 110 and wins
 * selection over the portable MbedTLS-PSA backend on this part, so the work
 * lands on dedicated silicon transparently.
 *
 * WHAT RUNS WHERE (current bench reality):
 *   - alp_random_bytes  -> SE TRNG, LIVE today (public
 *                          se_service_get_rnd_num under the hood).
 *   - alp_hash_* / alp_aead_*  -> SE CryptoCell SHA / AES-GCM.  These ride the
 *                          generic SE send-request seam
 *                          (CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM),
 *                          which DEFAULTS ON, so by default the SE CryptoCell
 *                          computes them on-silicon (bench-validated).  For an
 *                          alg the SE declines (SHA-384/512, over-ceiling
 *                          input) the dispatcher falls through to the
 *                          MbedTLS-PSA backend on the M55.  Either way the API
 *                          contract holds and the RESULT line reports which
 *                          surfaces answered.
 *
 * PASS gate: SHA-256("abc") matches the NIST known-answer vector, the
 * AES-128-GCM ciphertext decrypts back to the plaintext with a valid tag,
 * and the TRNG fill returns ALP_OK with not-all-zero bytes.
 *
 * Bench flow: RAM-run + RAM-console (the app UART is not on USB on this
 * bench; read 'ram_console_buf' over SWD).  Every alp_* call bounds its
 * wait inside the backend, so the app never hangs.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/security.h>

/*
 * NIST FIPS 180-4 known-answer vector: SHA-256("abc").
 *   ba7816bf 8f01cfea 414140de 5dae2223
 *   b00361a3 96177a9c b410ff61 f20015ad
 */
static const uint8_t k_sha256_abc[32] = {
	0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
	0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static void hexdump(const char *label, const uint8_t *p, size_t n)
{
	printk("        %s = ", label);
	for (size_t i = 0u; i < n; i++) {
		printk("%02x", p[i]);
	}
	printk("\n");
}

/* SHA-256("abc") through the portable hash surface. */
static bool test_sha256_abc(void)
{
	alp_hash_t *h = alp_hash_open(ALP_HASH_SHA256);
	if (h == NULL) {
		printk("sha256               : open FAILED (no backend)\n");
		return false;
	}

	uint8_t      digest[32] = { 0 };
	size_t       dlen       = 0u;
	alp_status_t s          = alp_hash_update(h, (const uint8_t *)"abc", 3u);
	if (s == ALP_OK) {
		s = alp_hash_finish(h, digest, sizeof(digest), &dlen);
		/* Contract nuance (<alp/security.h>): finish implicitly closes
		 * the context ON SUCCESS ONLY.  A failed finish leaves the
		 * handle open, so it must be closed explicitly or the backend
		 * slot leaks and later opens start failing. */
		if (s != ALP_OK) {
			alp_hash_close(h);
		}
	} else {
		/* Update failed before finish ever ran: release the handle
		 * (close is the "abandon without finalising" path). */
		alp_hash_close(h);
	}

	const bool ok = (s == ALP_OK) && (dlen == sizeof(digest)) &&
	                (memcmp(digest, k_sha256_abc, sizeof(digest)) == 0);
	printk("sha256(\"abc\")        : s=%d len=%zu %s\n", (int)s, dlen, ok ? "MATCH" : "MISMATCH");
	if (!ok) {
		hexdump("got        ", digest, sizeof(digest));
		hexdump("want       ", k_sha256_abc, sizeof(k_sha256_abc));
	}
	return ok;
}

/* AES-128-GCM encrypt then decrypt; the recovered plaintext must match. */
static bool test_aes128_gcm_roundtrip(void)
{
	/* Arbitrary but fixed key/IV so the run is deterministic. */
	const uint8_t key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		                      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
	const uint8_t iv[12]  = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b
	};
	const uint8_t aad[8]    = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7 };
	const uint8_t plain[19] = "alp-sdk SE crypto!";

	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));
	if (a == NULL) {
		printk("aes128gcm            : open FAILED (no backend)\n");
		return false;
	}

	uint8_t      cipher[sizeof(plain)] = { 0 };
	uint8_t      tag[16]               = { 0 };
	alp_status_t se                    = alp_aead_encrypt(
	    a, iv, sizeof(iv), aad, sizeof(aad), plain, sizeof(plain), cipher, tag, sizeof(tag));
	if (se != ALP_OK) {
		printk("aes128gcm encrypt    : s=%d FAILED\n", (int)se);
		alp_aead_close(a);
		return false;
	}

	uint8_t      recovered[sizeof(plain)] = { 0 };
	alp_status_t sd                       = alp_aead_decrypt(
	    a, iv, sizeof(iv), aad, sizeof(aad), cipher, sizeof(cipher), tag, sizeof(tag), recovered);
	alp_aead_close(a);

	const bool ok = (sd == ALP_OK) && (memcmp(recovered, plain, sizeof(plain)) == 0);
	printk(
	    "aes128gcm round-trip : enc=%d dec=%d %s\n", (int)se, (int)sd, ok ? "MATCH" : "MISMATCH");
	if (ok) {
		hexdump("tag        ", tag, sizeof(tag));
	}
	return ok;
}

/* Pull 16 TRNG bytes; PASS requires ALP_OK and a not-all-zero result. */
static bool test_trng(void)
{
	uint8_t      rnd[16] = { 0 };
	alp_status_t s       = alp_random_bytes(rnd, sizeof(rnd));

	bool nonzero = false;
	for (size_t i = 0u; i < sizeof(rnd); i++) {
		if (rnd[i] != 0u) {
			nonzero = true;
			break;
		}
	}

	const bool ok = (s == ALP_OK) && nonzero;
	printk("trng(16)             : s=%d %s\n", (int)s, ok ? "OK" : "FAIL");
	if (s == ALP_OK) {
		hexdump("rnd        ", rnd, sizeof(rnd));
	}
	return ok;
}

int main(void)
{
	printk("\n=== aen-se-crypto (SE CryptoCell via <alp/security.h>) ===\n");

	const bool sha_ok  = test_sha256_abc();
	const bool gcm_ok  = test_aes128_gcm_roundtrip();
	const bool trng_ok = test_trng();

	if (sha_ok && gcm_ok && trng_ok) {
		printk("RESULT PASS: SHA-256(\"abc\") known-answer MATCH + AES-128-GCM round-trip "
		       "MATCH + TRNG OK -- the portable <alp/security.h> surface answered through "
		       "the selected crypto backend (SE CryptoCell on E8 when the send seam is "
		       "wired; MbedTLS-PSA fallthrough otherwise)\n");
	} else {
		printk("RESULT FAIL: sha=%d gcm=%d trng=%d -- see the per-test lines above (a "
		       "MISMATCH means the backend computed a wrong value; a 'no backend' open "
		       "FAIL means no security backend linked / selected)\n",
		       (int)sha_ok,
		       (int)gcm_ok,
		       (int)trng_ok);
	}

	return 0;
}
