/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic 24Cxx-class 128-Kbit I2C EEPROM driver.
 * Covers Onsemi N24S128 + STMicro M24128 (footprint-compatible).
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/eeprom_24c128.h"

/* Overflow-safe offset/len range check (#743) -- see alp_size_range_valid()'s
 * doc comment.  Was previously `(uint32_t)offset + len > EEPROM_24C128_BYTES`,
 * which computes the sum before comparing: a large enough `len` wraps the
 * addition and bypasses the check, letting an out-of-range offset/len pair
 * reach the I2C transfer / memcpy below (#738). */
#include "alp_checked_arith.h"

/** Maximum poll-cycles to wait for a write to complete.  The 24Cxx internal
 *  write cycle is up to 5 ms (datasheet t_WR); we pace each poll by ~1 ms so the
 *  total budget (~20 ms) comfortably spans it.  Bench-found 2026-06-15: without
 *  the inter-poll delay, 20 back-to-back I2C transactions complete in well under
 *  one write cycle and poll_for_ack returned ALP_ERR_TIMEOUT on every page write
 *  (the chip was still busy), so eeprom_24c128_write failed even though the data
 *  had been accepted -- validated on the E8 (I2C2). */
#define EEPROM_WRITE_POLL_MAX     20
#define EEPROM_WRITE_POLL_STEP_US 1000u

static alp_status_t poll_for_ack(eeprom_24c128_t *ctx)
{
	/* Acknowledge polling: try a 0-byte (address-only) write at the device's
     * address; if the chip is still finishing an internal write cycle it NACKs.
     * Loop until ACK or budget exhausted, pacing each retry so the loop spans
     * the chip's write cycle rather than racing past it. */
	for (int i = 0; i < EEPROM_WRITE_POLL_MAX; ++i) {
		uint8_t      addr_buf[2] = { 0, 0 };
		alp_status_t s           = alp_i2c_write(ctx->bus, ctx->addr, addr_buf, 2);
		if (s == ALP_OK) return ALP_OK;
		alp_delay_us(EEPROM_WRITE_POLL_STEP_US);
	}
	return ALP_ERR_TIMEOUT;
}

alp_status_t eeprom_24c128_init(eeprom_24c128_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (addr_7bit < 0x50 || addr_7bit > 0x57) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus  = bus;
	ctx->addr = addr_7bit;

	/* Probe by reading 1 byte at offset 0. */
	uint8_t      addr_buf[2] = { 0, 0 };
	uint8_t      scratch     = 0;
	alp_status_t s = alp_i2c_write_read(bus, addr_7bit, addr_buf, sizeof(addr_buf), &scratch, 1);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t eeprom_24c128_read(eeprom_24c128_t *ctx, uint16_t offset, uint8_t *out, size_t len)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out == NULL && len > 0) return ALP_ERR_INVAL;
	if (!alp_size_range_valid((size_t)offset, len, EEPROM_24C128_BYTES)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	if (len == 0) return ALP_OK;

	uint8_t addr_buf[2] = { (uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF) };
	return alp_i2c_write_read(ctx->bus, ctx->addr, addr_buf, sizeof(addr_buf), out, len);
}

alp_status_t
eeprom_24c128_write(eeprom_24c128_t *ctx, uint16_t offset, const uint8_t *data, size_t len)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	if (!alp_size_range_valid((size_t)offset, len, EEPROM_24C128_BYTES)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	if (len == 0) return ALP_OK;

	/* Split at page boundaries.  Each chunk = (offset, payload),
     * sent as one I2C transaction. */
	while (len > 0) {
		size_t  page_remaining = EEPROM_24C128_PAGE_BYTES - (offset % EEPROM_24C128_PAGE_BYTES);
		size_t  chunk          = (len < page_remaining) ? len : page_remaining;
		uint8_t scratch[2 + EEPROM_24C128_PAGE_BYTES];
		scratch[0] = (uint8_t)(offset >> 8);
		scratch[1] = (uint8_t)(offset & 0xFF);
		memcpy(scratch + 2, data, chunk);
		alp_status_t s = alp_i2c_write(ctx->bus, ctx->addr, scratch, chunk + 2);
		if (s != ALP_OK) return s;

		s = poll_for_ack(ctx);
		if (s != ALP_OK) return s;

		offset = (uint16_t)(offset + chunk);
		data += chunk;
		len -= chunk;
	}
	return ALP_OK;
}

void eeprom_24c128_deinit(eeprom_24c128_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
