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

/* @p addr is the 7-bit address to poll -- ctx->addr (0x50 range) after a
 * main-array write, or ctx->addr + EEPROM_24C128_ALT_ADDR_OFFSET (0x58
 * range) after a Secure Data Page write.  Both headers decode onto the
 * same physical die, so either address-only write NACKs while an internal
 * write cycle from EITHER header is still in progress -- but polling the
 * header actually written keeps this function's contract independent of
 * that assumption. */
static alp_status_t poll_for_ack_at(eeprom_24c128_t *ctx, uint8_t addr)
{
	/* Acknowledge polling: try a 0-byte (address-only) write at the device's
     * address; if the chip is still finishing an internal write cycle it NACKs.
     * Loop until ACK or budget exhausted, pacing each retry so the loop spans
     * the chip's write cycle rather than racing past it. */
	for (int i = 0; i < EEPROM_WRITE_POLL_MAX; ++i) {
		uint8_t      addr_buf[2] = { 0, 0 };
		alp_status_t s           = alp_i2c_write(ctx->bus, addr, addr_buf, 2);
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

/* Second device-select header (`1011`) selector bytes -- see the table on
 * EEPROM_24C128_ALT_ADDR_OFFSET's doc comment.  Only bits A10/A9 (bits 2/1
 * of the first address byte) matter; the second pointer byte is always 0
 * (each object is smaller than 256 bytes, so it never needs a nonzero
 * low byte). */
#define EEPROM_IDENTITY_SEL_SECURE_PAGE   0x00u
#define EEPROM_IDENTITY_SEL_UNIQUE_ID     0x02u
#define EEPROM_IDENTITY_SEL_LOCK_STATUS   0x04u
#define EEPROM_IDENTITY_SEL_DEVICE_CONFIG 0x06u

alp_status_t eeprom_24c128_read_identity(eeprom_24c128_t *ctx, eeprom_24c128_identity_t *out)
{
	if (ctx == NULL || out == NULL) return ALP_ERR_INVAL;
	if (!ctx->initialised) return ALP_ERR_NOT_READY;

	memset(out, 0, sizeof(*out));

	/* Same strapped A2/A1/A0 as the array, second device-select header --
     * see EEPROM_24C128_ALT_ADDR_OFFSET's doc comment. Read-only: every
     * transfer below is a 2-byte pointer write followed by a
     * repeated-start read, never a data write -- see this function's
     * doc comment for why a write here is dangerous. */
	uint8_t alt_addr = (uint8_t)(ctx->addr + EEPROM_24C128_ALT_ADDR_OFFSET);
	uint8_t ptr[2];

	ptr[0] = EEPROM_IDENTITY_SEL_SECURE_PAGE;
	ptr[1] = 0x00;
	if (alp_i2c_write_read(
	        ctx->bus, alt_addr, ptr, sizeof(ptr), out->secure_page, sizeof(out->secure_page)) ==
	    ALP_OK) {
		out->secure_page_valid = true;
	}

	ptr[0] = EEPROM_IDENTITY_SEL_UNIQUE_ID;
	ptr[1] = 0x00;
	if (alp_i2c_write_read(
	        ctx->bus, alt_addr, ptr, sizeof(ptr), out->unique_id, sizeof(out->unique_id)) ==
	    ALP_OK) {
		out->unique_id_valid = true;
	}

	ptr[0]            = EEPROM_IDENTITY_SEL_LOCK_STATUS;
	ptr[1]            = 0x00;
	uint8_t lock_byte = 0;
	if (alp_i2c_write_read(ctx->bus, alt_addr, ptr, sizeof(ptr), &lock_byte, 1) == ALP_OK) {
		out->lock_valid         = true;
		out->secure_page_locked = (lock_byte & 0x02u) != 0u;
	}

	ptr[0] = EEPROM_IDENTITY_SEL_DEVICE_CONFIG;
	ptr[1] = 0x00;
	if (alp_i2c_write_read(ctx->bus, alt_addr, ptr, sizeof(ptr), &out->device_config, 1) ==
	    ALP_OK) {
		out->device_config_valid = true;
	}

	return ALP_OK;
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

		s = poll_for_ack_at(ctx, ctx->addr);
		if (s != ALP_OK) return s;

		offset = (uint16_t)(offset + chunk);
		data += chunk;
		len -= chunk;
	}
	return ALP_OK;
}

/* Compile-time proof that the write and lock selectors below can never
 * equal the Device Configuration Register selector (0x06) -- see
 * eeprom_24c128_secure_page_write's and eeprom_24c128_secure_page_lock's
 * doc comments in the header for why a write must never reach that
 * selector: it lands on the SWP bit and permanently write-protects the
 * array, this page, and the register together.  There is no separate
 * "op-code" byte in this wire format -- see the review note below -- so
 * this asserts the two literal selector values these functions hardcode. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(EEPROM_IDENTITY_SEL_SECURE_PAGE != EEPROM_IDENTITY_SEL_DEVICE_CONFIG,
               "secure-page write selector must never equal the device config selector");
_Static_assert(EEPROM_IDENTITY_SEL_LOCK_STATUS != EEPROM_IDENTITY_SEL_DEVICE_CONFIG,
               "secure-page lock selector must never equal the device config selector");
#endif

alp_status_t eeprom_24c128_secure_page_write(eeprom_24c128_t *ctx, const uint8_t *data, size_t len)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (data == NULL) return ALP_ERR_INVAL;
	/* Reject any length but the exact page size before it reaches the bus --
     * a caller-suppliable `size_t` here (not a fixed-size array parameter,
     * which decays to a pointer and would silently trust a too-short
     * buffer) is what makes it structurally impossible for a short buffer
     * to have this function memcpy past its end into a page that then gets
     * permanently sealed. */
	if (len != EEPROM_24C128_SECURE_PAGE_BYTES) return ALP_ERR_INVAL;

	uint8_t alt_addr = (uint8_t)(ctx->addr + EEPROM_24C128_ALT_ADDR_OFFSET);

	/* {sel, in-page offset} + 64 data bytes -- the SAME 2-byte pointer
     * convention eeprom_24c128_read_identity() already uses (bench-verified)
     * to read this same object, not a 3-byte "selector + op-code + offset"
     * scheme: docs/som-batch-provisioning-procedure.md §7's "second byte"
     * phrasing counts the I2C device-address byte as the first byte, so its
     * "xxxx x00x" IS this function's FIRST (and only) pointer byte, matching
     * EEPROM_IDENTITY_SEL_SECURE_PAGE exactly -- there is no separate op-code
     * byte.  An earlier revision of this function got that wrong, emitted a
     * 3-byte pointer, and rotated every byte of the page by one on write. */
	uint8_t scratch[2 + EEPROM_24C128_SECURE_PAGE_BYTES];
	scratch[0] = EEPROM_IDENTITY_SEL_SECURE_PAGE;
	scratch[1] = 0x00;
	memcpy(scratch + 2, data, EEPROM_24C128_SECURE_PAGE_BYTES);

	alp_status_t s = alp_i2c_write(ctx->bus, alt_addr, scratch, sizeof(scratch));
	if (s != ALP_OK) return s;

	return poll_for_ack_at(ctx, alt_addr);
}

alp_status_t eeprom_24c128_secure_page_lock(eeprom_24c128_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	uint8_t alt_addr = (uint8_t)(ctx->addr + EEPROM_24C128_ALT_ADDR_OFFSET);

	/* {sel, 0x00, 0xFF} -- selector 0x04 (Secure Page Lock Status, the SAME
     * first-pointer-byte selector eeprom_24c128_read_identity() reads),
     * second pointer byte 0x00, then the single 0xFF trigger byte per
     * docs/som-batch-provisioning-procedure.md §7 step 7.  Not a
     * "sel + op-code + don't-care + data" 4-byte frame -- see
     * eeprom_24c128_secure_page_write's comment above for why there is no
     * separate op-code byte in this wire format. */
	uint8_t cmd[3] = {
		EEPROM_IDENTITY_SEL_LOCK_STATUS,
		0x00,
		0xFFu,
	};
	alp_status_t s = alp_i2c_write(ctx->bus, alt_addr, cmd, sizeof(cmd));
	if (s != ALP_OK) return s;

	/* Deliberately NOT poll_for_ack_at(): that polls by issuing an
     * address-only WRITE to the just-locked page, and this repo's own
     * quoted datasheet text is "Any write instructions to the Secure Data
     * Page will return No ACK from the device" -- so on a CORRECT lock,
     * every poll attempt NACKs, the loop exhausts its budget, and this
     * function would report ALP_ERR_TIMEOUT for the success case, with the
     * header telling the caller not to retry a timeout here.  Wait out the
     * same write-cycle budget once instead, with no further write to the
     * page, then read Lock Status -- the only verdict this function needs. */
	alp_delay_us(EEPROM_WRITE_POLL_STEP_US * (uint32_t)EEPROM_WRITE_POLL_MAX);

	/* Confirm, don't trust: re-read Lock Status rather than trusting the
     * write's own ACK -- the only safe state check (see
     * eeprom_24c128_read_identity's doc comment for why the datasheet's
     * other method, attempting a page write and checking for a NAK, must
     * never be used). */
	uint8_t ptr[2]    = { EEPROM_IDENTITY_SEL_LOCK_STATUS, 0x00 };
	uint8_t lock_byte = 0;
	s                 = alp_i2c_write_read(ctx->bus, alt_addr, ptr, sizeof(ptr), &lock_byte, 1);
	if (s != ALP_OK) return s;
	if ((lock_byte & 0x02u) == 0u) return ALP_ERR_IO; /* wrote OK but didn't take */

	return ALP_OK;
}

void eeprom_24c128_deinit(eeprom_24c128_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
