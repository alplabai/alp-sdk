/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Infineon OPTIGA Trust M (SLS32AIA010MLUSON10XTMA2) secure
 * element -- v0.3 thin driver.
 *
 * Trust M's wire protocol is multi-layer: an I2C data-link layer
 * with PRESET / GET frames, an info-pack layer that carries
 * sequence numbers + CRC16, then APDUs at the top.  Implementing
 * the full stack here would duplicate Infineon's Host Library
 * (~5 KLOC).  The right architectural answer is to vendor the
 * upstream library as a Zephyr module + register its PSA driver
 * with MbedTLS so <alp/security.h>'s wrapper picks up hardware
 * acceleration transparently.  That work tracks as v0.3.x.
 *
 * For v0.3 we ship:
 *   - I2C address probe via a 4-byte read of the I2C_STATE register
 *     at 0x82.
 *   - Argument validation for the future product-info and raw-APDU
 *     entry points.
 *
 * The send_apdu / read_product_info paths return NOSUPPORT after
 * validation.  Trust M's APDU transport requires the sequence-numbered
 * info-pack layer; that should come from Infineon's host library rather
 * than a partial in-repo reimplementation.  This driver confirms wiring
 * + I2C connectivity only.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/optiga_trust_m.h"

#define OPTIGA_REG_DATA         0x80u /* Data register (where APDUs flow) */
#define OPTIGA_REG_DATA_REG_LEN 0x81u
#define OPTIGA_REG_I2C_STATE    0x82u

alp_status_t optiga_trust_m_init(optiga_trust_m_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus  = bus;
	ctx->addr = (addr_7bit != 0) ? addr_7bit : OPTIGA_TRUST_M_I2C_ADDR;

	/* Probe by reading the I2C state register.  Trust M ACKs at
	 * its address before OPEN_APPLICATION; if no ACK, NOT_READY tells
	 * the caller the chip isn't populated / mis-strapped. */
	uint8_t      reg      = OPTIGA_REG_I2C_STATE;
	uint8_t      state[4] = { 0 };
	alp_status_t s        = alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, state, sizeof(state));
	if (s != ALP_OK) return ALP_ERR_NOT_READY;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t optiga_trust_m_send_apdu(optiga_trust_m_t *ctx,
                                      const uint8_t    *apdu,
                                      size_t            apdu_len,
                                      uint8_t          *resp,
                                      size_t            resp_cap,
                                      size_t           *resp_len,
                                      uint32_t          timeout_ms)
{
	(void)timeout_ms;
	if (resp_len != NULL) *resp_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (apdu == NULL || apdu_len == 0u || resp == NULL || resp_cap == 0u || resp_len == NULL) {
		return ALP_ERR_INVAL;
	}
	/* Full transport (info-pack sequence + CRC16) lands via Infineon's
	 * host library.  Returning NOSUPPORT here is faithful to that
	 * contract without surfacing fake success. */
	return ALP_ERR_NOSUPPORT;
}

alp_status_t optiga_trust_m_read_product_info(optiga_trust_m_t              *ctx,
                                              optiga_trust_m_product_info_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;
	/* GET_DATA_OBJECT(0xE0C2) needs the full APDU stack; defer for the
	 * same reason as send_apdu. */
	return ALP_ERR_NOSUPPORT;
}

void optiga_trust_m_deinit(optiga_trust_m_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
