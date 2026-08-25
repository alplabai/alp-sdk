/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: SPI-slave transport (the DEFAULT host-control
 * link).
 *
 * Wire framing (see <alp/protocol/cc3501e.h>): a 4-byte little-endian
 * header followed by the payload -- NO start-of-frame byte and NO CRC
 * (the channel is a short, hardwired, point-to-point SPI bus, not a
 * noisy shared line):
 *
 *   REQUEST : cmd | flags | payload_len(LE16) | payload[payload_len]
 *   REPLY   : cmd | flags | payload_len(LE16) | status | data[...]
 *
 * Request and reply ride SEPARATE SPI transactions: the firmware stages
 * the reply on CS-high (request complete) and the host reads it back in
 * a follow-up transaction once the firmware raises its READY line (the
 * v0.x bring-up handshake; see chips/cc3501e/cc3501e.c).
 *
 * THIS FILE is SILICON-FREE: byte-stream staging + the
 * protocol_build_reply() hand-off only.  All framing/dispatch lives in
 * protocol.c so SPI and SDIO stay byte-identical.  The TI SimpleLink /
 * driverlib SPI-slave init + ISR wiring lives in the ti backend
 * (hal/ti/transport_hw_ti_spi.c) and drives the spi_slave_*() seams; the
 * stub backend leaves the bring-up hook a no-op so the host unit test
 * can feed the same seams directly.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"
#include "transport.h"

/* Weak default: the stub backend links this no-op so it needs no TI
 * SDK.  The ti backend's hal/ti/transport_hw_ti_spi.c overrides it with
 * the real SPI0 slave + CS bring-up (the CC3501E's SPI0; Alif = SPI1 master). */
__attribute__((weak)) void bridge_transport_spi_hw_init(void)
{
}

/* Weak default for the post-Wlan_Start slave re-arm (the ti backend's
 * Wlan_Start DMA-coexistence fix).  The stub / silicon-free host build never
 * reaches it (only the ti HAL, hal/ti/cc3501e_hw_ti.c, calls it), but link a
 * no-op default so the symbol resolves on any non-ti backend.  The ti backend
 * (hal/ti/transport_hw_ti_spi.c) overrides it with the real SPI_close + reopen
 * + re-arm. */
__attribute__((weak)) void bridge_transport_spi_hw_reinit(void)
{
}

__attribute__((weak)) bool bridge_transport_spi_is_dead(void)
{
	return false; /* no real SPI slave on this backend -- never dead */
}

/* Weak default for the bridge stand-down during a HIF-re-arbitrating radio op (BLE
 * enable).  The ti backend (hal/ti/transport_hw_ti_spi.c) overrides it with the real
 * SPI_transferCancel + SPI_close; no-op on any non-ti backend. */
__attribute__((weak)) void bridge_transport_spi_hw_suspend(void)
{
}

/* Weak defaults for OTA update mode (see transport.h).  The stub / silicon-free
 * host build has no persisted-flag RAM and no polled SPI, so it is ALWAYS in the
 * normal mode; the ti backend (hal/ti/transport_hw_ti_spi.c) overrides all four
 * with the real .TI.noinit-backed implementation.
 *
 * These are NOT optional: src/protocol_ota.c calls bridge_transport_spi_polled() /
 * _request_polled_boot() / _clear_polled_boot() and src/protocol_diag.c calls
 * _boot_mark(), and tests/zephyr/cc3501e_bridge_transport links those TUs with no
 * hal/ti/ TU at all -- without these the suite does not link. */
/* transport.h promises a weak default for all THREE of suspend/release/quiesce.
 * Only _hw_suspend had one, so any non-ti translation unit that called release or
 * quiesce failed to link.  Both are no-ops on a backend that never claims the
 * bus, exactly as _hw_suspend above is. */
__attribute__((weak)) void bridge_transport_spi_hw_release(void)
{
}

__attribute__((weak)) void bridge_transport_spi_hw_quiesce(bool on)
{
	(void)on;
}

__attribute__((weak)) bool bridge_transport_spi_polled(void)
{
	return false; /* no persisted flag on this backend -- never update mode */
}

__attribute__((weak)) bool bridge_transport_spi_request_polled_boot(void)
{
	return false; /* no persisted flag on this backend -- nothing to arm */
}

__attribute__((weak)) bool bridge_transport_spi_clear_polled_boot(void)
{
	return false;
}

__attribute__((weak)) uint8_t bridge_transport_spi_boot_mark(void)
{
	return 0u;
}

/* Receive-side staging buffer (filled byte-by-byte by the ISR). */
static uint8_t spi_rx_buf[CC3501E_FRAME_MAX_BYTES];
static size_t  spi_rx_len;

/* Reply staging buffer + drain cursor.  Built on CS-high; clocked back
 * to the host on the follow-up read transaction. */
static uint8_t spi_tx_buf[CC3501E_FRAME_MAX_BYTES];
static size_t  spi_tx_len;
static size_t  spi_tx_cursor;

/* --------------------------------------------------------------- */
/* Seams (testable; HW-side wiring lives in the ti backend)          */
/* --------------------------------------------------------------- */

void spi_slave_cs_low(void)
{
	spi_rx_len = 0u;
}

void spi_slave_rx_byte(uint8_t b)
{
	if (spi_rx_len < sizeof(spi_rx_buf)) {
		spi_rx_buf[spi_rx_len++] = b;
	}
	/* Else: silently drop -- protocol_build_reply() sees the captured
     * count exceed the declared payload and replies RESP_ERR_PROTOCOL. */
}

void spi_slave_cs_high(void)
{
	/* Empty transaction: the host toggled CS without sending a request
     * (e.g. a bare reply-read poll before the READY handshake).  Leave
     * the staged reply intact and rewind its cursor so the read
     * re-serves it from the top. */
	if (spi_rx_len == 0u) {
		spi_tx_cursor = 0u;
		return;
	}
	spi_tx_len    = protocol_build_reply(spi_rx_buf, spi_rx_len, spi_tx_buf, sizeof(spi_tx_buf));
	spi_tx_cursor = 0u;
}

uint8_t spi_slave_tx_next_byte(void)
{
	if (spi_tx_cursor < spi_tx_len) {
		return spi_tx_buf[spi_tx_cursor++];
	}
	return 0xFFu; /* idle pattern once the reply is drained */
}

bool spi_slave_tx_pending(void)
{
	return spi_tx_cursor < spi_tx_len;
}

size_t spi_slave_tx_take(uint8_t *dst, size_t cap)
{
	if (dst == NULL || cap == 0u) {
		return 0u;
	}
	size_t n = (size_t)(spi_tx_len - spi_tx_cursor);

	if (n > cap) {
		n = cap;
	}
	memcpy(dst, &spi_tx_buf[spi_tx_cursor], n);
	spi_tx_cursor += (uint16_t)n;
	return n;
}

void transport_spi_init(void)
{
	spi_rx_len    = 0u;
	spi_tx_len    = 0u;
	spi_tx_cursor = 0u;
	/* SPI0 slave bring-up lives in the ti HAL backend
     * (hal/ti/transport_hw_ti_spi.c); the stub backend's weak no-op
     * keeps this hardware-free for host-side protocol tests. */
	bridge_transport_spi_hw_init();
}

/* Weak default for backends without the TI polled phase machine. */
__attribute__((weak)) uint8_t bridge_transport_spi_phase(void)
{
	return 0u;
}
