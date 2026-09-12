/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hermetic host round-trip test for BLE_GATT_REGISTER (0x38) dynamic
 * service registration (#480): drives the REAL CC3501E BLE backend ops
 * vtable (src/backends/ble/cc3501e.c, reached via the REAL
 * alp_backend_select() registry selector -- the same production code path
 * alp_ble_gatt_register_service() uses) and the REAL chip driver
 * (chips/cc3501e/cc3501e_ble.c) against a software model of the firmware
 * SPI slave -- no silicon, no BLE stack.  Same pattern as
 * tests/zephyr/cc3501e_wifi_backend_security (issue #742).
 *
 * Covers the wire contract in <alp/protocol/cc3501e.h>:
 *   (a) ENCODE -- alp_ble_service_def_t -> version | service_uuid[16] |
 *       num_chars | per-char uuid[16] | properties(1) | initial_len(LE16) |
 *       initial_value, byte-exact.
 *   (b) DECODE -- a crafted reply (status | num_handles | handle(LE16)*n)
 *       -> handles_out, in declaration order.
 *   (c) ERRORS -- num_chars past the wire cap -> ALP_ERR_INVAL (host-side,
 *       before any transfer); a firmware ordering-guard reply
 *       (ALP_CC3501E_RESP_ERR_STATE) -> ALP_ERR_BUSY, returned on the FIRST
 *       poll (not a confusing IO/timeout, and not retried for the whole
 *       budget -- see cc3501e_ble_gatt_register's @note on the NimBLE
 *       ble_gatts_mutable() constraint).
 *
 *       #2035 follow-up -- REVISED CONTRACT (was: retried to budget,
 *       surfaced ALP_ERR_TIMEOUT): a DECODED ALP_CC3501E_RESP_ERR_RADIO /
 *       ERR_PROTOCOL / ERR_INTERNAL reply is now ALSO terminal, like
 *       ERR_STATE above, and for the identical reason -- poll_by_repeat
 *       resends the SAME req_seq unchanged on every attempt so the
 *       firmware's per-seq retry latch (proto v8) answers a repeat from its
 *       latch WITHOUT re-executing the op, so retrying a genuine decoded
 *       device-side failure can only ever replay that SAME failure. Burning
 *       the whole poll budget on a guaranteed-unchanging answer was itself
 *       the bug (BLE_ENABLE cost the full CC35_BLE_TIMEOUT_MS, ~10 s, for a
 *       radio fault the FIRST reply already told us about) and it reported
 *       the wrong error class (ALP_ERR_TIMEOUT, "never answered", instead of
 *       ALP_ERR_IO, "answered with a real fault").  See
 *       cc3501e_internal.h's poll_by_repeat EXCEPTION 2.
 *
 *       This is still UN-MASKED (#480/#892) in the sense that matters: a
 *       decoded device-side fault is never folded into ALP_ERR_BUSY the way
 *       the old IO||TIMEOUT->BUSY remap used to do -- it now surfaces its
 *       real ALP_ERR_IO, promptly, instead of ALP_ERR_TIMEOUT eventually.
 *       A genuinely UNDECODED, PRE-DECODE ALP_ERR_IO (the bridge link
 *       transport itself down for this transaction -- no reply ever framed)
 *       is a different thing again and is NOT covered by this carve-out: it
 *       still retries to the full budget, same as before this change -- see
 *       test_register_pre_decode_io_fault_still_retries.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/chips/cc3501e.h>
#include <alp/peripheral.h>

#include "backends/ble/ble_ops.h"
#include "cc3501e_reply_model.h"

/* This test does NOT link src/ble_dispatch.c (see CMakeLists.txt comment),
 * so it instantiates the ble class-range entry itself -- the one thing
 * alp_backend_select("ble", ...) needs to find src/backends/ble/cc3501e.c's
 * ALP_BACKEND_REGISTER rows. */
ALP_BACKEND_DEFINE_CLASS(ble);

/* ---- software model of the firmware slave --------------------------------
 * Simpler than tests/zephyr/cc3501e_host_driver's: every test here issues at
 * most one BLE_GATT_REGISTER (plus the BLE_ENABLE the backend's open() fires
 * first), so the reply is staged directly by the test before each call
 * rather than computed from the opcode. */

enum slave_phase {
	PH_REQ_HDR = 0,
	PH_REQ_PL,
	PH_REPLY_HDR,
	PH_REPLY_PL,
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;
	uint16_t         req_len;
	uint8_t          req_pl[ALP_CC3501E_MAX_PAYLOAD]; /* captured request payload */

	uint8_t  reply_pl[ALP_CC3501E_MAX_PAYLOAD]; /* staged reply: status + data */
	uint16_t reply_len;                         /* == 1 + data bytes */

	uint32_t req_hdr_count; /* # of PH_REQ_HDR entries -- one per request/reply
	                         * round trip; proves poll_by_repeat did (or did
	                         * NOT) retry a given command. */
} slave;

/* Set by test_register_pre_decode_io_fault_still_retries to force every
 * transceive to fail at the TRANSPORT level -- no bytes exchanged, slave.phase
 * untouched -- simulating "the bridge link was DOWN for this transaction"
 * (cc3501e_internal.h) rather than a decoded device-side reply. */
static bool     force_transport_fail;
static uint32_t transport_fail_calls;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase          = PH_REQ_HDR;
	force_transport_fail = false;
	transport_fail_calls = 0u;
	/* Default: every request bare-OKs (covers BLE_ENABLE, fired by the
	 * backend's open()) until a test stages something else for the request
	 * it is about to issue.  Staged here, before any request has landed, so
	 * the cmd the CRC trailer covers (cc3501e_reply_model.h) is the
	 * EXPLICIT opcode this reply is known in advance to answer -- BLE_ENABLE
	 * -- not `slave.cmd`, which is still whatever a PRIOR request left it as. */
	slave.reply_len = cc3501e_model_stage_reply(
	    slave.reply_pl, ALP_CC3501E_CMD_BLE_ENABLE, ALP_CC3501E_RESP_OK, NULL, 0u);
}

/* An ALP_CC3501E_RESP_ERR_* frame-level reject -- the legacy (unpadded,
 * no-CRC) shape is fine here: cc3501e_reply_verdict() only demands the
 * MAJOR-4 trailer for a 0x5A status (see cc3501e_reply_model.h). */
static void stage_status(uint8_t st)
{
	slave.reply_len = cc3501e_model_stage_legacy_reply(slave.reply_pl, st, NULL, 0u);
}

/* BLE_GATT_REGISTER success reply.  Two layers, per <alp/protocol/cc3501e.h>:
 * byte 0 is the FRAME-level resp (cc3501e_request's resp_to_status() input,
 * stripped before the driver ever sees it); bytes 1.. are the reply DATA the
 * driver decodes -- in_status(1)=0 | num_handles(1) | attr_handle(LE16)*n.
 * Staged before the request lands (same reasoning as slave_reset() above),
 * so the cmd is the explicit BLE_GATT_REGISTER opcode, not `slave.cmd`. */
static void stage_register_ok(const uint16_t *handles, uint8_t num_handles)
{
	uint8_t data[2u + 2u * 8u]; /* in-payload status(1) + num_handles(1) + up to 8 handles */
	data[0] = 0u;               /* in-payload status: OK */
	data[1] = num_handles;
	for (uint8_t i = 0; i < num_handles; i++) {
		data[2u + 2u * i]      = (uint8_t)(handles[i] & 0xFFu);
		data[2u + 2u * i + 1u] = (uint8_t)((handles[i] >> 8) & 0xFFu);
	}
	slave.reply_len = cc3501e_model_stage_reply(slave.reply_pl,
	                                            ALP_CC3501E_CMD_BLE_GATT_REGISTER,
	                                            ALP_CC3501E_RESP_OK,
	                                            data,
	                                            (uint16_t)(2u + 2u * (uint16_t)num_handles));
}

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)bus;
	if (len == 0u) {
		return ALP_OK;
	}
	if (force_transport_fail) {
		/* Deliberately does NOT touch rx or slave.phase: a real transceive
		 * failure exchanges no bytes and the fake slave never advanced, so
		 * the next attempt starts the same clean 4-phase cycle. */
		transport_fail_calls++;
		return ALP_ERR_IO;
	}
	switch (slave.phase) {
	case PH_REQ_HDR:
		slave.cmd = tx[0];
		slave.req_hdr_count++;
		slave.req_len = (uint16_t)tx[2] | ((uint16_t)tx[3] << 8);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave.phase = (slave.req_len > 0u) ? PH_REQ_PL : PH_REPLY_HDR;
		break;
	case PH_REQ_PL:
		memcpy(slave.req_pl, tx, len);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave.phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		rx[0]       = slave.cmd; /* echo cmd */
		rx[1]       = 0x00u;     /* solicited */
		rx[2]       = (uint8_t)(slave.reply_len & 0xFFu);
		rx[3]       = (uint8_t)((slave.reply_len >> 8) & 0xFFu);
		slave.phase = PH_REPLY_PL;
		break;
	case PH_REPLY_PL:
		memcpy(rx, slave.reply_pl, len);
		slave.phase = PH_REQ_HDR;
		break;
	}
	return ALP_OK;
}

/* alp_delay_us is a no-op under the sim; the GPIO seams are inert (the
 * fixture's ctx leaves reset/enable/ready pins unset, so the wrappers under
 * test never call them). alp_delay_ms and alp_uptime_ms share one fake
 * millisecond counter (same pattern as tests/zephyr/cc3501e_poll_deadline):
 * poll_by_repeat()'s deadline (issue #1953) is measured against it, so the
 * genuine-radio-fault test's retry budget still elapses deterministically,
 * without any real sleeping. */
static uint64_t g_fake_now_ms;

void alp_delay_us(uint32_t us)
{
	(void)us;
}
void alp_delay_ms(uint32_t ms)
{
	g_fake_now_ms += ms;
}
uint64_t alp_uptime_ms(void)
{
	return g_fake_now_ms;
}
alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	(void)pin_id;
	return NULL;
}
alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

/* ---- fixture --------------------------------------------------------------- */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw;

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
	zassert_equal(alp_ble_cc3501e_attach(&fw), ALP_OK, "attach the live bridge handle");
}

/* Selects the CC3501E BLE backend the same way alp_ble_open() does (by exact
 * silicon_ref) and drives its open() -- the production dispatch path, minus
 * the alp_ble_t handle-pool bookkeeping this test doesn't need.  open() fires
 * BLE_ENABLE first (bare-OK by the slave_reset() default reply above). */
static alp_ble_radio_state_t open_cc3501e_state(void)
{
	const alp_backend_t *be = alp_backend_select("ble", "alif:ensemble:e7");
	zassert_not_null(be, "CC3501E BLE backend registered for alif:ensemble:e7");
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0, "picked the CC3501E backend");

	alp_ble_radio_state_t state = { .be_data = NULL, .ops = (const alp_ble_ops_t *)be->ops };
	alp_capabilities_t    caps  = { .flags = be->base_caps };
	zassert_equal(state.ops->open(&state, &caps), ALP_OK, "backend open -> OK (BLE_ENABLE)");
	return state;
}

/* ---- tests ------------------------------------------------------------- */

/* Distinct, easily-verifiable 16-byte UUID: b[i] = base + i. */
static alp_ble_uuid_t make_uuid(uint8_t base)
{
	alp_ble_uuid_t u;
	for (int i = 0; i < 16; i++) {
		u.b[i] = (uint8_t)(base + i);
	}
	return u;
}

/* (a) ENCODE + (b) DECODE: a 2-characteristic service registers to the exact
 * wire bytes <alp/protocol/cc3501e.h> documents, and the crafted reply's
 * handles land in handles_out in declaration order. */
ZTEST(cc3501e_ble_gatt_register, test_register_encodes_wire_bytes_and_decodes_handles)
{
	alp_ble_radio_state_t state = open_cc3501e_state();

	const alp_ble_uuid_t     svc_uuid    = make_uuid(0x00);
	const alp_ble_uuid_t     chr0_uuid   = make_uuid(0x10);
	const alp_ble_uuid_t     chr1_uuid   = make_uuid(0x20);
	const uint8_t            chr0_val[3] = { 0xAA, 0xBB, 0xCC };
	const alp_ble_char_def_t chars[2]    = {
		{ .uuid          = chr0_uuid,
		  .properties    = ALP_BLE_GATT_PROP_READ | ALP_BLE_GATT_PROP_NOTIFY,
		  .initial_value = chr0_val,
		  .initial_len   = sizeof(chr0_val) },
		{ .uuid          = chr1_uuid,
		  .properties    = ALP_BLE_GATT_PROP_WRITE,
		  .initial_value = NULL,
		  .initial_len   = 0u },
	};
	const alp_ble_service_def_t def = {
		.service_uuid = svc_uuid,
		.chars        = chars,
		.num_chars    = 2u,
	};

	const uint16_t want_handles[2] = { 0x0011u, 0x2233u };
	stage_register_ok(want_handles, 2u);

	alp_ble_attr_handle_t handles_out[2] = { 0 };
	zassert_equal(
	    state.ops->gatt_register_service(&state, &def, handles_out), ALP_OK, "GATT_REGISTER -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_BLE_GATT_REGISTER, "opcode 0x38 emitted");

	/* ---- (a) byte-exact request encoding ---- */
	size_t off = 0u;
	zassert_equal(
	    slave.req_pl[off], ALP_CC3501E_BLE_GATT_REGISTER_VERSION, "version byte @ offset 0");
	off += 1u;
	zassert_mem_equal(&slave.req_pl[off], svc_uuid.b, 16u, "service_uuid verbatim @ offset 1");
	off += 16u;
	zassert_equal(slave.req_pl[off], 2u, "num_chars @ offset 17");
	off += 1u;

	zassert_mem_equal(&slave.req_pl[off], chr0_uuid.b, 16u, "char0 uuid verbatim");
	off += 16u;
	zassert_equal(slave.req_pl[off], chars[0].properties, "char0 properties");
	off += 1u;
	zassert_equal(slave.req_pl[off], (uint8_t)(sizeof(chr0_val) & 0xFFu), "char0 initial_len lo");
	zassert_equal(slave.req_pl[off + 1u], 0u, "char0 initial_len hi (3 fits in one byte)");
	off += 2u;
	zassert_mem_equal(&slave.req_pl[off], chr0_val, sizeof(chr0_val), "char0 initial_value bytes");
	off += sizeof(chr0_val);

	zassert_mem_equal(&slave.req_pl[off], chr1_uuid.b, 16u, "char1 uuid verbatim");
	off += 16u;
	zassert_equal(slave.req_pl[off], chars[1].properties, "char1 properties");
	off += 1u;
	zassert_equal(slave.req_pl[off], 0u, "char1 initial_len lo (no initial value)");
	zassert_equal(slave.req_pl[off + 1u], 0u, "char1 initial_len hi");
	off += 2u;

	zassert_equal(slave.req_len, (uint16_t)off, "total request length matches the walked layout");

	/* ---- (b) reply decode: handles land in declaration order ---- */
	zassert_equal(handles_out[0], want_handles[0], "handle[0] decoded LE16");
	zassert_equal(handles_out[1], want_handles[1], "handle[1] decoded LE16");

	state.ops->close(&state);
}

/* (c) num_chars past the wire cap is rejected host-side -- no transfer ever
 * reaches the slave. */
ZTEST(cc3501e_ble_gatt_register, test_register_num_chars_over_max_rejected)
{
	alp_ble_radio_state_t state = open_cc3501e_state();

	alp_ble_char_def_t chars[ALP_CC3501E_BLE_GATT_MAX_CHARS + 1u];
	memset(chars, 0, sizeof(chars));
	const alp_ble_service_def_t def = {
		.chars     = chars,
		.num_chars = ALP_CC3501E_BLE_GATT_MAX_CHARS + 1u,
	};
	alp_ble_attr_handle_t handles_out[ALP_CC3501E_BLE_GATT_MAX_CHARS + 1u] = { 0 };

	slave.cmd = 0u; /* BLE_ENABLE from open() already ran; reset the marker */
	zassert_equal(state.ops->gatt_register_service(&state, &def, handles_out),
	              ALP_ERR_INVAL,
	              "num_chars > ALP_CC3501E_BLE_GATT_MAX_CHARS -> INVAL");
	zassert_equal(slave.cmd, 0u, "rejected before any transfer reached the wire");

	state.ops->close(&state);
}

/* (c) the firmware's NimBLE ble_gatts_mutable() ordering guard (register
 * attempted while advertising/scanning/connected) is reported on the wire as
 * the DISTINCT ALP_CC3501E_RESP_ERR_STATE (#480/#892, no longer folded into
 * RESP_ERR_RADIO) -- the host maps that straight to ALP_ERR_BUSY, and
 * TERMINALLY: exactly one request/reply round trip, no poll_by_repeat retry
 * loop burning the budget on a reject that will not change (see
 * cc3501e_ble_gatt_register's @note + poll_by_repeat's @note in
 * cc3501e_internal.h). */
ZTEST(cc3501e_ble_gatt_register, test_register_firmware_ordering_guard_maps_to_busy)
{
	alp_ble_radio_state_t state = open_cc3501e_state();

	const alp_ble_char_def_t chars[1] = {
		{ .properties = ALP_BLE_GATT_PROP_READ, .initial_value = NULL, .initial_len = 0u },
	};
	const alp_ble_service_def_t def = {
		.chars     = chars,
		.num_chars = 1u,
	};
	alp_ble_attr_handle_t handles_out[1] = { 0 };

	stage_status(ALP_CC3501E_RESP_ERR_STATE);
	slave.req_hdr_count = 0u; /* BLE_ENABLE from open() already ran; count only this call */

	zassert_equal(state.ops->gatt_register_service(&state, &def, handles_out),
	              ALP_ERR_BUSY,
	              "firmware ordering-guard trip -> ALP_ERR_BUSY, not IO/TIMEOUT");
	zassert_equal(slave.req_hdr_count,
	              1u,
	              "terminal reject -- exactly one round trip, no poll_by_repeat retry");

	state.ops->close(&state);
}

/* (c) UN-MASKING (#480/#892), REVISED (#2035 follow-up): a GENUINE, DECODED
 * radio failure -- distinct from the ordering-guard reject above -- still
 * reports RESP_ERR_RADIO on the wire, and resp_to_status() still maps that to
 * ALP_ERR_IO.  What changed is poll_by_repeat's handling of it: 0x06 is
 * ALSO ALP_CC3501E_CMD_GET_CAPABILITIES, and BLE_ENABLE's own req/reply cycle
 * sits four opcodes downstream of it in the phase-8 EVK sequence, so the OLD
 * "retry every ALP_ERR_IO to budget" behaviour was reviewed and found unsafe
 * -- see cc3501e_internal.h's poll_by_repeat EXCEPTION 2 and
 * ALP_CC3501E_RX_SCRATCH_NO_STATUS's comment in cc3501e_core.c.  A decoded
 * RESP_ERR_RADIO is now TERMINAL, exactly like the STATE reject above and for
 * the identical retry-latch reason (a repeat of the SAME req_seq can only
 * ever replay the SAME decoded failure, never spontaneously succeed): ONE
 * round trip, ALP_ERR_IO returned promptly -- not ALP_ERR_TIMEOUT after
 * burning the whole poll budget. Still un-masked in the sense #480/#892
 * cared about: never folded into ALP_ERR_BUSY the way the old
 * IO||TIMEOUT->BUSY remap used to. The OPPOSITE case -- a genuinely
 * UNDECODED pre-decode ALP_ERR_IO, which must still retry -- is
 * test_register_pre_decode_io_fault_still_retries below; there was no
 * coverage of that branch at all before this change. */
ZTEST(cc3501e_ble_gatt_register, test_register_genuine_radio_fault_stays_unmasked)
{
	alp_ble_radio_state_t state = open_cc3501e_state();

	const alp_ble_char_def_t chars[1] = {
		{ .properties = ALP_BLE_GATT_PROP_READ, .initial_value = NULL, .initial_len = 0u },
	};
	const alp_ble_service_def_t def = {
		.chars     = chars,
		.num_chars = 1u,
	};
	alp_ble_attr_handle_t handles_out[1] = { 0 };

	stage_status(ALP_CC3501E_RESP_ERR_RADIO);
	slave.req_hdr_count = 0u; /* BLE_ENABLE from open() already ran; count only this call */

	alp_status_t rc = state.ops->gatt_register_service(&state, &def, handles_out);
	zassert_equal(rc, ALP_ERR_IO, "decoded genuine radio fault -> ALP_ERR_IO, terminal");
	zassert_not_equal(rc, ALP_ERR_BUSY, "un-masked -- no longer folded into ALP_ERR_BUSY");
	zassert_equal(slave.req_hdr_count,
	              1u,
	              "terminal decoded fault -- exactly one round trip, no retry-to-budget");

	state.ops->close(&state);
}

/* (c) NEW (#2035 follow-up): the OPPOSITE of the case above -- a genuinely
 * UNDECODED, pre-decode ALP_ERR_IO (the bridge link transport itself down
 * for this transaction, no reply ever framed) is NOT the retry-latch case
 * cc3501e_internal.h's poll_by_repeat EXCEPTION 2 carves out, so it must
 * still retry to the full budget exactly like before this change -- only a
 * genuinely DECODED device-side status is terminal now, never a raw
 * transport hiccup. This also pins down that ALP_CC3501E_RX_SCRATCH_NO_STATUS
 * (cc3501e_core.c) is doing its job: alp_spi_transceive below returns
 * ALP_ERR_IO without writing ANY bytes into rx, so if
 * cc3501e_request_locked() did not explicitly poison ctx->rx_scratch[0] on
 * this path, it would carry over whatever the PRIOR successful decode
 * (BLE_ENABLE's bare RESP_OK, 0x5A) left there -- harmless by luck here, but
 * exactly the residue class the reworked fix exists to close everywhere,
 * not just for the one opcode (0x06) the unsafe original version special-
 * cased. No test before this one exercised a goto-out / pre-decode branch at
 * all. */
ZTEST(cc3501e_ble_gatt_register, test_register_pre_decode_io_fault_still_retries)
{
	alp_ble_radio_state_t state = open_cc3501e_state();

	const alp_ble_char_def_t chars[1] = {
		{ .properties = ALP_BLE_GATT_PROP_READ, .initial_value = NULL, .initial_len = 0u },
	};
	const alp_ble_service_def_t def = {
		.chars     = chars,
		.num_chars = 1u,
	};
	alp_ble_attr_handle_t handles_out[1] = { 0 };

	force_transport_fail = true;
	transport_fail_calls = 0u;

	alp_status_t rc = state.ops->gatt_register_service(&state, &def, handles_out);
	zassert_equal(rc, ALP_ERR_TIMEOUT, "persistent pre-decode transport fault -> ALP_ERR_TIMEOUT");
	zassert_true(transport_fail_calls > 1u,
	             "pre-decode IO fault IS retried, unlike a decoded device-side one");

	force_transport_fail = false;
	state.ops->close(&state);
}

ZTEST_SUITE(cc3501e_ble_gatt_register, NULL, NULL, reset_before, NULL, NULL);
