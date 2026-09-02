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
 * the full stack here would duplicate Infineon's Host Library, so
 * this driver deliberately does not.
 *
 * That library is github.com/Infineon/optiga-trust-m, MIT, and it
 * does ship a Zephyr PAL (extras/pal/zephyr/).  What it does NOT
 * ship is a Zephyr *module*: at release-v5.8.1 the tree has no
 * zephyr/module.yml and no library-level CMakeLists, so a plain
 * west project pin would be inert for exactly the reason
 * vendors/u8g2/README.md describes.  Consuming it therefore means
 * vendored source (src/ + include/ are 19,777 lines) plus module
 * glue alp-sdk writes itself -- not a one-line manifest entry.
 * Two hardware facts gate that work as much as the vendoring does:
 * the PAL resolves its bus through DT_ALIAS(optiga_i2c), which no
 * alp-sdk board declares, and its optional reset support wants
 * DT_ALIAS(optiga_reset) as a SoC GPIO -- but on V2N/V2M SE_RST is
 * not wired to the SoC at all.  It hangs off the GD32 supervisor
 * (PC13), reachable only via gd32g553_se_reset().  See #1164.
 *
 * #1164 also asks whether a minimal in-tree APDU implementation --
 * just the handful of commands this SDK needs -- beats vendoring the
 * host library.  It doesn't, and not by a small margin.  Upstream's own
 * comms stack sizes the transport this driver would have to
 * hand-derive from the wire spec instead of reusing:
 * ifx_i2c_data_link_layer.c (608 lines) implements an 11-state
 * retry/resend/ack/nack machine (DL_STATE_TX/RX/ACK/RESEND/NACK/ERROR/
 * RX_DF/RX_CF/...) plus a CRC16 over every frame, and
 * ifx_i2c_transport_layer.c (479 lines) chains APDUs across frames --
 * both required before OpenApplication/CloseApplication (APDU commands
 * 0x70/0x71, src/cmd/optiga_cmd.c:31,33) or any other command APDU can
 * run.  That is 1,087 lines at minimum -- not "a handful of commands"
 * -- and it excludes ifx_i2c_presentation_layer.c (1086 lines): that
 * file's entire body is guarded #ifdef OPTIGA_COMMS_SHIELDED_CONNECTION
 * (verified against a fresh clone of release-v5.8.1), i.e. the OPTIONAL
 * Shielded Connection encryption layer, not part of the required path.
 * Vendoring the whole src/comms/ifx_i2c/ directory instead -- the three
 * files above plus ifx_i2c_physical_layer.c (720), ifx_i2c.c (315) and
 * ifx_i2c_config.c (140) -- totals 3,348 lines, already tested against
 * real silicon.  Hand-deriving even the 1,087-line minimum is written
 * blind against a security element with no silicon in reach to run it
 * against even once.  A wrong CRC polynomial, a wrong sequence-toggle
 * bit, or a mishandled retry transition is silent until it corrupts a
 * real command to a real key-storage part, and nothing in this repo can
 * catch that without a bench.  That's the real cost of "minimal
 * in-tree," and it's not desk-safe to ship as anything other than
 * NOSUPPORT without a way to verify it.
 *
 * This is this driver's own call, made on the cost above -- not
 * something #1164's comment thread (2026-08-30) already decided.  That
 * thread says the opposite on feasibility ("writing the APDU transport
 * layer itself is desk work") and leaves speculative-vs-wait open
 * ("Worth deciding whether to write it speculatively or wait, since an
 * unverified security-chip driver is not much better than a stub").
 *
 * Anything that changes send_apdu()/read_product_info() away from
 * NOSUPPORT must move every one of these NOSUPPORT sites in the same
 * change, or the chips suite goes red on the first run:
 * tests/zephyr/chips/src/test_security.c:88,104-106;
 * examples/v2n/v2n-secure-element-sign/src/main.c:52,73;
 * examples/aen/aen-secure-element-sign/src/main.c:103,123
 * (both example READMEs document the NOSUPPORT line as the PASS case);
 * docs/tutorials/06-secure-element-sign.md:23-30 documents the same
 * contract for a reader, not just a test.
 *
 * For v0.3 we ship:
 *   - I2C address probe via a 4-byte read of the I2C_STATE register
 *     at 0x82.  The register numbers below and that 4-byte length
 *     match upstream's own physical layer
 *     (src/comms/ifx_i2c/ifx_i2c_physical_layer.c: PL_REG_DATA 0x80,
 *     PL_REG_DATA_REG_LEN 0x81, PL_REG_I2C_STATE 0x82,
 *     PL_REG_LEN_I2C_STATE 4U), so the probe stays correct when the
 *     real transport lands on top of it.
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
	/* addr_7bit == 0 is the documented "fall back to the default
	 * provisioned address" sentinel; the address is otherwise
	 * provisioning-defined (no fixed strap range this driver can
	 * assert), so only the generic 7-bit domain bound applies here. */
	if (addr_7bit > 0x7Fu) return ALP_ERR_INVAL;
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
