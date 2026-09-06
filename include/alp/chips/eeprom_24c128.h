/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file eeprom_24c128.h
 * @brief Generic 24Cxx-class 128-Kbit (16 KB) I2C EEPROM driver.
 *
 * @par Verification status: `eeprom_24c128_read_identity()`'s second-header
 *   objects are [BENCH-VERIFIED] -- measured 2026-09-06 on an E1M-AEN803
 *   (serial 2026W36-0003, SoC I2C2, Flow C RAM-run, labgrid place
 *   e1m-aen-evk-01): Unique ID / Lock Status / Device Configuration Register
 *   all read back at `0x58` and matched the datasheet delivery state, and
 *   `0x58` was proven distinct from the array read at `0x50`.  The rest of
 *   the driver (`eeprom_24c128_init/read/write/deinit`, the array read/write
 *   path) remains [UNTESTED] -- compiles + passes NULL-arg smokes only; treat
 *   those numbers + lifecycle sequencing as paper-correct until the v1.0
 *   verification sweep lands.
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

#define EEPROM_24C128_BYTES        (16u * 1024u) /* 128 Kbit = 16 KB */
#define EEPROM_24C128_PAGE_BYTES   64u           /* Datasheet page-write granularity */
#define EEPROM_24C128_I2C_ADDR_LOW 0x50u         /* A2:A1:A0 = 000 */

/** Onsemi N24S128 device-select header offset: `1010` (the 16 KB array,
 *  what @ref eeprom_24c128_read / @ref eeprom_24c128_write talk to) versus
 *  `1011` (Secure Data Page / Unique ID / Lock Status / Device Configuration
 *  Register, what @ref eeprom_24c128_read_identity talks to) differ only in
 *  that top nibble -- both headers share the same strapped A2/A1/A0, so the
 *  second header's 7-bit address is always the array's address + 0x08. */
#define EEPROM_24C128_ALT_ADDR_OFFSET   0x08u
#define EEPROM_24C128_SECURE_PAGE_BYTES 64u /* Lockable Secure Data Page */
#define EEPROM_24C128_UNIQUE_ID_BYTES   16u /* Factory-set Unique ID Number */

typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;
} eeprom_24c128_t;

/**
 * @brief The N24S128's four read-only second-header objects, read in one call.
 *
 * Per-field `_valid` flags rather than an all-or-nothing error: the
 * approved footprint-compatible second source (STMicro M24128-BFMH6TG,
 * DNP alternate) has no second device-select header at all, so on that
 * part every read here NACKs.  @ref eeprom_24c128_read_identity still
 * returns ::ALP_OK in that case, with every `_valid` flag false -- a board
 * populated with the alternate part is not an I/O failure, just an absent
 * capability.
 */
typedef struct {
	bool    secure_page_valid;
	uint8_t secure_page[EEPROM_24C128_SECURE_PAGE_BYTES]; /**< Lockable; erased = all 0xFF. */
	bool    unique_id_valid;
	/** Factory-set.  NOT uniformly random: bytes vary by silicon batch, but
	 *  a trailing run is a printable ASCII lot code (bench-observed: bytes
	 *  8..14 read `"7321261"` on a 2026W36 unit) -- callers must not treat
	 *  this as a uniform-random identifier. */
	uint8_t unique_id[EEPROM_24C128_UNIQUE_ID_BYTES];
	bool    lock_valid;
	bool    secure_page_locked; /**< Lock Status Read bit 1; 1 = locked (permanent). */
	bool    device_config_valid;
	uint8_t device_config; /**< A2/A1/A0 + SWP write-protect bit; delivery state 0x1D. */
} eeprom_24c128_identity_t;

/** @brief Probe the EEPROM (1-byte read at offset 0; ACK -> success). */
alp_status_t eeprom_24c128_init(eeprom_24c128_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/**
 * @brief Read @p len bytes starting at @p offset.
 *
 * Reads can cross page boundaries freely (the chip handles that
 * internally).  @p offset + @p len must not exceed
 * @ref EEPROM_24C128_BYTES.
 */
alp_status_t eeprom_24c128_read(eeprom_24c128_t *ctx, uint16_t offset, uint8_t *out, size_t len);

/**
 * @brief Read the N24S128's second-header identity objects (Secure Data
 *   Page, Unique ID, Lock Status, Device Configuration Register).
 *
 * READ-ONLY, deliberately: this function must never write a data byte to
 * the device.  A stray write at selector `0x06` lands in the Device
 * Configuration Register, whose low bits are the device's own A2/A1/A0
 * straps -- writing it moves the EEPROM off @p ctx's configured address --
 * and whose SWP bit permanently write-protects the array, the Secure Data
 * Page, and this register together.  For the same reason this function
 * only ever issues the Lock Status Read; it deliberately does NOT use the
 * datasheet's other lock-check method (attempt a Secure Data Page write
 * and see whether it ACKs), because that method is itself a write.
 * Locking the Secure Data Page is permanent and is intentionally NOT
 * offered here -- it belongs in the provisioning tool, behind a
 * cold-cycle read-back gate, not this driver.
 *
 * @param ctx  Initialised driver context (see @ref eeprom_24c128_init).
 * @param out  Destination.  Zeroed first, then filled per-field; see
 *   ::eeprom_24c128_identity_t's `_valid` flags -- a `_valid` field being
 *   false (with this function still returning ::ALP_OK) means that object
 *   NACKed, which is the expected result on a board populated with the
 *   footprint-compatible alternate part (no second header to answer).
 * @return ::ALP_ERR_INVAL if @p ctx or @p out is `NULL`;
 *   ::ALP_ERR_NOT_READY if @p ctx has not been initialised;
 *   ::ALP_OK otherwise, regardless of how many of the four objects
 *   answered.
 */
alp_status_t eeprom_24c128_read_identity(eeprom_24c128_t *ctx, eeprom_24c128_identity_t *out);

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
 */
alp_status_t
eeprom_24c128_write(eeprom_24c128_t *ctx, uint16_t offset, const uint8_t *data, size_t len);

/** @brief Release the driver context.  Idempotent. */
void eeprom_24c128_deinit(eeprom_24c128_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_EEPROM_24C128_H */
