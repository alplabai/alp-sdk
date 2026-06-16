/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the gd32-bridge SPI-slave transport seams
 * (firmware/gd32-bridge/src/transport_spi.c) -- the CS-framed staging
 * layer between the byte-level HAL and protocol_dispatch().
 *
 * The regression that motivates this suite (silicon, 2026-06-04): the
 * gd32 HAL backend consumes the staged reply through
 * spi_slave_tx_next_byte() at stage time (it drains the bytes into its
 * TX DMA buffer), so when the host's reply read lands BEFORE a slow
 * handler has staged the reply, the resulting all-0x00 "drain"
 * transaction used to find the cursor already spent and re-armed a
 * zero-length reply -- every subsequent re-read clocked 0x00 forever
 * and the command failed 255-of-256 despite the correct reply sitting
 * in the staging buffer.  The drain gate now REWINDS the cursor; these
 * tests pin that contract from the HAL's point of view.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "protocol.h"
#include "transport.h"

/* PING request and its byte-identical reply: SOF | 0x00 | CRC(0xFF84)
 * emitted lo-byte first on the wire (silicon-verified). */
static const uint8_t ping_frame[] = { 0xA5u, 0x00u, 0x84u, 0xFFu };

/* Replays one CS transaction through the seams, as the HAL's CS-EXTI
 * rising path does: reset staging, feed the captured bytes, decode. */
static void transaction(const uint8_t *bytes, size_t len)
{
	spi_slave_cs_low();
	for (size_t i = 0; i < len; i++) {
		spi_slave_rx_byte(bytes[i]);
	}
	spi_slave_cs_high();
}

/* Drains the staged reply exactly the way hal/transport_hw_gd32.c
 * fills its TX DMA buffer: pull bytes while the transport reports
 * them pending.  Returns the number of bytes drained. */
static size_t hal_drain(uint8_t *out, size_t cap)
{
	size_t n = 0;
	while (spi_slave_tx_pending() && n < cap) {
		out[n++] = spi_slave_tx_next_byte();
	}
	return n;
}

ZTEST(gd32_bridge_transport, test_ping_stages_reply)
{
	uint8_t reply[80];

	transport_spi_init();
	transaction(ping_frame, sizeof ping_frame);

	size_t n = hal_drain(reply, sizeof reply);

	zassert_equal(n, sizeof ping_frame, "PING reply is 4 bytes");
	zassert_mem_equal(reply, ping_frame, sizeof ping_frame,
	                  "PING reply is byte-identical to the request");
}

/* The silicon regression: after the HAL consumed the staged reply, an
 * all-0x00 drain transaction must REWIND the cursor so the HAL can
 * re-drain and re-arm the very same reply for the host's re-read. */
ZTEST(gd32_bridge_transport, test_drain_rewinds_consumed_reply)
{
	uint8_t       first[80], again[80];
	const uint8_t zeros[8] = { 0 };

	transport_spi_init();
	transaction(ping_frame, sizeof ping_frame);
	size_t n_first = hal_drain(first, sizeof first);
	zassert_equal(n_first, sizeof ping_frame, "reply staged");
	zassert_false(spi_slave_tx_pending(), "cursor consumed by the HAL drain");

	/* Host read that found nothing useful -> all-0x00 capture. */
	transaction(zeros, sizeof zeros);

	zassert_true(spi_slave_tx_pending(), "drain gate must rewind the staged reply (silicon bug "
	                                     "2026-06-04: spent cursor disarmed every re-read)");
	size_t n_again = hal_drain(again, sizeof again);
	zassert_equal(n_again, n_first, "full reply re-armed");
	zassert_mem_equal(again, first, n_first, "identical bytes re-armed");
}

/* Repeated drains must be idempotent -- each one re-arms the same
 * reply (the host may re-read several times down its backoff ladder). */
ZTEST(gd32_bridge_transport, test_drain_rearm_is_idempotent)
{
	uint8_t       buf[80];
	const uint8_t zeros[4] = { 0 };

	transport_spi_init();
	transaction(ping_frame, sizeof ping_frame);
	(void)hal_drain(buf, sizeof buf);

	for (int i = 0; i < 3; i++) {
		transaction(zeros, sizeof zeros);
		size_t n = hal_drain(buf, sizeof buf);
		zassert_equal(n, sizeof ping_frame, "re-arm %d intact", i);
		zassert_mem_equal(buf, ping_frame, sizeof ping_frame, "re-arm %d byte-exact", i);
	}
}

/* An EMPTY transaction (CS toggled, zero bytes captured -- the edge-
 * coalescing case where a colliding read's bytes hit the mid-reset SPI
 * and are lost) must also rewind: the gd32 backend re-drains after
 * every rising edge, and a spent cursor would disarm TX and force a
 * guaranteed second miss (silicon 2026-06-04, the ADC double miss). */
ZTEST(gd32_bridge_transport, test_empty_transaction_rewinds)
{
	uint8_t buf[80];

	transport_spi_init();
	transaction(ping_frame, sizeof ping_frame);
	(void)hal_drain(buf, sizeof buf);
	zassert_false(spi_slave_tx_pending(), "cursor consumed");

	/* CS pulse with no captured bytes. */
	transaction(NULL, 0u);

	zassert_true(spi_slave_tx_pending(), "empty transaction must rewind");
	size_t n = hal_drain(buf, sizeof buf);
	zassert_equal(n, sizeof ping_frame, "full reply re-armed");
	zassert_mem_equal(buf, ping_frame, sizeof ping_frame, "byte-exact");
}

/* A fresh request replaces the staged reply -- the rewind must never
 * resurrect a PREVIOUS command's reply once a new one decodes. */
ZTEST(gd32_bridge_transport, test_new_request_replaces_staged_reply)
{
	uint8_t       buf[80];
	const uint8_t zeros[4] = { 0 };

	/* GET_VERSION request: SOF | 0x01 | CRC.  CRC over [A5 01]
     * computed by the linked production crc16_ccitt_false. */
	uint8_t        gv[4] = { 0xA5u, 0x01u, 0, 0 };
	const uint16_t crc   = crc16_ccitt_false(gv, 2u);

	gv[2]                = (uint8_t)(crc & 0xFFu);
	gv[3]                = (uint8_t)(crc >> 8);

	transport_spi_init();
	transaction(ping_frame, sizeof ping_frame);
	(void)hal_drain(buf, sizeof buf);

	transaction(gv, sizeof gv);
	size_t n = hal_drain(buf, sizeof buf);

	/* GET_VERSION reply: SOF | STATUS | MAJOR MINOR PATCH | CRC = 7 B. */
	zassert_equal(n, 7u, "GET_VERSION reply staged");
	zassert_equal(buf[0], 0xA5u, "SOF");
	zassert_equal(buf[1], 0x00u, "STATUS_OK");

	/* And the drain re-arms the NEW reply, not the old PING. */
	transaction(zeros, sizeof zeros);
	size_t n2 = hal_drain(buf, sizeof buf);
	zassert_equal(n2, 7u, "rewind re-arms the current reply");
}

/* A corrupted (non-SOF, non-zero) capture must stage a loud STATUS_IO
 * error reply -- not preserve the previous reply, not stay silent. */
ZTEST(gd32_bridge_transport, test_mangled_request_stages_io_error)
{
	uint8_t       buf[80];
	const uint8_t garbage[5] = { 0x00u, 0xA5u, 0x12u, 0x34u, 0x56u };

	transport_spi_init();
	transaction(ping_frame, sizeof ping_frame);
	(void)hal_drain(buf, sizeof buf);

	transaction(garbage, sizeof garbage);
	size_t n = hal_drain(buf, sizeof buf);

	zassert_equal(n, 4u, "error reply is the empty envelope");
	zassert_equal(buf[0], 0xA5u, "SOF");
	zassert_equal(buf[1], 0x05u, "STATUS_IO");
}

/* ------------------------------------------------------------------ */
/* v0.7 STATUS_SEQ -- the stale-reply kill (silicon-fingerprinted      */
/* 2026-06-06: byte-exact replays on back-to-back identical frames).   */
/* ------------------------------------------------------------------ */

/* Builds + replays a CMD_LINK_FEATURES transaction wanting `feat`. */
static void negotiate(uint8_t feat)
{
	uint8_t        lf[5] = { 0xA5u, 0x81u /* CMD_LINK_FEATURES */, feat, 0, 0 };
	const uint16_t crc   = crc16_ccitt_false(lf, 3u);

	lf[3]                = (uint8_t)(crc & 0xFFu);
	lf[4]                = (uint8_t)(crc >> 8);
	transaction(lf, sizeof lf);
}

/* The full feature contract in one walk: the negotiation reply itself
 * is stamped (baseline), every fresh decode advances the stamp, the
 * drain/rewind re-serve KEEPS the stamp (that is the stale signature
 * the host detects), error envelopes advance + stamp too, and a
 * disable returns the wire to the legacy unstamped framing. */
ZTEST(gd32_bridge_transport, test_status_seq_stamp_contract)
{
	uint8_t       buf[80];
	const uint8_t zeros[4]   = { 0 };
	const uint8_t garbage[5] = { 0x00u, 0xA5u, 0x12u, 0x34u, 0x56u };

	transport_spi_init();

	/* Negotiate ON: reply carries granted=0x01 and stamp 1 (the
     * feature arms BEFORE the reply stages). */
	negotiate(0x01u);
	size_t n = hal_drain(buf, sizeof buf);
	zassert_equal(n, 5u, "LINK_FEATURES reply: SOF STATUS granted CRC");
	zassert_equal(buf[1], 0x10u, "stamp=1, code=OK");
	zassert_equal(buf[2], 0x01u, "STATUS_SEQ granted");
	/* The CRC covers the STAMPED status byte. */
	{
		const uint16_t crc = crc16_ccitt_false(buf, 3u);
		zassert_equal(buf[3], (uint8_t)(crc & 0xFFu), "stamped CRC lo");
		zassert_equal(buf[4], (uint8_t)(crc >> 8), "stamped CRC hi");
	}

	/* Fresh decode -> stamp advances. */
	transaction(ping_frame, sizeof ping_frame);
	n = hal_drain(buf, sizeof buf);
	zassert_equal(n, 4u, "PING reply staged");
	zassert_equal(buf[1], 0x20u, "stamp=2, code=OK");

	/* Drain/rewind re-serves the SAME stamp -- the stale fingerprint
     * a host uses to know its next request was never decoded. */
	transaction(zeros, sizeof zeros);
	n = hal_drain(buf, sizeof buf);
	zassert_equal(n, 4u, "rewound reply re-armed");
	zassert_equal(buf[1], 0x20u, "stamp UNCHANGED across rewind");

	/* Another fresh decode advances again... */
	transaction(ping_frame, sizeof ping_frame);
	n = hal_drain(buf, sizeof buf);
	zassert_equal(buf[1], 0x30u, "stamp=3 after the next decode");

	/* ...and a framing-error envelope is a fresh decode too. */
	transaction(garbage, sizeof garbage);
	n = hal_drain(buf, sizeof buf);
	zassert_equal(n, 4u, "error envelope");
	zassert_equal(buf[1], 0x45u, "stamp=4, code=STATUS_IO");

	/* Negotiate OFF: the disable-ack itself is already unstamped
     * (feature disarmed before staging), and the wire returns to the
     * legacy framing. */
	negotiate(0x00u);
	n = hal_drain(buf, sizeof buf);
	zassert_equal(n, 5u, "LINK_FEATURES reply");
	zassert_equal(buf[1], 0x00u, "disable-ack unstamped");
	zassert_equal(buf[2], 0x00u, "nothing granted");

	transaction(ping_frame, sizeof ping_frame);
	n = hal_drain(buf, sizeof buf);
	zassert_mem_equal(buf, ping_frame, sizeof ping_frame,
	                  "legacy framing restored (PING byte-identical)");
}

/* The 4-bit stamp wraps 15 -> 0 -> 1; stamp 0 is a VALID value mid-
 * session (host-side inequality still detects staleness across it). */
ZTEST(gd32_bridge_transport, test_status_seq_wraps_mod_16)
{
	uint8_t buf[80];

	transport_spi_init();
	negotiate(0x01u);
	(void)hal_drain(buf, sizeof buf); /* stamp 1 consumed */

	/* 14 more decodes take the stamp to 15. */
	for (int i = 0; i < 14; i++) {
		transaction(ping_frame, sizeof ping_frame);
		(void)hal_drain(buf, sizeof buf);
	}
	transaction(ping_frame, sizeof ping_frame);
	(void)hal_drain(buf, sizeof buf);
	zassert_equal(buf[1] >> 4, 0x0u, "stamp wrapped to 0");

	transaction(ping_frame, sizeof ping_frame);
	(void)hal_drain(buf, sizeof buf);
	zassert_equal(buf[1] >> 4, 0x1u, "and keeps counting");

	negotiate(0x00u); /* leave the suite in legacy framing */
	(void)hal_drain(buf, sizeof buf);
}

ZTEST_SUITE(gd32_bridge_transport, NULL, NULL, NULL, NULL, NULL);
