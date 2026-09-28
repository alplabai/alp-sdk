/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble Secure-Enclave TRNG as a Zephyr entropy driver (#2192).
 *
 * ADR 0017 Tier 1.5: thin glue over the vendor HAL. The TRNG lives inside the
 * Secure Enclave, so there is no register block to drive; every request goes
 * to the SE through hal_alif's public se_service_get_rnd_num()
 * (SERVICE_CRYPTOCELL_GET_RND), the same call src/backends/security/
 * se_cryptocell.c already rides for alp_crypto_random().
 *
 * One SE request returns at most MAX_RND_LENGTH (256) bytes -- hal_alif's
 * get_rnd_svc_t carries a fixed inline response buffer of that size -- so
 * longer requests are chunked.
 *
 * No ISR variant: an SE request blocks on a semaphore until the SE answers
 * over the MHUv2 mailbox, which cannot be done from interrupt context.
 * entropy_get_entropy_isr() on this device therefore reports -ENOSYS, and
 * Zephyr's CSPRNG only ever calls the thread-context entry point.
 */

#define DT_DRV_COMPAT alif_se_trng

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>

#include <se_service.h>

/* hal_alif get_rnd_svc_t.resp_rnd[MAX_RND_LENGTH]; se_cryptocell.c's
 * SE_RND_MAX_CHUNK is the same ceiling. */
#define ENTROPY_ALIF_SE_MAX_CHUNK 256U

static int entropy_alif_se_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	uint16_t done = 0U;

	ARG_UNUSED(dev);

	while (done < length) {
		uint16_t remaining = length - done;
		uint16_t chunk =
		    (remaining > ENTROPY_ALIF_SE_MAX_CHUNK) ? ENTROPY_ALIF_SE_MAX_CHUNK : remaining;

		if (se_service_get_rnd_num(buffer + done, chunk) != 0) {
			return -EIO;
		}
		done += chunk;
	}

	return 0;
}

static DEVICE_API(entropy, entropy_alif_se_api) = {
	.get_entropy = entropy_alif_se_get_entropy,
};

DEVICE_DT_INST_DEFINE(0,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      POST_KERNEL,
                      CONFIG_ENTROPY_INIT_PRIORITY,
                      &entropy_alif_se_api);
