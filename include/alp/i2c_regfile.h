/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file i2c_regfile.h
 * @brief Register-file I2C target (slave) helper over <alp/peripheral.h>.
 *
 * The byte-granular target callbacks in <alp/peripheral.h> mirror the
 * wire protocol, and almost every target application layers the same
 * "register-mapped peripheral" state machine on top of them: the first
 * byte the external controller writes after a (re)START latches a
 * register pointer, further written bytes store into a register file
 * with auto-increment, and a (repeated-START) read streams the file
 * from the latched pointer.  This helper ships that state machine once
 * so applications stop re-pasting it:
 *
 *   - controller write, byte 0   -> latches the register pointer
 *     (taken modulo the file length, EEPROM-style wraparound)
 *   - controller write, byte 1.. -> stores into the backing buffer at
 *     the pointer, auto-increment with wraparound
 *   - controller read            -> streams the backing buffer from the
 *     pointer, auto-increment with wraparound
 *   - STOP                       -> re-arms "next written byte is the
 *     pointer" for the following transaction
 *
 * The backing buffer is caller-owned, so firmware publishes state by
 * plain (volatile) stores into it and observes controller writes by
 * reading it back -- no extra API between the ISR-context callbacks
 * and the application thread.
 *
 * Availability tracks @ref alp_i2c_target_open exactly: the helper is a
 * pure layer over the portable target API and degrades with the same
 * status codes (ALP_ERR_NOSUPPORT on backends/drivers without target
 * mode, ALP_ERR_NOT_READY when the bus alias is unset).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.9 new, tracks the [ABI-EXPERIMENTAL] alp_i2c_target_* surface
 *      it wraps.  See docs/abi-markers.md.
 */

#ifndef ALP_I2C_REGFILE_H
#define ALP_I2C_REGFILE_H

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h" /* alp_status_t + the target API this wraps */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque register-file target handle.
 *
 * Obtained from @ref alp_i2c_regfile_open, released with
 * @ref alp_i2c_regfile_close.
 */
typedef struct alp_i2c_regfile alp_i2c_regfile_t;

/**
 * @brief Bus-traffic counters for bench observability.
 *
 * Filled by @ref alp_i2c_regfile_stats.  Both counters start at zero
 * at open and saturate only by uint32_t wraparound; they are updated
 * from ISR context, so a snapshot taken mid-transaction may be one
 * byte stale -- fine for "is the controller talking to us at all".
 */
typedef struct {
	uint32_t writes_seen; /**< Payload bytes received (register-pointer bytes excluded). */
	uint32_t reads_seen;  /**< Bytes streamed out to the controller. */
} alp_i2c_regfile_stats_t;

/**
 * @brief Expose @p regs as a register-mapped I2C target on @p bus_id.
 *
 * Registers this MCU on the bus at @p own_addr_7bit (via
 * @ref alp_i2c_target_open) and runs the classic register-file state
 * machine over the caller-owned buffer.  The controller-visible
 * register pointer starts at 0 and wraps modulo @p len on every
 * access, so the controller can never index outside the buffer.
 *
 * The callbacks start firing as soon as this returns; prime @p regs
 * with the state the controller should see (ID register, defaults)
 * BEFORE calling.  The buffer is written from ISR context -- declare
 * it `volatile` when the application thread polls it.
 *
 * The whole file is controller-writable by default; carve out
 * read-only registers with @ref alp_i2c_regfile_set_write_window.
 *
 * @param[in]  bus_id         Studio-resolved bus instance id
 *                            (same id space as @ref alp_i2c_open).
 * @param[in]  own_addr_7bit  Address this target answers on (0x08..0x77).
 * @param[in]  regs           Caller-owned register file backing buffer.
 *                            Must stay valid until close.
 * @param[in]  len            Register count (bytes) in @p regs; >= 1.
 * @param[out] out            Receives the open handle on ALP_OK, NULL
 *                            on any error.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL on NULL @p regs / @p out,
 *         @p len == 0 or an out-of-range address; ALP_ERR_NOMEM when
 *         the helper (or target) handle pool is exhausted; otherwise
 *         the @ref alp_i2c_target_open failure code (ALP_ERR_NOSUPPORT
 *         when the backend or controller driver lacks target mode,
 *         ALP_ERR_NOT_READY when the bus alias is unset).
 */
alp_status_t alp_i2c_regfile_open(uint32_t            bus_id,
                                  uint8_t             own_addr_7bit,
                                  volatile uint8_t   *regs,
                                  size_t              len,
                                  alp_i2c_regfile_t **out);

/**
 * @brief Restrict controller writes to @p count registers starting at
 *        @p first; everything outside the window becomes read-only.
 *
 * Out-of-window writes are dropped silently but the register pointer
 * still auto-increments, mirroring real silicon with read-only ID /
 * status registers.  @p count == 0 makes the whole file read-only.
 * Reads are never restricted.
 *
 * Call it right after @ref alp_i2c_regfile_open, before controller
 * traffic: the window is read from ISR context and is not updated
 * atomically against an in-flight transaction.
 *
 * @param[in] rf     Handle from @ref alp_i2c_regfile_open.
 * @param[in] first  First controller-writable register index.
 * @param[in] count  Number of controller-writable registers.
 *
 * @return ALP_OK; ALP_ERR_INVAL on NULL @p rf or a window that does
 *         not fit the register file.
 */
alp_status_t alp_i2c_regfile_set_write_window(alp_i2c_regfile_t *rf, size_t first, size_t count);

/**
 * @brief Snapshot the traffic counters ("is the controller talking?").
 *
 * @param[in]  rf   Handle from @ref alp_i2c_regfile_open.
 * @param[out] out  Receives the counter snapshot.
 *
 * @return ALP_OK; ALP_ERR_INVAL on NULL @p rf / @p out.
 */
alp_status_t alp_i2c_regfile_stats(const alp_i2c_regfile_t *rf, alp_i2c_regfile_stats_t *out);

/**
 * @brief Unregister the target and release the handle.  Idempotent on
 *        NULL.  No callback touches the backing buffer after this
 *        returns; the buffer stays caller-owned throughout.
 *
 * @param[in] rf  Handle from @ref alp_i2c_regfile_open, or NULL.
 */
void alp_i2c_regfile_close(alp_i2c_regfile_t *rf);

#ifdef __cplusplus
}
#endif

#endif /* ALP_I2C_REGFILE_H */
