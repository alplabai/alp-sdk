/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file eeprom_24c128.h
 * @brief Generic 24Cxx-class 128-Kbit (16 KB) I2C EEPROM driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Covers the two footprint-compatible variants populated on the
 * E1M-AEN module: **N24S128C4DYT3G** (Onsemi, default) and
 * **M24128-BFMH6TG** (STMicro, alternate / DNP).  Both speak the
 * same 24Cxx wire protocol with a 2-byte memory-address pointer,
 * 64-byte page write, 5 ms typical write cycle, and a 7-bit
 * I2C address strapped via A0/A1/A2 in the 0x50..0x57 range
 * (default 0x50 on E1M-AEN).
 *
 * The driver is part-agnostic -- any 24C128 / AT24C128 /
 * 24LC128 also works.  Larger pin-compatible variants (24C256,
 * 24C512) need their own size constant; v0.3.x adds a
 * `device_size` config parameter.
 */

#ifndef ALP_CHIPS_EEPROM_24C128_H
#define ALP_CHIPS_EEPROM_24C128_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EEPROM_24C128_BYTES        (16u * 1024u) /**< Total capacity: 128 Kbit = 16 KB. */
#define EEPROM_24C128_PAGE_BYTES   64u           /**< Page-write granularity (datasheet). */
#define EEPROM_24C128_I2C_ADDR_LOW 0x50u         /**< 7-bit base address (A2:A1:A0 = 000). */

/** @brief 24C128 EEPROM driver context. Bound to a single device on one I2C bus. */
typedef struct {
	bool       initialised; /**< True once eeprom_24c128_init() probed the device. */
	alp_i2c_t *bus;         /**< Borrowed I2C bus handle (caller-owned, kept open). */
	uint8_t    addr;        /**< 7-bit I2C address in the 0x50..0x57 range. */
} eeprom_24c128_t;

/**
 * @brief Bind a context and probe the EEPROM for presence.
 *
 * Issues a 1-byte read at offset 0; an ACK confirms the device is alive. The
 * @p bus handle is borrowed, not owned — keep it open for the context lifetime.
 *
 * @param ctx        Output: caller-allocated context to populate.
 * @param bus        Open I2C bus the EEPROM sits on.
 * @param addr_7bit  7-bit I2C address (0x50..0x57, strapped via A0/A1/A2).
 * @return `ALP_OK` if the device ACKs; an `ALP_ERR_*` status otherwise.
 */
alp_status_t eeprom_24c128_init(eeprom_24c128_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/**
 * @brief Read @p len bytes starting at @p offset.
 *
 * Reads can cross page boundaries freely (the chip handles that
 * internally).  @p offset + @p len must not exceed
 * @ref EEPROM_24C128_BYTES.
 *
 * @param ctx     Initialised context.
 * @param offset  Byte address to start reading from (0 .. capacity-1).
 * @param out     Destination buffer of at least @p len bytes.
 * @param len     Number of bytes to read.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a bus error or
 *         out-of-range request.
 */
alp_status_t eeprom_24c128_read(eeprom_24c128_t *ctx, uint16_t offset, uint8_t *out, size_t len);

/**
 * @brief Write @p len bytes starting at @p offset.
 *
 * Writes are split into 64-byte page-aligned chunks because the
 * chip's internal buffer wraps at the page boundary -- a write
 * that straddles a boundary would alias the high bytes back to
 * the page start.  Each page write is followed by a polling-ACK
 * cycle that waits for the internal write to complete (max
 * 5 ms typical, 10 ms worst-case per datasheet).
 *
 * Caller is responsible for not exceeding the device's
 * write-endurance budget (10^6 writes per byte typical).
 *
 * @param ctx     Initialised context.
 * @param offset  Byte address to start writing at (0 .. capacity-1).
 * @param data    Source buffer of at least @p len bytes.
 * @param len     Number of bytes to write.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a bus error or
 *         out-of-range request. Blocks until each page's write cycle completes.
 */
alp_status_t
eeprom_24c128_write(eeprom_24c128_t *ctx, uint16_t offset, const uint8_t *data, size_t len);

/**
 * @brief Release the driver context. Idempotent; NULL @p ctx is tolerated.
 *
 * Clears the bound bus reference but does not close it — the caller owns the
 * I2C handle.
 *
 * @param ctx  Context to release (may be NULL).
 */
void eeprom_24c128_deinit(eeprom_24c128_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_EEPROM_24C128_H */
