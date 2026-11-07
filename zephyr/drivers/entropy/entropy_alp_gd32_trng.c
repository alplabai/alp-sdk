/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G553 supervisor TRNG as a Zephyr entropy driver (#2951).
 *
 * Zephyr 4.4.1's mbedTLS is TF-PSA-Crypto: modules/mbedtls/zephyr_entropy.c
 * implements mbedtls_platform_get_entropy() over entropy_get_entropy() on
 * DT_CHOSEN(zephyr_entropy).  Nothing there calls mbedtls_hardware_poll(), so
 * the way to feed mbedTLS is to be the chosen entropy device -- which also
 * gives the CSPRNG (CONFIG_CSPRNG_ENABLED) a real source.
 *
 * Every request goes through the V2N supervisor singleton
 * (alp_z_v2n_supervisor_acquire(), the same serialised GD32 bridge every other
 * backend uses) and gd32g553_trng_read(), in <= GD32G553_BRIDGE_TRNG_MAX_BYTES
 * pieces, one acquire/release per piece so other bridge users are not starved
 * for the length of a long fill.
 *
 * Thread context only: the bridge is a mutex-guarded SPI transfer.  An ISR
 * caller gets -EWOULDBLOCK; there is no get_entropy_isr.
 */

#define DT_DRV_COMPAT alp_gd32_trng

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>

#include "alp/chips/gd32g553.h"
#include "entropy_alp_gd32_trng_fill.h"
#include "v2n_supervisor.h"

static int bridge_read(void *ctx, uint8_t *buf, size_t len)
{
	ARG_UNUSED(ctx);

	/* docs/gd32-bridge-protocol.md section 3.11: a tripped TRNG self-check
	 * answers exactly one STATUS_IO (ALP_ERR_IO) while the firmware re-seeds;
	 * the next TRNG_READ succeeds.  Retry once on IO before calling it down.
	 * gd32g553_trng_read() itself already rides out STATUS_BUSY. */
	for (int attempt = 0; attempt < 2; attempt++) {
		gd32g553_t *gd = NULL;

		if (alp_z_v2n_supervisor_acquire(&gd) != ALP_OK) {
			return -EIO;
		}
		alp_status_t s = gd32g553_trng_read(gd, buf, len);

		alp_z_v2n_supervisor_release();
		if (s == ALP_OK) {
			return 0;
		}
		if (s != ALP_ERR_IO) {
			break;
		}
	}
	return -EIO;
}

static int entropy_gd32_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	ARG_UNUSED(dev);

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	return gd32_trng_fill(bridge_read, NULL, buffer, length, GD32G553_BRIDGE_TRNG_MAX_BYTES);
}

/* The bridge handshake is lazy (first acquire), so init has nothing to do and
 * the device is always ready; a missing GD32 surfaces as -EIO per request. */
static int entropy_gd32_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static DEVICE_API(entropy, entropy_gd32_api) = {
	.get_entropy = entropy_gd32_get_entropy,
};

DEVICE_DT_INST_DEFINE(0,
                      entropy_gd32_init,
                      NULL,
                      NULL,
                      NULL,
                      POST_KERNEL,
                      CONFIG_ENTROPY_INIT_PRIORITY,
                      &entropy_gd32_api);
