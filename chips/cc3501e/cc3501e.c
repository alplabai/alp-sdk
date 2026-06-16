/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif-side host driver for the on-module TI CC3501E Wi-Fi 6 +
 * BLE 5.4 coprocessor.  See <alp/chips/cc3501e.h> for the public
 * lifecycle and <alp/protocol/cc3501e.h> for the wire protocol.
 *
 * Ships the call shape (init / reset / get_version / request /
 * set_event_callback / deinit) and the framing logic.  The actual
 * reset-pin pulse + WIFI.EN sequencing arrives once the EVK overlay
 * declares the Alif's P15_5 / P15_1_FLEX as GPIOs reachable via
 * alp_gpio_*; until then reset() returns NOSUPPORT cleanly.
 *
 * Wire framing matches the embedded firmware
 * (firmware/cc3501e/hal/ti/transport_hw_ti_spi.c): the current E1M-AEN
 * rev wires only SCLK/MOSI/MISO (no CS, no host IRQ -- both arrive next
 * rev), so a request/reply is clocked as four deterministic fixed-count
 * transfers in lockstep (request header, request payload, reply header,
 * reply payload) with a settle gap before the reply read.  The reply
 * payload's first byte is the response status (mapped via
 * resp_to_status); the data follows.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/cc3501e.h"
#include "alp/peripheral.h"

static void encode_header(uint8_t *frame, alp_cc3501e_cmd_t cmd, uint8_t flags,
                          uint16_t payload_len)
{
	frame[0] = (uint8_t)cmd;
	frame[1] = flags;
	frame[2] = (uint8_t)(payload_len & 0xFF);
	frame[3] = (uint8_t)((payload_len >> 8) & 0xFF);
}

static uint16_t decode_header_payload_len(const uint8_t *frame)
{
	return (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
}

alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus         = bus;
	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t cc3501e_reset(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ctx->reset_pin == NULL || ctx->enable_pin == NULL) {
		/* The studio's pin allocator (or hand-written firmware
         * via alp_gpio_open) must populate enable_pin / reset_pin
         * before reset() is meaningful.  Until then there's no
         * line to pulse. */
		return ALP_ERR_NOSUPPORT;
	}
	/* Reset sequence per TI SWRU626 §7.1.5 (CC3501E technical
     * reference manual):
     *
     *   1. Assert nRESET low while bringing rails down so the
     *      chip stays clamped through the supply transition.
     *   2. Drop WIFI_EN low; wait briefly for the rails to
     *      discharge (10us is comfortably above the rail RC).
     *   3. Raise WIFI_EN; wait ~5 ms for the supply ramps to
     *      stabilise (typical PMIC soft-start window).
     *   4. Hold nRESET low for >= 10 us per §7.1.5 after the
     *      supplies are valid.
     *   5. Release nRESET; wait the T1+T2+T3+T4 boot budget
     *      (~900 ms typical for BL1 + BL2 + Chain-of-Trust)
     *      before the first PING is meaningful.
     *
     * Total blocking time: ~905 ms.  Callers that don't want
     * the synchronous wait can call cc3501e_reset asynchronously
     * (kicked off from a worker thread) and poll via PING; v0.3.x
     * adds a non-blocking variant once the firmware's "boot done"
     * GPIO is wired. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	(void)alp_gpio_write(ctx->enable_pin, false);
	alp_delay_us(10u);
	(void)alp_gpio_write(ctx->enable_pin, true);
	alp_delay_ms(5u);
	/* nRESET stays low through the rail ramp; this assignment is
     * idempotent but kept explicit for clarity. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	alp_delay_us(10u);
	(void)alp_gpio_write(ctx->reset_pin, true);
	alp_delay_ms(900u);
	return ALP_OK;
}

/* Map a CC3501E response status byte (first reply-payload byte, per
 * <alp/protocol/cc3501e.h>) onto the SDK's alp_status_t. */
static alp_status_t resp_to_status(uint8_t resp)
{
	switch (resp) {
	case ALP_CC3501E_RESP_OK:
		return ALP_OK;
	case ALP_CC3501E_RESP_ERR_INVALID:
		return ALP_ERR_INVAL;
	case ALP_CC3501E_RESP_ERR_BUSY:
		return ALP_ERR_BUSY;
	case ALP_CC3501E_RESP_ERR_TIMEOUT:
		return ALP_ERR_TIMEOUT;
	case ALP_CC3501E_RESP_ERR_NO_MEM:
		return ALP_ERR_NOMEM;
	case ALP_CC3501E_RESP_ERR_NOT_READY:
		return ALP_ERR_NOT_READY;
	case ALP_CC3501E_RESP_ERR_VERSION:
		return ALP_ERR_VERSION;
	case ALP_CC3501E_RESP_ERR_RADIO:
	case ALP_CC3501E_RESP_ERR_PROTOCOL:
	case ALP_CC3501E_RESP_ERR_INTERNAL:
	default:
		return ALP_ERR_IO;
	}
}

alp_status_t cc3501e_request(cc3501e_t *ctx, alp_cc3501e_cmd_t cmd, const uint8_t *tx_payload,
                             size_t tx_len, uint8_t *rx_buf, size_t rx_cap, size_t *rx_len,
                             uint32_t timeout_ms)
{
	(void)timeout_ms; /* Reserved for a future IRQ-driven wait (next HW rev). */
	if (rx_len != NULL) *rx_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (tx_len > ALP_CC3501E_MAX_PAYLOAD) return ALP_ERR_INVAL;
	if (tx_payload == NULL && tx_len > 0) return ALP_ERR_INVAL;

	/*
     * 3-wire deterministic framing (this HW rev wires only SCLK/MOSI/MISO
     * -- no CS, no host IRQ; CS + IRQ arrive next rev).  Each transfer's
     * length is derived from a header already exchanged, so master + slave
     * stay in lockstep without a CS edge.  Matches the firmware SPI-slave
     * state machine in firmware/cc3501e/hal/ti/transport_hw_ti_spi.c.
     *
     *   1. send request header (4)        3. read reply header (4)
     *   2. send request payload (tx_len)  4. read reply payload (status+data)
     */
	encode_header(ctx->tx_scratch, cmd, ALP_CC3501E_FLAG_RESP_REQUIRED, (uint16_t)tx_len);
	alp_status_t s =
	    alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) return s;
	if (tx_len > 0) {
		s = alp_spi_transceive(ctx->bus, tx_payload, ctx->rx_scratch, tx_len);
		if (s != ALP_OK) return s;
	}

	/* Settle gap: let the slave dispatch + arm its reply before we read.
     * v0.1 META dispatch is instant; slow (Wi-Fi/BLE) replies + async
     * events need the next-rev host IRQ line, not a fixed gap. */
	alp_delay_us(200u);

	/* Dummies for the read transactions (MOSI is don't-care on a read). */
	memset(ctx->tx_scratch, 0xFF, sizeof(ctx->tx_scratch));

	/* 3. Reply header -> learn the reply payload length. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) return s;
	uint16_t resp_payload_len = decode_header_payload_len(ctx->rx_scratch);
	/* Every reply payload is status(1) + data; 0 or over-ceiling means the
     * slave wasn't ready or the lockstep desynced (no CS edge to recover). */
	if (resp_payload_len == 0u || resp_payload_len > ALP_CC3501E_MAX_PAYLOAD) {
		return ALP_ERR_IO;
	}

	/* 4. Reply payload: status byte followed by the response data. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, resp_payload_len);
	if (s != ALP_OK) return s;

	const uint8_t resp     = ctx->rx_scratch[0];
	const size_t  data_len = (size_t)resp_payload_len - 1u;
	if (data_len > 0u && rx_buf != NULL) {
		const size_t n = (data_len > rx_cap) ? rx_cap : data_len;
		memcpy(rx_buf, &ctx->rx_scratch[1], n);
		if (rx_len != NULL) *rx_len = n;
	}
	return resp_to_status(resp);
}

alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out)
{
	if (version_out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[2] = { 0 };
	size_t       got      = 0;
	alp_status_t s =
	    cc3501e_request(ctx, ALP_CC3501E_CMD_GET_VERSION, NULL, 0, reply, sizeof(reply), &got, 100);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	*version_out = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return ALP_OK;
}

alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	ctx->event_cb   = cb;
	ctx->event_user = user;
	return ALP_OK;
}

void cc3501e_deinit(cc3501e_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
