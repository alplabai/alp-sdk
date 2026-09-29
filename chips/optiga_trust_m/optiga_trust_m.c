/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Infineon OPTIGA Trust M (SLS32AIA010MLUSON10XTMA2) secure element.
 *
 * Trust M's wire protocol is multi-layer: an I2C data-link layer with
 * sequence-numbered, CRC16-protected frames, a transport layer that
 * chains APDUs across frames, then APDUs at the top.  This driver does
 * not reimplement that stack: it runs Infineon's host library
 * (vendors/optiga-trust-m, release-v5.8.3, MIT) through a PAL written on
 * the portable alp surface (vendors/optiga-trust-m/pal_alp/pal_alp.c), so
 * the same code path serves the A55 and an MCU core (#1164).
 *
 *   - init() probes I2C_STATE (0x82) only.  No application session is
 *     opened, so a caller that only wants "is it fitted" pays one read.
 *   - read_product_info() opens the Trust M application through the
 *     library's util layer and reads the Coprocessor UID (0xE0C2).
 *   - send_apdu() runs a raw session on the library's comms layer: the
 *     caller owns the APDU sequence, including OpenApplication.
 *
 * The two sessions share the library's single IFX I2C instance, so they
 * are exclusive: switching mode closes the other session first.
 *
 * Shielded Connection (link encryption) is off: it needs a platform
 * binding secret written into the chip, an irreversible provisioning
 * step that is not designed yet.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/optiga_trust_m.h"
#include "optiga_comms.h"
#include "optiga_util.h"
#include "pal_alp.h"

#define OPTIGA_REG_I2C_STATE 0x82u

/* Trust M NACKs the first access while it wakes from its idle sleep and
 * ACKs the next one: bench, E1M-V2M103 2026W38-0001, where a single probe
 * reported a fitted part as absent.  Upstream's physical layer polls on
 * NACK too (PL_POLLING_INVERVAL_US).  10 x 1 ms is well past the one
 * retry the bench needed. */
#define OPTIGA_PROBE_TRIES    10u
#define OPTIGA_PROBE_RETRY_MS 1u

/* OpenApplication runs a soft reset plus link sync; bench: ~60 ms. */
#define OPTIGA_OPEN_TIMEOUT_MS 2000u
#define OPTIGA_READ_TIMEOUT_MS 1000u

#define OPTIGA_OID_COPROCESSOR_UID 0xE0C2u

enum { SESSION_NONE = 0, SESSION_UTIL, SESSION_RAW };

static void op_done(void *context, optiga_lib_status_t status)
{
	((optiga_trust_m_t *)context)->op_status = status;
}

/* Drive the library until the pending operation completes: its timed
 * callbacks run from here (pal_alp.c is threadless). */
static alp_status_t op_wait(optiga_trust_m_t *ctx, uint32_t timeout_ms)
{
	uint64_t deadline = alp_uptime_ms() + timeout_ms;
	while (ctx->op_status == OPTIGA_LIB_BUSY) {
		if (alp_optiga_pal_poll()) continue;
		if (alp_uptime_ms() >= deadline) return ALP_ERR_TIMEOUT;
		alp_delay_ms(1);
	}
	return ctx->op_status == OPTIGA_LIB_SUCCESS ? ALP_OK : ALP_ERR_IO;
}

static void session_close(optiga_trust_m_t *ctx)
{
	if (ctx->session == SESSION_UTIL) {
		ctx->op_status = OPTIGA_LIB_BUSY;
		if (optiga_util_close_application(ctx->util, 0) == OPTIGA_LIB_SUCCESS) {
			(void)op_wait(ctx, OPTIGA_OPEN_TIMEOUT_MS);
		}
	} else if (ctx->session == SESSION_RAW) {
		ctx->op_status = OPTIGA_LIB_BUSY;
		if (optiga_comms_close(ctx->comms) == OPTIGA_LIB_SUCCESS) {
			(void)op_wait(ctx, OPTIGA_OPEN_TIMEOUT_MS);
		}
	}
	ctx->session = SESSION_NONE;
}

static alp_status_t session_open(optiga_trust_m_t *ctx, uint8_t kind)
{
	if (ctx->session == kind) return ALP_OK;
	session_close(ctx);
	alp_optiga_pal_bind(ctx->bus, ctx->addr);

	optiga_lib_status_t rc;
	ctx->op_status = OPTIGA_LIB_BUSY;
	if (kind == SESSION_UTIL) {
		if (ctx->util == NULL) ctx->util = optiga_util_create(OPTIGA_INSTANCE_ID_0, op_done, ctx);
		if (ctx->util == NULL) return ALP_ERR_NOMEM;
		rc = optiga_util_open_application(ctx->util, 0);
	} else {
		if (ctx->comms == NULL) ctx->comms = optiga_comms_create(op_done, ctx);
		if (ctx->comms == NULL) return ALP_ERR_NOMEM;
		rc = optiga_comms_open(ctx->comms);
	}
	if (rc != OPTIGA_LIB_SUCCESS) return ALP_ERR_IO;
	alp_status_t s = op_wait(ctx, OPTIGA_OPEN_TIMEOUT_MS);
	if (s == ALP_OK) ctx->session = kind;
	return s;
}

alp_status_t optiga_trust_m_init(optiga_trust_m_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	/* addr_7bit == 0 is the documented "fall back to the default
	 * provisioned address" sentinel; the address is otherwise
	 * provisioning-defined (no fixed strap range this driver can
	 * assert), so only the generic 7-bit domain bound applies here. */
	if (addr_7bit > 0x7Fu) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus  = bus;
	ctx->addr = (addr_7bit != 0) ? addr_7bit : OPTIGA_TRUST_M_I2C_ADDR;

	/* Probe by reading the I2C state register.  Trust M ACKs at
	 * its address before OPEN_APPLICATION; if it still does not ACK
	 * after the wake retries, NOT_READY tells the caller the chip isn't
	 * populated / mis-strapped. */
	uint8_t      reg      = OPTIGA_REG_I2C_STATE;
	uint8_t      state[4] = { 0 };
	alp_status_t s        = ALP_ERR_NOT_READY;
	for (unsigned i = 0; i < OPTIGA_PROBE_TRIES && s != ALP_OK; i++) {
		if (i != 0u) alp_delay_ms(OPTIGA_PROBE_RETRY_MS);
		s = alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, state, sizeof(state));
	}
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
	if (resp_len != NULL) *resp_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (apdu == NULL || apdu_len == 0u || resp == NULL || resp_cap == 0u || resp_len == NULL) {
		return ALP_ERR_INVAL;
	}
	/* The comms layer counts in uint16_t. */
	if (apdu_len > UINT16_MAX) return ALP_ERR_INVAL;
	uint16_t rx_len = resp_cap > UINT16_MAX ? UINT16_MAX : (uint16_t)resp_cap;

	alp_status_t s = session_open(ctx, SESSION_RAW);
	if (s != ALP_OK) return s;
	ctx->op_status = OPTIGA_LIB_BUSY;
	if (optiga_comms_transceive(ctx->comms, apdu, (uint16_t)apdu_len, resp, &rx_len) !=
	    OPTIGA_LIB_SUCCESS) {
		return ALP_ERR_IO;
	}
	s = op_wait(ctx, timeout_ms);
	if (s == ALP_OK) *resp_len = rx_len;
	return s;
}

alp_status_t optiga_trust_m_read_product_info(optiga_trust_m_t              *ctx,
                                              optiga_trust_m_product_info_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;

	alp_status_t s = session_open(ctx, SESSION_UTIL);
	if (s != ALP_OK) return s;
	uint8_t  uid[sizeof(*out)];
	uint16_t len   = sizeof(uid);
	ctx->op_status = OPTIGA_LIB_BUSY;
	if (optiga_util_read_data(ctx->util, OPTIGA_OID_COPROCESSOR_UID, 0, uid, &len) !=
	    OPTIGA_LIB_SUCCESS) {
		return ALP_ERR_IO;
	}
	s = op_wait(ctx, OPTIGA_READ_TIMEOUT_MS);
	if (s != ALP_OK) return s;
	/* The object is exactly the 27-byte UID laid out as the struct. */
	if (len != sizeof(uid)) return ALP_ERR_IO;
	memcpy(out, uid, sizeof(uid));
	return ALP_OK;
}

void optiga_trust_m_deinit(optiga_trust_m_t *ctx)
{
	if (ctx == NULL) return;
	if (ctx->initialised) session_close(ctx);
	if (ctx->util != NULL) (void)optiga_util_destroy(ctx->util);
	if (ctx->comms != NULL) optiga_comms_destroy(ctx->comms);
	ctx->util        = NULL;
	ctx->comms       = NULL;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
