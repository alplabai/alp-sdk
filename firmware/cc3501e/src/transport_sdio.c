/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: SDIO-slave transport (OPTIONAL, customer-
 * selectable; the higher-throughput alternative to the default SPI
 * link).
 *
 * Availability constraint: the Alif Ensemble has a SINGLE SDIO
 * controller, shared at board level with the micro-SD slot.  SDIO is
 * the host-control link to the CC3501E only on boards that route that
 * controller to the CC3501E instead of an SD card; when an SD card is
 * populated, SDIO is blocked and the link MUST fall back to SPI.  See
 * docs/cc3501e-bridge.md "Selectable host-control transport".
 *
 * Same wire frame as SPI -- SDIO just carries the [header | payload]
 * frame inside its data blocks instead of a raw byte stream.  Framing
 * and dispatch are shared via protocol_build_reply(), so SPI and SDIO
 * are byte-identical on the wire.
 *
 * THIS FILE is SILICON-FREE: it stages a reply frame from a received
 * request frame.  The TI SimpleLink SDIO-function / block transfer
 * wiring (the v0.x bench item) lives in hal/ti/transport_hw_ti_sdio.c
 * and drives the sdio_slave_*() seams below.
 */

#include <stdint.h>

#include "protocol.h"
#include "transport.h"

/* Weak default: the stub backend links this no-op so it needs no TI
 * SDK.  The ti backend overrides it with the real SDIO-slave bring-up. */
__attribute__((weak)) void bridge_transport_sdio_hw_init(void)
{
}

/* Staged reply for the most recent request frame. */
static uint8_t  sdio_reply_buf[CC3501E_FRAME_MAX_BYTES];
static uint16_t sdio_reply_len;

/* --------------------------------------------------------------- */
/* Seams (testable; HW-side wiring lives in the ti backend)          */
/* --------------------------------------------------------------- */

/* Called by the SDIO HW backend with a complete received request frame.
 * Builds + stages the reply frame for the backend to clock back. */
void sdio_slave_on_request(const uint8_t *frame, uint16_t len)
{
	const size_t n = protocol_build_reply(frame, len, sdio_reply_buf, sizeof(sdio_reply_buf));
	sdio_reply_len = (uint16_t)n;
}

const uint8_t *sdio_slave_reply(void)
{
	return sdio_reply_buf;
}

uint16_t sdio_slave_reply_len(void)
{
	return sdio_reply_len;
}

void transport_sdio_init(void)
{
	sdio_reply_len = 0u;
	bridge_transport_sdio_hw_init();
}
