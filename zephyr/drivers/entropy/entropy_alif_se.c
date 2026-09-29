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
 * One SE request returns at most MAX_RND_LENGTH bytes (hal_alif's
 * get_rnd_svc_t carries a fixed inline response buffer of that size, and
 * se_service_get_rnd_num() does not bound `length` before copying out of it),
 * so longer requests are chunked.
 *
 * Thread context only. An SE request takes hal_alif's service mutex and blocks
 * on a semaphore until the SE answers over the MHUv2 mailbox. Zephyr can still
 * route an ISR here -- with ENTROPY_HAS_DRIVER the non-CS sys_rand_get()
 * defaults to ENTROPY_DEVICE_RANDOM_GENERATOR, which calls
 * entropy_get_entropy() -- so an ISR caller gets -EWOULDBLOCK instead of a
 * mutex from interrupt context. There is no get_entropy_isr.
 */

#define DT_DRV_COMPAT alif_se_trng

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <se_service.h>
#include <services_lib_protocol.h>

LOG_MODULE_REGISTER(entropy_alif_se, CONFIG_ENTROPY_LOG_LEVEL);

/* The SE requests ride the se_service channel; without it every call would
 * dereference hal_alif's NULL mailbox handles. */
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(se_service)),
             "alif,se-trng needs the se_service node (and its mailboxes) enabled");

#define ENTROPY_ALIF_SE_MBOX(prop) DEVICE_DT_GET(DT_PHANDLE(DT_NODELABEL(se_service), prop))

static int entropy_alif_se_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	/* hal_alif's se_service_mhuv2_nodes_init() leaves its send/recv handles
	 * NULL when a mailbox is missing, and the first request would then fault
	 * in ipm_send(NULL, ...). Refuse readiness here instead. */
	if (!device_is_ready(ENTROPY_ALIF_SE_MBOX(mhuv2_send_node)) ||
	    !device_is_ready(ENTROPY_ALIF_SE_MBOX(mhuv2_recv_node))) {
		LOG_ERR("SE-service mailboxes not ready");
		return -ENODEV;
	}
	return 0;
}

static int entropy_alif_se_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	uint16_t done = 0U;

	ARG_UNUSED(dev);

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	while (done < length) {
		uint16_t remaining = length - done;
		uint16_t chunk     = (remaining > MAX_RND_LENGTH) ? MAX_RND_LENGTH : remaining;
		int      rc        = se_service_get_rnd_num(buffer + done, chunk);

		if (rc != 0) {
			/* rc is a transport errno (e.g. -EAGAIN on a mailbox timeout)
			 * or the SE's own positive resp_error_code. */
			LOG_ERR("SE GET_RND rc=%d", rc);
			return (rc < 0) ? rc : -EIO;
		}
		done += chunk;
	}

	return 0;
}

static DEVICE_API(entropy, entropy_alif_se_api) = {
	.get_entropy = entropy_alif_se_get_entropy,
};

DEVICE_DT_INST_DEFINE(0,
                      entropy_alif_se_init,
                      NULL,
                      NULL,
                      NULL,
                      POST_KERNEL,
                      CONFIG_ENTROPY_INIT_PRIORITY,
                      &entropy_alif_se_api);
