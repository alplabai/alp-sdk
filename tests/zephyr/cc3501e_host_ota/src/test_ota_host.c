/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hermetic host-side tests for the CC3501E OTA wrappers
 * (chips/cc3501e/cc3501e.c: cc3501e_ota_begin / _write / _finish /
 * _abort / _status / _update).  These exercise the REAL host driver --
 * its request encoding, the 3-wire lockstep transaction (header ->
 * payload -> reply header -> reply payload), and the reply decoding --
 * against a software model of the firmware OTA slave.
 *
 * The model lives entirely in the test's alp_spi_transceive() stub: it
 * plays the firmware SPI-slave role in the CS-less lockstep, tracking the
 * OTA session state machine (BEGIN -> WRITE* -> FINISH -> STATUS) so the
 * host wrappers see a faithful wire peer WITHOUT any TI silicon, PSA-FWU,
 * or Zephyr SPI backend.  We assert both directions of the wire contract:
 * the bytes the host EMITS (BEGIN's total_len, each WRITE's offset + data)
 * and the values it DECODES back (OTA_STATUS state / bytes_written /
 * total_len).
 *
 * Wire framing mirrors <alp/protocol/cc3501e.h> and the firmware transport
 * (firmware/cc3501e/hal/ti/transport_hw_ti_spi.c): a 4-byte LE header
 * [cmd | flags | payload_len(LE16)] then payload; the reply header echoes
 * the request cmd and declares the reply payload length; the reply
 * payload's first byte is the response status (ALP_CC3501E_RESP_*).
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/protocol/cc3501e.h"

/* ---- software model of the firmware OTA slave ------------------------------ */

/* The host clocks each request as up to four fixed-count transfers; the model
 * walks these phases so it knows whether a given transceive() is a request
 * header, a request payload, a reply-header read, or a reply-payload read. */
enum slave_phase {
	PH_REQ_HDR = 0, /* next transfer is a 4-byte request header  */
	PH_REQ_PL,      /* next transfer is the request payload      */
	PH_REPLY_HDR,   /* host reads the 4-byte reply header        */
	PH_REPLY_PL,    /* host reads the reply payload (status+data) */
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;     /* opcode of the in-flight request         */
	uint16_t         req_len; /* declared request payload length         */
	uint8_t          req_pl[ALP_CC3501E_MAX_PAYLOAD];

	/* Staged reply (built when the request completes, drained over phases 3+4). */
	uint8_t  reply_pl[ALP_CC3501E_MAX_PAYLOAD]; /* status byte + data */
	uint16_t reply_len;                         /* == 1 + data bytes */

	/* OTA session state the model tracks, exactly like the firmware. */
	uint8_t  ota_state;  /* alp_cc3501e_ota_state_t */
	uint32_t ota_total;  /* total_len from BEGIN     */
	uint32_t ota_cursor; /* bytes accepted so far    */

	/* Inspection: the full concatenated image the host streamed (for _update). */
	uint8_t  image[8192];
	uint32_t image_len;

	/* Fault injection: make the NEXT OTA_WRITE reject (out-of-order sim). */
	bool fail_next_write;
} slave;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase     = PH_REQ_HDR;
	slave.ota_state = ALP_CC3501E_OTA_STATE_IDLE;
}

static void stage_status(uint8_t st)
{
	slave.reply_pl[0] = st;
	slave.reply_len   = 1u;
}

/* Build the reply for the just-received request (called at the end of the
 * request phase, before the host reads the reply header). */
static void slave_dispatch(void)
{
	switch (slave.cmd) {
	case ALP_CC3501E_CMD_OTA_BEGIN: {
		slave.ota_total  = (uint32_t)slave.req_pl[0] | ((uint32_t)slave.req_pl[1] << 8) |
		                   ((uint32_t)slave.req_pl[2] << 16) | ((uint32_t)slave.req_pl[3] << 24);
		slave.ota_cursor = 0u;
		slave.image_len  = 0u;
		slave.ota_state  = ALP_CC3501E_OTA_STATE_WRITING;
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	}
	case ALP_CC3501E_CMD_OTA_WRITE: {
		const uint32_t offset   = (uint32_t)slave.req_pl[0] | ((uint32_t)slave.req_pl[1] << 8) |
		                          ((uint32_t)slave.req_pl[2] << 16) |
		                          ((uint32_t)slave.req_pl[3] << 24);
		const uint16_t data_len = (uint16_t)(slave.req_len - 4u);
		if (slave.fail_next_write) {
			slave.fail_next_write = false;
			stage_status(ALP_CC3501E_RESP_ERR_INTERNAL);
			break;
		}
		if (slave.ota_state != ALP_CC3501E_OTA_STATE_WRITING || offset != slave.ota_cursor) {
			/* Out-of-order / no session -- reject, like the firmware. */
			stage_status(ALP_CC3501E_RESP_ERR_INVALID);
			break;
		}
		memcpy(&slave.image[slave.image_len], &slave.req_pl[4], data_len);
		slave.image_len += data_len;
		slave.ota_cursor += data_len;
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	}
	case ALP_CC3501E_CMD_OTA_FINISH: {
		if (slave.ota_state == ALP_CC3501E_OTA_STATE_WRITING &&
		    slave.ota_cursor == slave.ota_total) {
			slave.ota_state = ALP_CC3501E_OTA_STATE_STAGED;
			stage_status(ALP_CC3501E_RESP_OK);
		} else {
			stage_status(ALP_CC3501E_RESP_ERR_NOT_READY);
		}
		break;
	}
	case ALP_CC3501E_CMD_OTA_ABORT: {
		slave.ota_state  = ALP_CC3501E_OTA_STATE_IDLE;
		slave.ota_cursor = 0u;
		stage_status(ALP_CC3501E_RESP_OK);
		break;
	}
	case ALP_CC3501E_CMD_OTA_STATUS: {
		/* status(1) + alp_cc3501e_ota_status_t on the wire (12 bytes):
		 * state, reserved[3], bytes_written(LE32), total_len(LE32). */
		slave.reply_pl[0]  = ALP_CC3501E_RESP_OK;
		slave.reply_pl[1]  = slave.ota_state;
		slave.reply_pl[2]  = 0u;
		slave.reply_pl[3]  = 0u;
		slave.reply_pl[4]  = 0u;
		slave.reply_pl[5]  = (uint8_t)(slave.ota_cursor & 0xFFu);
		slave.reply_pl[6]  = (uint8_t)((slave.ota_cursor >> 8) & 0xFFu);
		slave.reply_pl[7]  = (uint8_t)((slave.ota_cursor >> 16) & 0xFFu);
		slave.reply_pl[8]  = (uint8_t)((slave.ota_cursor >> 24) & 0xFFu);
		slave.reply_pl[9]  = (uint8_t)(slave.ota_total & 0xFFu);
		slave.reply_pl[10] = (uint8_t)((slave.ota_total >> 8) & 0xFFu);
		slave.reply_pl[11] = (uint8_t)((slave.ota_total >> 16) & 0xFFu);
		slave.reply_pl[12] = (uint8_t)((slave.ota_total >> 24) & 0xFFu);
		slave.reply_len    = 13u;
		break;
	}
	default:
		stage_status(ALP_CC3501E_RESP_ERR_INVALID);
		break;
	}
}

/* ---- test doubles for the alp_* seams the host driver links against -------- */

/* The one seam that carries the wire contract: route the host's fixed-count
 * transfers through the slave model above.  On read phases the host ignores
 * the tx bytes (they are 0xFF fill) and consumes rx; on write phases the
 * model records tx and the host ignores rx -- so this single function models
 * a full-duplex CS-less lockstep with a purely half-duplex payload each phase. */
alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)bus;
	if (len == 0u) {
		return ALP_OK;
	}
	switch (slave.phase) {
	case PH_REQ_HDR:
		/* [cmd | flags | payload_len(LE16)] */
		slave.cmd     = tx[0];
		slave.req_len = (uint16_t)tx[2] | ((uint16_t)tx[3] << 8);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		if (slave.req_len > 0u) {
			slave.phase = PH_REQ_PL;
		} else {
			slave_dispatch();
			slave.phase = PH_REPLY_HDR;
		}
		break;
	case PH_REQ_PL:
		memcpy(slave.req_pl, tx, len);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave_dispatch();
		slave.phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		/* Reply header echoes the cmd + declares the reply payload length. */
		rx[0]       = slave.cmd;
		rx[1]       = 0x00u; /* solicited reply */
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

/* The OTA path never touches these, but the host TU references them, so they
 * must resolve at link.  Delays are no-ops under the sim; the GPIO seams are
 * inert (the OTA wrappers pass a ctx with reset/ready pins unset). */
void alp_delay_us(uint32_t us)
{
	(void)us;
}
void alp_delay_ms(uint32_t ms)
{
	(void)ms;
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
static alp_spi_t *fake_bus = (alp_spi_t *)&fw; /* opaque, non-NULL; the stub ignores it */

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
}

/* ---- tests ----------------------------------------------------------------- */

/* BEGIN encodes total_len (LE32) on the wire and opens the session. */
ZTEST(cc3501e_host_ota, test_begin_encodes_total_len_and_opens_session)
{
	const uint32_t total = 0x00012345u;
	zassert_equal(cc3501e_ota_begin(&fw, total, 100u), ALP_OK, "BEGIN -> OK");

	zassert_equal(slave.cmd, ALP_CC3501E_CMD_OTA_BEGIN, "opcode reached the slave");
	zassert_equal(slave.req_len, 4u, "BEGIN payload is the 4-byte total_len");
	zassert_equal(slave.ota_total, total, "total_len decoded from the LE32 the host emitted");
	zassert_equal(slave.ota_state, ALP_CC3501E_OTA_STATE_WRITING, "session is WRITING");
}

/* WRITE encodes offset(LE32) + inline bytes, and the host feeds it the exact
 * chunk; the model advances its cursor when offset == cursor. */
ZTEST(cc3501e_host_ota, test_write_encodes_offset_and_data)
{
	zassert_equal(cc3501e_ota_begin(&fw, 8u, 100u), ALP_OK, "BEGIN");

	const uint8_t chunk0[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	const uint8_t chunk1[4] = { 0x01, 0x02, 0x03, 0x04 };
	zassert_equal(cc3501e_ota_write(&fw, 0u, chunk0, sizeof chunk0, 100u), ALP_OK, "WRITE @0");
	zassert_equal(slave.req_len, 4u + 4u, "WRITE payload = offset(4) + data(4)");
	zassert_equal(slave.ota_cursor, 4u, "cursor advanced by the first chunk");

	zassert_equal(cc3501e_ota_write(&fw, 4u, chunk1, sizeof chunk1, 100u), ALP_OK, "WRITE @4");
	zassert_equal(slave.ota_cursor, 8u, "cursor advanced by the second chunk");

	/* The reassembled image equals what we streamed, in order. */
	const uint8_t expect[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
	zassert_equal(slave.image_len, 8u, "all bytes landed");
	zassert_mem_equal(slave.image, expect, 8u, "image reassembled in order");
}

/* WRITE rejects an over-sized chunk at the host BEFORE any transfer. */
ZTEST(cc3501e_host_ota, test_write_oversize_chunk_rejected_by_host)
{
	static uint8_t big[ALP_CC3501E_OTA_MAX_CHUNK + 1u];
	zassert_equal(cc3501e_ota_write(&fw, 0u, big, sizeof big, 100u),
	              ALP_ERR_INVAL,
	              "chunk > ALP_CC3501E_OTA_MAX_CHUNK -> INVAL (host-side guard)");
	zassert_equal(slave.cmd, 0u, "no transfer was clocked");
}

/* FINISH only stages once every declared byte has been written. */
ZTEST(cc3501e_host_ota, test_finish_requires_full_image_then_stages)
{
	zassert_equal(cc3501e_ota_begin(&fw, 8u, 100u), ALP_OK, "BEGIN 8 bytes");
	const uint8_t four[4] = { 1, 2, 3, 4 };
	zassert_equal(cc3501e_ota_write(&fw, 0u, four, sizeof four, 100u), ALP_OK, "WRITE @0");

	/* Only 4 of 8 bytes written -> the slave refuses FINISH. */
	zassert_equal(cc3501e_ota_finish(&fw, 100u), ALP_ERR_NOT_READY, "partial image -> NOT_READY");
	zassert_equal(slave.ota_state, ALP_CC3501E_OTA_STATE_WRITING, "still WRITING");

	zassert_equal(cc3501e_ota_write(&fw, 4u, four, sizeof four, 100u), ALP_OK, "WRITE @4");
	zassert_equal(cc3501e_ota_finish(&fw, 100u), ALP_OK, "full image -> FINISH OK");
	zassert_equal(slave.ota_state, ALP_CC3501E_OTA_STATE_STAGED, "session STAGED");
}

/* STATUS round-trips the 12-byte body: the host decodes state / bytes_written /
 * total_len back out of the reply payload it read. */
ZTEST(cc3501e_host_ota, test_status_decodes_progress)
{
	zassert_equal(cc3501e_ota_begin(&fw, 16u, 100u), ALP_OK, "BEGIN 16 bytes");
	const uint8_t six[6] = { 9, 8, 7, 6, 5, 4 };
	zassert_equal(cc3501e_ota_write(&fw, 0u, six, sizeof six, 100u), ALP_OK, "WRITE 6 bytes");

	alp_cc3501e_ota_status_t st;
	memset(&st, 0xA5, sizeof st);
	zassert_equal(cc3501e_ota_status(&fw, &st, 100u), ALP_OK, "STATUS -> OK");
	zassert_equal(st.state, ALP_CC3501E_OTA_STATE_WRITING, "decoded state");
	zassert_equal(st.bytes_written, 6u, "decoded bytes_written");
	zassert_equal(st.total_len, 16u, "decoded total_len");
}

/* STATUS with a NULL out-struct is rejected at the host boundary. */
ZTEST(cc3501e_host_ota, test_status_null_out_invalid)
{
	zassert_equal(cc3501e_ota_status(&fw, NULL, 100u), ALP_ERR_INVAL, "NULL out -> INVAL");
}

/* ABORT resets the session back to IDLE. */
ZTEST(cc3501e_host_ota, test_abort_resets_session)
{
	zassert_equal(cc3501e_ota_begin(&fw, 8u, 100u), ALP_OK, "BEGIN");
	zassert_equal(cc3501e_ota_abort(&fw, 100u), ALP_OK, "ABORT -> OK");
	zassert_equal(slave.ota_state, ALP_CC3501E_OTA_STATE_IDLE, "session back to IDLE");
}

/* cc3501e_ota_update drives the whole cycle: BEGIN -> page-aligned WRITE loop
 * (incl. a short tail) -> FINISH.  Verify the image the slave reassembled is
 * byte-identical to the source blob and the session ends STAGED. */
ZTEST(cc3501e_host_ota, test_update_streams_full_blob_and_stages)
{
	/* 700 bytes = two 256 B pages + a 188 B tail (exercises the partial-tail path). */
	static uint8_t blob[700];
	for (size_t i = 0; i < sizeof blob; i++) {
		blob[i] = (uint8_t)(i * 7u + 3u);
	}

	zassert_equal(cc3501e_ota_update(&fw, blob, sizeof blob, 200u), ALP_OK, "update -> OK");
	zassert_equal(slave.ota_total, (uint32_t)sizeof blob, "BEGIN declared the blob size");
	zassert_equal(slave.image_len, (uint32_t)sizeof blob, "every byte streamed");
	zassert_mem_equal(slave.image, blob, sizeof blob, "reassembled image == source blob");
	zassert_equal(slave.ota_state, ALP_CC3501E_OTA_STATE_STAGED, "ends STAGED");
}

/* cc3501e_ota_update rejects a NULL / empty image at the host boundary. */
ZTEST(cc3501e_host_ota, test_update_null_image_invalid)
{
	zassert_equal(cc3501e_ota_update(&fw, NULL, 16u, 100u), ALP_ERR_INVAL, "NULL image -> INVAL");
	static const uint8_t one = 0u;
	zassert_equal(cc3501e_ota_update(&fw, &one, 0u, 100u), ALP_ERR_INVAL, "zero len -> INVAL");
}

ZTEST_SUITE(cc3501e_host_ota, NULL, NULL, reset_before, NULL, NULL);
