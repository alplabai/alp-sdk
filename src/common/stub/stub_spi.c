/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI (controller + target) NOSUPPORT stubs -- <alp/peripheral.h>.
 * Split out of the former src/common/stub_backend.c monolith (issue
 * #673); owns every `alp_spi_*` symbol not provided by a vendor
 * backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_SPI)
alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_spi_transceive(alp_spi_t *b, const uint8_t *t, uint8_t *r, size_t l)
{
	(void)b;
	(void)t;
	(void)r;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_spi_write(alp_spi_t *b, const uint8_t *t, size_t l)
{
	(void)b;
	(void)t;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_spi_read(alp_spi_t *b, uint8_t *r, size_t l)
{
	(void)b;
	(void)r;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
void alp_spi_close(alp_spi_t *b)
{
	(void)b;
}
#endif /* !ALP_VENDOR_OVERRIDES_SPI */

/* Unguarded -- same reasoning as alp_i2c_capabilities: no Linux
 * spidev wrapper or plain stub implements it; only the Zephyr registry
 * dispatcher (src/spi_dispatch.c) does (#593). */
const alp_capabilities_t *alp_spi_capabilities(const alp_spi_t *b)
{
	(void)b;
	return NULL;
}

/* SPI target (slave) mode -- Zephyr-only today.  Gated independently
 * of ALP_VENDOR_OVERRIDES_SPI so a vendor wrapper can adopt target
 * mode later without re-implementing the controller-mode surface. */
#if !defined(ALP_VENDOR_OVERRIDES_SPI_TARGET)
alp_spi_target_t *alp_spi_target_open(const alp_spi_target_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_spi_target_transceive(alp_spi_target_t *b,
                                       const uint8_t    *t,
                                       uint8_t          *r,
                                       size_t            l,
                                       size_t           *rl,
                                       uint32_t          to_ms)
{
	(void)b;
	(void)t;
	(void)r;
	(void)l;
	(void)to_ms;
	if (rl != NULL) *rl = 0;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_spi_target_close(alp_spi_target_t *t)
{
	(void)t;
	return ALP_OK; /* nothing was ever opened -- close is a no-op */
}
#endif /* !ALP_VENDOR_OVERRIDES_SPI_TARGET */
