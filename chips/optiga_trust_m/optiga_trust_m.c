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
#include "ifx_i2c_config.h"
#include "optiga_comms.h"
#include "optiga_util.h"
#include "pal_alp.h"

#define OPTIGA_REG_I2C_STATE 0x82u

/* Trust M NACKs accesses while it wakes from its idle sleep.  After a
 * few seconds idle the wake outlasts 10 x 1 ms (bench, E1M-V2M103
 * 2026W38-0001: every run after a 2 s pause failed a 10-try probe), so
 * the probe uses the host library's own NACK-polling budget:
 * PL_POLLING_MAX_CNT tries at PL_POLLING_INVERVAL_US (ifx_i2c_config.h). */
#define OPTIGA_PROBE_TRIES    PL_POLLING_MAX_CNT
#define OPTIGA_PROBE_RETRY_MS (PL_POLLING_INVERVAL_US / 1000u)

/* After a longer idle (>~10 s, alplabai/alp-sdk#2507) the part stops
 * answering at all and only a hardware reset revives it.  The library's
 * own reset sequence (ifx_i2c.c, IFX_I2C_STATE_RESET_PIN_LOW / _HIGH)
 * holds RESET low RESET_LOW_TIME_MSEC and then waits STARTUP_TIME_MSEC
 * before the first access.  Despite the name both constants are
 * microseconds (they feed pal_os_event_register_callback_oneshot's
 * time_us): 2 ms low, 12 ms start-up.  Round up to whole milliseconds. */
#define OPTIGA_RESET_LOW_MS (RESET_LOW_TIME_MSEC / 1000u)
#define OPTIGA_STARTUP_MS   ((STARTUP_TIME_MSEC + 999u) / 1000u)

/* OpenApplication runs a soft reset plus link sync; bench: ~60 ms. */
#define OPTIGA_OPEN_TIMEOUT_MS 2000u
#define OPTIGA_READ_TIMEOUT_MS 1000u

#define OPTIGA_OID_COPROCESSOR_UID 0xE0C2u

enum { SESSION_NONE = 0, SESSION_UTIL, SESSION_RAW };

/* What a timed-out library op was.  The library still holds the op, its
 * buffers and ctx as the callback context, so ctx->op_pending records it
 * until op_drain() sees it finish.  The first two values match the
 * session kind an open would have produced. */
enum { PEND_NONE = 0, PEND_OPEN_UTIL = SESSION_UTIL, PEND_OPEN_RAW = SESSION_RAW, PEND_OTHER };

static void op_done(void *context, optiga_lib_status_t status)
{
	((optiga_trust_m_t *)context)->op_status = status;
}

/* Drive the library until the pending operation completes: its timed
 * callbacks run from here (pal_alp.c is threadless).  On timeout the op
 * is still in flight and is recorded as @p pend. */
static alp_status_t op_wait(optiga_trust_m_t *ctx, uint32_t timeout_ms, uint8_t pend)
{
	uint64_t deadline = alp_uptime_ms() + timeout_ms;
	while (ctx->op_status == OPTIGA_LIB_BUSY) {
		if (alp_optiga_pal_poll()) continue;
		if (alp_uptime_ms() >= deadline) {
			ctx->op_pending = pend;
			return ALP_ERR_TIMEOUT;
		}
		alp_delay_ms(1);
	}
	return ctx->op_status == OPTIGA_LIB_SUCCESS ? ALP_OK : ALP_ERR_IO;
}

/* Let a timed-out op finish before anything else touches the library.
 * ALP_ERR_BUSY while it still runs.  A finished open that succeeded left
 * a session behind: adopt it so the caller closes or reuses it. */
static alp_status_t op_drain(optiga_trust_m_t *ctx)
{
	uint8_t pend = ctx->op_pending;
	if (pend == PEND_NONE) return ALP_OK;
	if (op_wait(ctx, OPTIGA_OPEN_TIMEOUT_MS, pend) == ALP_ERR_TIMEOUT) return ALP_ERR_BUSY;
	if (ctx->op_status == OPTIGA_LIB_SUCCESS && pend != PEND_OTHER) ctx->session = pend;
	ctx->op_pending = PEND_NONE;
	return ALP_OK;
}

static void session_close(optiga_trust_m_t *ctx)
{
	if (ctx->session == SESSION_UTIL) {
		ctx->op_status = OPTIGA_LIB_BUSY;
		if (optiga_util_close_application(ctx->util, 0) == OPTIGA_LIB_SUCCESS) {
			(void)op_wait(ctx, OPTIGA_OPEN_TIMEOUT_MS, PEND_OTHER);
		}
	} else if (ctx->session == SESSION_RAW) {
		ctx->op_status = OPTIGA_LIB_BUSY;
		if (optiga_comms_close(ctx->comms) == OPTIGA_LIB_SUCCESS) {
			(void)op_wait(ctx, OPTIGA_OPEN_TIMEOUT_MS, PEND_OTHER);
		}
	}
	ctx->session = SESSION_NONE;
}

static alp_status_t session_open_once(optiga_trust_m_t *ctx, uint8_t kind)
{
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
	/* The library refused the call because its instance is busy: nothing
	 * ran on the bus, so this is not a wedged part. */
	if (rc == OPTIGA_UTIL_ERROR_INSTANCE_IN_USE) return ALP_ERR_BUSY;
	if (rc != OPTIGA_LIB_SUCCESS) return ALP_ERR_IO;
	alp_status_t s = op_wait(ctx, OPTIGA_OPEN_TIMEOUT_MS, kind);
	if (s == ALP_OK) ctx->session = kind;
	return s;
}

static alp_status_t hw_reset(optiga_trust_m_reset_fn_t reset, void *user);

/* Open a session.  A part that idled out after init NACKs the open (#2517):
 * if init was given a reset hook, pulse RESET once and open again.  Only
 * an I/O failure of a completed op is retried, and only once.  A timeout
 * is not: the library op is still in flight, so every entry point drains
 * it first (ALP_ERR_BUSY while it still runs) and never resets over it.
 * An already-open session is not re-opened, so an idle-out between two
 * calls on the same session still surfaces to the caller: a reset would
 * drop the caller's APDU state (OpenApplication, session context), which
 * it must redo. */
static alp_status_t session_open(optiga_trust_m_t *ctx, uint8_t kind)
{
	alp_status_t s = op_drain(ctx);
	if (s != ALP_OK) return s;
	if (ctx->session == kind) return ALP_OK;
	session_close(ctx);
	if (ctx->op_pending != PEND_NONE) return ALP_ERR_BUSY;
	s = session_open_once(ctx, kind);
	if (s == ALP_ERR_IO && ctx->reset != NULL && hw_reset(ctx->reset, ctx->reset_user) == ALP_OK) {
		s = session_open_once(ctx, kind);
	}
	return s;
}

/* Probe by reading the I2C state register.  Trust M ACKs at its address
 * before OPEN_APPLICATION.  Register address and data go in two
 * transactions with a STOP between them, as upstream's physical layer
 * does: the part NACKs a repeated-start write-read (bench, E1M-V2M103
 * 2026W38-0001). */
static alp_status_t probe_i2c_state(optiga_trust_m_t *ctx)
{
	uint8_t      reg      = OPTIGA_REG_I2C_STATE;
	uint8_t      state[4] = { 0 };
	alp_status_t s        = ALP_ERR_NOT_READY;
	for (unsigned i = 0; i < OPTIGA_PROBE_TRIES && s != ALP_OK; i++) {
		if (i != 0u) alp_delay_ms(OPTIGA_PROBE_RETRY_MS);
		s = alp_i2c_write(ctx->bus, ctx->addr, &reg, 1);
		if (s != ALP_OK) continue;
		/* The part needs PL_GUARD_TIME_INTERVAL_US (50 us) between the
		 * register write and the read; alp_delay_ms is the finest
		 * portable wait. */
		alp_delay_ms(1);
		s = alp_i2c_read(ctx->bus, ctx->addr, state, sizeof(state));
	}
	return s;
}

/* Pulse RESET through the caller's hook: low for the library's reset
 * time, then release and wait the library's start-up time.  RESET is
 * always released, even if asserting it failed. */
static alp_status_t hw_reset(optiga_trust_m_reset_fn_t reset, void *user)
{
	alp_status_t a = reset(user, true);
	if (a == ALP_OK) alp_delay_ms(OPTIGA_RESET_LOW_MS);
	alp_status_t r = reset(user, false);
	if (a != ALP_OK) return a;
	if (r != ALP_OK) return r;
	alp_delay_ms(OPTIGA_STARTUP_MS);
	return ALP_OK;
}

alp_status_t optiga_trust_m_init_with_reset(optiga_trust_m_t         *ctx,
                                            alp_i2c_t                *bus,
                                            uint8_t                   addr_7bit,
                                            optiga_trust_m_reset_fn_t reset,
                                            void                     *reset_user)
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
	/* Kept for session_open()'s reset-and-retry (#2517). */
	ctx->reset      = reset;
	ctx->reset_user = reset_user;

	/* If it still does not ACK after the wake retries, either it is
	 * wedged in its idle state (#2507) or it isn't populated /
	 * mis-strapped.  With a reset hook, reset once and probe again, so
	 * NOT_READY means a fitted part failed to answer even after a
	 * hardware reset. */
	alp_status_t s = probe_i2c_state(ctx);
	if (s != ALP_OK && reset != NULL) {
		if (hw_reset(reset, reset_user) == ALP_OK) s = probe_i2c_state(ctx);
	}
	if (s != ALP_OK) return ALP_ERR_NOT_READY;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t optiga_trust_m_init(optiga_trust_m_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
	return optiga_trust_m_init_with_reset(ctx, bus, addr_7bit, NULL, NULL);
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
	/* The library writes the received length when the op completes, which
	 * after a timeout is a later call: keep it in ctx, not on the stack.
	 * resp is the caller's and must stay valid until the next call. */
	alp_status_t s = session_open(ctx, SESSION_RAW);
	if (s != ALP_OK) return s;
	/* After session_open(): its drain of a timed-out op rewrites xfer_len. */
	ctx->xfer_len  = resp_cap > UINT16_MAX ? UINT16_MAX : (uint16_t)resp_cap;
	ctx->op_status = OPTIGA_LIB_BUSY;
	if (optiga_comms_transceive(ctx->comms, apdu, (uint16_t)apdu_len, resp, &ctx->xfer_len) !=
	    OPTIGA_LIB_SUCCESS) {
		return ALP_ERR_IO;
	}
	s = op_wait(ctx, timeout_ms, PEND_OTHER);
	if (s == ALP_OK) *resp_len = ctx->xfer_len;
	return s;
}

alp_status_t optiga_trust_m_read_product_info(optiga_trust_m_t              *ctx,
                                              optiga_trust_m_product_info_t *out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL) return ALP_ERR_INVAL;

	alp_status_t s = session_open(ctx, SESSION_UTIL);
	if (s != ALP_OK) return s;
	/* Buffers live in ctx: a timed-out read completes during a later call. */
	ctx->xfer_len  = sizeof(ctx->uid);
	ctx->op_status = OPTIGA_LIB_BUSY;
	if (optiga_util_read_data(ctx->util, OPTIGA_OID_COPROCESSOR_UID, 0, ctx->uid, &ctx->xfer_len) !=
	    OPTIGA_LIB_SUCCESS) {
		return ALP_ERR_IO;
	}
	s = op_wait(ctx, OPTIGA_READ_TIMEOUT_MS, PEND_OTHER);
	if (s != ALP_OK) return s;
	/* The object is exactly the 27-byte UID laid out as the struct. */
	if (ctx->xfer_len != sizeof(ctx->uid)) return ALP_ERR_IO;
	memcpy(out, ctx->uid, sizeof(ctx->uid));
	return ALP_OK;
}

void optiga_trust_m_deinit(optiga_trust_m_t *ctx)
{
	if (ctx == NULL) return;
	/* Best effort: the library holds ctx as the op callback context, so
	 * let a timed-out op finish before the instances are destroyed. */
	(void)op_drain(ctx);
	if (ctx->initialised) session_close(ctx);
	if (ctx->util != NULL) (void)optiga_util_destroy(ctx->util);
	if (ctx->comms != NULL) optiga_comms_destroy(ctx->comms);
	ctx->util        = NULL;
	ctx->comms       = NULL;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
