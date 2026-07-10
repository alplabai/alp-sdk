/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file i2c.h
 * @brief Injection API for the I2C test double (priority-255 backend).
 *
 * `src/backends/i2c/testing_drv.c` registers a `silicon_ref="*"`
 * backend at priority 255 (see @ref ALP_BACKEND_REGISTER), so with
 * `CONFIG_ALP_SDK_TESTING_I2C=y` it wins @ref alp_backend_select for
 * every bus id and the portable `<alp/peripheral.h>` `alp_i2c_*`
 * CONTROLLER-mode API rides on it transparently -- no real or emulated
 * I2C controller needed.  This header is the test-side control
 * surface: it models the DEVICE the app-under-test's controller talks
 * to on the bus -- canned read responses per 7-bit address, one-shot
 * bus faults (@ref alp_testing_bus_fault_t, `<alp/testing/common.h>`),
 * and read-back of what the app last wrote.
 *
 * This double implements only the CONTROLLER-mode ops
 * (`alp_i2c_open`/`write`/`read`/`write_read`/`close`); it leaves
 * `target_open`/`target_close` NULL, so @ref alp_i2c_target_open fails
 * @ref ALP_ERR_NOSUPPORT on it -- this double simulates the OTHER
 * device on the bus, not this MCU's own target (slave) mode.
 *
 * Every function keys off the same @p bus_id the app passes to
 * @ref alp_i2c_open, plus a 7-bit @p addr identifying which simulated
 * device on that bus the call concerns -- a single bus can host
 * several canned devices at once, exactly like a real I2C segment.
 * The injectors (@ref alp_testing_i2c_target_respond,
 * @ref alp_testing_i2c_fail_next) are create-on-first-touch -- a test
 * may prime a device's response or arm a fault BEFORE the app opens
 * the bus.  @ref alp_testing_i2c_last_write is the exception: it
 * reports on a (bus_id, addr) pair that has been touched at least once
 * and fails @ref ALP_ERR_INVAL for one that never has -- there is
 * nothing yet to read back.
 *
 * @note `alp_i2c_open()` on this double ALWAYS succeeds (a deliberate
 *       ergonomic choice, mirroring the GPIO/UART doubles' `open()` --
 *       see `testing_drv.c`'s open()), so an app under test that opens
 *       a genuinely-invalid bus id still gets back a live handle: this
 *       double cannot catch a wrong-bus application bug. Proving
 *       open() rejects an invalid instance is the real backend's
 *       conformance job (see the invalid-instance case in
 *       `tests/zephyr/conformance/src/main.c`), not this one's.
 *
 * @note The canned read response set by @ref alp_testing_i2c_target_respond
 *       is NOT a consumed queue (unlike the UART double's RX FIFO): it
 *       behaves like a device register snapshot and is returned, in
 *       full or truncated to the caller's requested length, by every
 *       @ref alp_i2c_read / read phase of @ref alp_i2c_write_read until
 *       replaced by another `..._target_respond()` call or cleared by
 *       @ref alp_testing_reset_all. A request longer than the canned
 *       response is zero-padded.
 */

#ifndef ALP_TESTING_I2C_H
#define ALP_TESTING_I2C_H

#include <stddef.h>
#include <stdint.h>

#include <alp/peripheral.h>
#include <alp/testing/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the canned response @p addr on @p bus_id returns to the
 *        next (and every following, until replaced) @ref alp_i2c_read
 *        or the read phase of @ref alp_i2c_write_read.
 *
 * @param[in] bus_id  The same id the app passes to @ref alp_i2c_open.
 * @param[in] addr    7-bit address of the simulated device.
 * @param[in] rsp     Response bytes; copied, so the caller's buffer
 *                     need not outlive this call.
 * @param[in] len     Byte count.  Must fit the double's fixed
 *                     per-device response capacity (see
 *                     `testing_drv.c`) -- longer register files are
 *                     out of this double's scope.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p rsp is NULL with
 *         @p len > 0; ALP_ERR_NOMEM if @p len exceeds the response
 *         capacity or the bus's simulated-device table is full.
 */
alp_status_t
alp_testing_i2c_target_respond(uint32_t bus_id, uint8_t addr, const uint8_t *rsp, size_t len);

/**
 * @brief Arm a one-shot bus fault for the NEXT transfer that touches
 *        (@p bus_id, @p addr).
 *
 * Fires exactly once -- on the next @ref alp_i2c_write,
 * @ref alp_i2c_read, or @ref alp_i2c_write_read addressed to @p addr on
 * @p bus_id -- then disarms itself automatically. See
 * @ref alp_testing_bus_fault_t (`<alp/testing/common.h>`) for what
 * each fault kind does to the transfer and which @ref alp_status_t it
 * surfaces.
 *
 * @param[in] bus_id     The same id the app passes to @ref alp_i2c_open.
 * @param[in] addr       7-bit address of the simulated device.
 * @param[in] f          Fault kind.
 * @param[in] short_len  Bytes actually transferred before the fault,
 *                        for @ref ALP_TESTING_FAULT_SHORT. Ignored for
 *                        the other fault kinds.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p f is not a valid
 *         @ref alp_testing_bus_fault_t; ALP_ERR_NOMEM if the bus's
 *         simulated-device table is full.
 */
alp_status_t alp_testing_i2c_fail_next(uint32_t                bus_id,
                                       uint8_t                 addr,
                                       alp_testing_bus_fault_t f,
                                       size_t                  short_len);

/**
 * @brief Read back the bytes the app under test last wrote to @p addr
 *        on @p bus_id, via @ref alp_i2c_write or the write phase of
 *        @ref alp_i2c_write_read.
 *
 * Reports the single most recent write (not a drained queue, unlike
 * the UART double's TX capture ring) -- a second call without an
 * intervening write returns the same bytes again.
 *
 * @param[in]  bus_id  The same id the app passes to @ref alp_i2c_open.
 * @param[in]  addr    7-bit address of the simulated device.
 * @param[out] out     Destination buffer.  Must be non-NULL if
 *                       @p cap > 0.
 * @param[in]  cap     Capacity of @p out, in bytes.
 * @param[out] got     Receives the number of bytes actually copied
 *                       (`min(cap, bytes captured by the last write)`).
 *                       Must be non-NULL.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p got is NULL, @p out is
 *         NULL with @p cap > 0, or (@p bus_id, @p addr) has never been
 *         touched (no open-and-write, no injection).
 */
alp_status_t
alp_testing_i2c_last_write(uint32_t bus_id, uint8_t addr, uint8_t *out, size_t cap, size_t *got);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_I2C_H */
