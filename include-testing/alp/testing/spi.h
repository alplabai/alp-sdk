/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file spi.h
 * @brief Injection API for the SPI test double (priority-255 backend).
 *
 * `src/backends/spi/testing_drv.c` registers a `silicon_ref="*"`
 * backend at priority 255 (see @ref ALP_BACKEND_REGISTER), so with
 * `CONFIG_ALP_SDK_TESTING_SPI=y` it wins @ref alp_backend_select for
 * every bus id and the portable `<alp/peripheral.h>` `alp_spi_*`
 * CONTROLLER-mode API rides on it transparently -- no real or emulated
 * SPI controller needed.  This header is the test-side control
 * surface: it models the DEVICE the app-under-test's controller talks
 * to on the bus -- a canned MISO (device-to-controller) byte stream,
 * one-shot bus faults (@ref alp_testing_bus_fault_t,
 * `<alp/testing/common.h>`), and read-back of the last MOSI
 * (controller-to-device) bytes.
 *
 * This double implements only the CONTROLLER-mode ops
 * (`alp_spi_open`/`transceive`/`close`, which
 * `alp_spi_write`/`alp_spi_read` are thin wrappers over); it leaves
 * `target_open`/`target_transceive`/`target_close` NULL, so
 * @ref alp_spi_target_open fails @ref ALP_ERR_NOSUPPORT on it -- this
 * double simulates the OTHER device on the bus, not this MCU's own
 * target (slave) mode.
 *
 * Unlike I2C, a single SPI bus handle already identifies exactly one
 * device (selected by the handle's own chip-select at
 * @ref alp_spi_open time), so every function here keys off only
 * @p bus_id -- no address parameter. The injectors
 * (@ref alp_testing_spi_load_miso, @ref alp_testing_spi_fail_next) are
 * create-on-first-touch -- a test may prime a response or arm a fault
 * BEFORE the app opens the bus. @ref alp_testing_spi_last_mosi is the
 * exception: it reports on a @p bus_id that has been touched at least
 * once and fails @ref ALP_ERR_INVAL for one that never has.
 *
 * @note `alp_spi_open()` on this double ALWAYS succeeds (a deliberate
 *       ergonomic choice, mirroring the GPIO/UART/I2C doubles'
 *       `open()`), so an app under test that opens a genuinely-invalid
 *       bus id still gets back a live handle: this double cannot catch
 *       a wrong-bus application bug. Proving open() rejects an invalid
 *       instance is the real backend's conformance job (see the
 *       invalid-instance case in `tests/zephyr/conformance/src/main.c`),
 *       not this one's.
 *
 * @note @ref alp_spi_transceive is full-duplex: a NULL `tx` (the
 *       @ref alp_spi_read wrapper) means the caller does not care what
 *       goes out on MOSI, so this double leaves the last-MOSI capture
 *       untouched on such a call; a NULL `rx` (the @ref alp_spi_write
 *       wrapper) means the caller does not want MISO data, so this
 *       double does not touch the caller's (absent) buffer. The canned
 *       MISO stream set by @ref alp_testing_spi_load_miso is NOT a
 *       consumed queue: it behaves like a device register snapshot and
 *       is returned, in full or truncated/zero-padded to the
 *       transfer's length, by every transceive that supplies an `rx`
 *       buffer, until replaced or cleared by @ref alp_testing_reset_all.
 */

#ifndef ALP_TESTING_SPI_H
#define ALP_TESTING_SPI_H

#include <stddef.h>
#include <stdint.h>

#include <alp/peripheral.h>
#include <alp/testing/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the canned MISO bytes @p bus_id returns on the next (and
 *        every following, until replaced) transceive that supplies an
 *        `rx` buffer (@ref alp_spi_transceive / @ref alp_spi_read).
 *
 * @param[in] bus_id  The same id the app passes to @ref alp_spi_open.
 * @param[in] d       MISO bytes; copied, so the caller's buffer need
 *                      not outlive this call.
 * @param[in] len     Byte count.  Must fit the double's fixed MISO
 *                      capacity (see `testing_drv.c`).
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p d is NULL with
 *         @p len > 0; ALP_ERR_NOMEM if @p len exceeds the MISO capacity.
 */
alp_status_t alp_testing_spi_load_miso(uint32_t bus_id, const uint8_t *d, size_t len);

/**
 * @brief Arm a one-shot bus fault for the NEXT transceive on @p bus_id.
 *
 * Fires exactly once -- on the next @ref alp_spi_transceive (including
 * through the @ref alp_spi_write / @ref alp_spi_read wrappers) -- then
 * disarms itself automatically. See @ref alp_testing_bus_fault_t
 * (`<alp/testing/common.h>`) for what each fault kind does to the
 * transfer and which @ref alp_status_t it surfaces; NACK on SPI models
 * the simulated device never responding at all (e.g. a mis-wired
 * chip-select), not a literal I2C-style acknowledgement.
 *
 * @param[in] bus_id     The same id the app passes to @ref alp_spi_open.
 * @param[in] f          Fault kind.
 * @param[in] short_len  Bytes actually transferred before the fault,
 *                        for @ref ALP_TESTING_FAULT_SHORT. Ignored for
 *                        the other fault kinds.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p f is not a valid
 *         @ref alp_testing_bus_fault_t.
 */
alp_status_t
alp_testing_spi_fail_next(uint32_t bus_id, alp_testing_bus_fault_t f, size_t short_len);

/**
 * @brief Read back the MOSI bytes the app under test last drove via
 *        @ref alp_spi_transceive / @ref alp_spi_write on @p bus_id.
 *
 * Reports the single most recent transceive that supplied a non-NULL
 * `tx` (not a drained queue) -- a second call without an intervening
 * write-carrying transceive returns the same bytes again.
 *
 * @param[in]  bus_id  The same id the app passes to @ref alp_spi_open.
 * @param[out] out     Destination buffer.  Must be non-NULL if
 *                       @p cap > 0.
 * @param[in]  cap     Capacity of @p out, in bytes.
 * @param[out] got     Receives the number of bytes actually copied
 *                       (`min(cap, bytes captured by the last MOSI)`).
 *                       Must be non-NULL.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p got is NULL, @p out is
 *         NULL with @p cap > 0, or @p bus_id has never been touched
 *         (no open-and-transceive-with-tx, no injection).
 */
alp_status_t alp_testing_spi_last_mosi(uint32_t bus_id, uint8_t *out, size_t cap, size_t *got);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_SPI_H */
