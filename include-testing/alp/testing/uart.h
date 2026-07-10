/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file uart.h
 * @brief Injection API for the UART test double (priority-255 backend).
 *
 * `src/backends/uart/testing_drv.c` registers a `silicon_ref="*"`
 * backend at priority 255 (see @ref ALP_BACKEND_REGISTER), so with
 * `CONFIG_ALP_SDK_TESTING_UART=y` it wins @ref alp_backend_select for
 * every port id and the portable `<alp/peripheral.h>` `alp_uart_*` API
 * rides on it transparently -- no real or emulated UART controller
 * needed.  This header is the test-side control surface: it queues RX
 * bytes/errors a port reads back via @ref alp_uart_read, and reads
 * back what the app under test wrote via @ref alp_uart_write.
 *
 * Every function keys off the same @p port_id the app passes to
 * @ref alp_uart_open, and the injectors (@ref alp_testing_uart_rx_feed,
 * @ref alp_testing_uart_rx_feed_at, @ref alp_testing_uart_rx_inject_error)
 * are create-on-first-touch -- a test may queue RX data BEFORE the app
 * opens the port, so power-on / boot-banner scenarios are expressible.
 * @ref alp_testing_uart_tx_drain is the exception: it reads back a
 * capture buffer that only exists once the port has been touched (by
 * an injector call or by `alp_uart_open`); it reports zero for a
 * port id nothing has touched yet.
 *
 * @par The load-bearing piece -- timeout semantics:
 *      This is the FIRST `alp/testing` double whose read path resolves
 *      a caller-supplied timeout against the virtual clock instead of
 *      the wall clock. @ref alp_uart_read NEVER sleeps or busy-waits
 *      (a busy-wait would hang native_sim, whose POSIX time only
 *      advances when the CPU goes idle). Instead, on every call it
 *      computes, from the RX bytes/errors already queued plus any
 *      @ref alp_testing_uart_rx_feed_at entries due at or before
 *      `now + timeout_ms`, how much is deliverable inside that window.
 *      @p timeout_ms bounds the WHOLE call, not the gap between two
 *      already-arrived bytes (the same contract @ref alp_uart_read
 *      documents in `<alp/peripheral.h>`), so the clock advancement
 *      depends on whether the request was satisfied IN FULL:
 *        - Full fill (`len` bytes collected): the request was
 *          satisfied outright, so the virtual clock advances (via
 *          @ref alp_testing_clock_advance_ms) only to the timestamp
 *          of the last byte consumed -- reading data that was already
 *          ready costs no simulated time -- and returns @ref ALP_OK.
 *        - Partial fill (more than zero but fewer than `len` bytes
 *          collected -- the queue ran dry or the next entry falls
 *          beyond the deadline): a real port keeps listening until
 *          the deadline instead of returning the moment its queue
 *          empties, so the clock advances by the FULL @p timeout_ms
 *          and returns @ref ALP_OK with the partial bytes in @p data.
 *        - Nothing collected: the clock likewise advances by the FULL
 *          @p timeout_ms and returns @ref ALP_ERR_TIMEOUT -- exactly
 *          as if a real port had waited out the deadline.
 *      An in-stream error reached at the head of the queue with
 *      nothing collected yet on that call (@ref
 *      alp_testing_uart_rx_inject_error) is a distinct event, not a
 *      short read waiting out the deadline: it returns immediately
 *      with the injected status and advances the clock only to that
 *      entry's own ready_ts.
 *
 * @note `alp_uart_open()` on this double ALWAYS succeeds (a deliberate
 *       ergonomic choice, mirroring the GPIO double's `open()` --
 *       see `testing_drv.c`'s open()), so an app under test that opens
 *       a genuinely-invalid port id still gets back a live handle:
 *       this double cannot catch a wrong-port application bug. Proving
 *       open() rejects an invalid instance is the real backend's
 *       conformance job (see the invalid-instance case in
 *       `tests/zephyr/conformance/src/main.c`), not this one's. Do
 *       not read "open() succeeded" as "port_id was valid" when
 *       writing a test against this double.
 */

#ifndef ALP_TESTING_UART_H
#define ALP_TESTING_UART_H

#include <stddef.h>
#include <stdint.h>

#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Queue RX bytes an open (or not-yet-open) port reads back
 *        from @ref alp_uart_read, available immediately.
 *
 * Appended to the port's RX queue behind anything already queued
 * (FIFO) -- a real wire delivers bytes in the order they were sent, so
 * this double preserves insertion order across @ref
 * alp_testing_uart_rx_feed, @ref alp_testing_uart_rx_feed_at, and
 * @ref alp_testing_uart_rx_inject_error calls on the same @p port_id.
 * Ready the moment it is queued -- `now` at the time of this call, so
 * a subsequent @ref alp_uart_read observes it without needing to
 * advance the virtual clock.
 *
 * @param[in] port_id  The same id the app passes to @ref alp_uart_open.
 * @param[in] d        Source bytes; copied, so the caller's buffer
 *                      need not outlive this call.
 * @param[in] len       Byte count.  Must fit the double's fixed
 *                      per-chunk capacity (128 bytes; see
 *                      testing_drv.c) -- feed longer streams as
 *                      multiple calls.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p d is NULL with
 *         @p len > 0; ALP_ERR_NOMEM if @p len exceeds the per-chunk
 *         capacity or the port's RX queue is full.
 */
alp_status_t alp_testing_uart_rx_feed(uint32_t port_id, const uint8_t *d, size_t len);

/**
 * @brief Queue RX bytes that only become readable once the virtual
 *        clock reaches @p at_ms.
 *
 * Same FIFO ordering and per-chunk capacity as @ref
 * alp_testing_uart_rx_feed, but the queued chunk is not deliverable to
 * @ref alp_uart_read until the virtual clock's "now" (@ref
 * alp_testing_clock_now_ms) is `>= at_ms`. A read whose deadline
 * (`now + timeout_ms`) reaches or passes @p at_ms can consume it --
 * see this file's header for whether that advances the clock to
 * exactly @p at_ms (a full fill) or to the full deadline (a partial
 * fill); a read whose deadline falls short leaves it queued for a
 * later call.
 *
 * @param[in] port_id  The same id the app passes to @ref alp_uart_open.
 * @param[in] at_ms    Virtual-clock timestamp (@ref alp_testing_clock_now_ms)
 *                      at or after which the chunk becomes readable.
 * @param[in] d        Source bytes; copied.
 * @param[in] len       Byte count.  Must fit the double's fixed
 *                      per-chunk capacity (128 bytes; see
 *                      testing_drv.c).
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p d is NULL with
 *         @p len > 0; ALP_ERR_NOMEM if @p len exceeds the per-chunk
 *         capacity or the port's RX queue is full.
 */
alp_status_t
alp_testing_uart_rx_feed_at(uint32_t port_id, uint64_t at_ms, const uint8_t *d, size_t len);

/**
 * @brief Queue an in-stream RX error at its position in the FIFO.
 *
 * Models a framing/parity/overrun fault the controller would surface
 * mid-stream.  Queued immediately (ready `now`, like @ref
 * alp_testing_uart_rx_feed -- there is no deferred `_at` variant).  A
 * subsequent @ref alp_uart_read that reaches this entry AT THE HEAD of
 * the still-unconsumed queue (i.e. with nothing collected yet on that
 * call) returns @p err directly instead of ALP_OK/ALP_ERR_TIMEOUT, and
 * dequeues it.  A read that has already collected bytes from earlier
 * entries in the same call stops short of the error and returns
 * ALP_OK with the partial data -- the error stays queued for the NEXT
 * @ref alp_uart_read to surface.
 *
 * @param[in] port_id  The same id the app passes to @ref alp_uart_open.
 * @param[in] err      Status @ref alp_uart_read returns once this
 *                      entry reaches the head of the queue.  Any
 *                      @ref alp_status_t value; @ref ALP_ERR_IO is the
 *                      typical framing/line-fault choice.
 *
 * @return ALP_OK on success; ALP_ERR_NOMEM if the port's RX queue is full.
 */
alp_status_t alp_testing_uart_rx_inject_error(uint32_t port_id, alp_status_t err);

/**
 * @brief Read back bytes the app under test wrote with @ref alp_uart_write,
 *        draining them from the capture buffer.
 *
 * Non-blocking; copies up to @p cap bytes, oldest first, and removes
 * them from the capture buffer (a second call without an intervening
 * write returns 0 more once everything has been drained).
 *
 * @param[in]  port_id  The same id the app passes to @ref alp_uart_open.
 * @param[out] out      Destination buffer.  Must be non-NULL if
 *                       @p cap > 0.
 * @param[in]  cap      Capacity of @p out, in bytes.
 *
 * @return Number of bytes actually copied (0 if @p port_id has never
 *         been touched, @p out is NULL with @p cap > 0, or nothing has
 *         been captured yet).
 */
size_t alp_testing_uart_tx_drain(uint32_t port_id, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_UART_H */
