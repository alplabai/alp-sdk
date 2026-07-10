/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file can.h
 * @brief Injection API for the CAN test double (priority-255 backend).
 *
 * `src/backends/can/testing_drv.c` registers a `silicon_ref="*"`
 * backend at priority 255 (see @ref ALP_BACKEND_REGISTER), so with
 * `CONFIG_ALP_SDK_TESTING_CAN=y` it wins @ref alp_backend_select for
 * every bus id and the portable `<alp/can.h>` `alp_can_*` API rides on
 * it transparently -- no real or emulated CAN controller needed. This
 * header is the test-side control surface: it delivers RX frames
 * against the filters the app under test installed via
 * @ref alp_can_add_filter, injects controller bus-state faults, and
 * reads back what the app under test sent via @ref alp_can_send.
 *
 * Every function keys off the same @p bus_id the app passes to
 * @ref alp_can_open, and @ref alp_testing_can_inject_rx,
 * @ref alp_testing_can_inject_rx_at, and
 * @ref alp_testing_can_set_bus_state are create-on-first-touch -- a
 * test may inject a frame or a bus fault BEFORE the app opens the bus,
 * so power-on / cold-bus scenarios are expressible. @ref
 * alp_testing_can_tx_drain and @ref alp_testing_can_get_bus_state are
 * the read-back exception: they report on a bus id that has been
 * touched at least once (by an injector call or by `alp_can_open`),
 * and fail/return zero for one that never has -- there is nothing yet
 * to read back.
 *
 * @par Callback dispatch mirrors the GPIO double.
 *      @ref alp_testing_can_inject_rx walks the filters this bus's
 *      handle registered via @ref alp_can_add_filter (the same
 *      `(frame.id & mask) == (id & mask)` + exact `ext_id` match
 *      @ref alp_can_filter_t documents) and invokes every matching
 *      filter's callback SYNCHRONOUSLY, on the calling thread, before
 *      this function returns -- exactly like @ref alp_testing_gpio_edge
 *      fires an armed GPIO edge callback. @ref alp_can_remove_filter
 *      (or @ref alp_can_close, which removes every filter still
 *      installed on its handle, mirroring the real Zephyr backend's
 *      `z_close()`) detaches a filter's callback wiring immediately,
 *      so an injection that arrives after removal is a documented
 *      no-op for that filter (no use-after-remove / use-after-close
 *      callback).
 *
 * @par Deferred delivery mirrors the GPIO double's edge_at, not UART's
 *      read-timeout.
 *      CAN reception is callback-driven (@ref alp_can_add_filter), not
 *      polled with a caller-supplied timeout the way
 *      @ref alp_uart_read is -- there is no `alp_can_receive()` to
 *      resolve against a deadline. @ref alp_testing_can_inject_rx_at
 *      therefore schedules against the virtual clock
 *      (`<alp/testing/clock.h>`) and fires the callback the moment a
 *      later @ref alp_testing_clock_advance_ms carries "now" to
 *      `>= at_ms`, synchronously, exactly like
 *      @ref alp_testing_gpio_edge_at.
 *
 * @note `alp_can_open()` on this double ALWAYS succeeds (a deliberate
 *       ergonomic choice, mirroring the GPIO/UART doubles -- see
 *       `testing_drv.c`'s open()), so an app under test that opens a
 *       genuinely-invalid bus id still gets back a live handle: this
 *       double cannot catch a wrong-bus application bug. Proving
 *       open() rejects an invalid instance is the real backend's
 *       conformance job (see the invalid-instance case in
 *       `tests/zephyr/conformance/src/main.c`), not this one's. Do
 *       not read "open() succeeded" as "bus_id was valid" when writing
 *       a test against this double.
 */

#ifndef ALP_TESTING_CAN_H
#define ALP_TESTING_CAN_H

#include <stddef.h>
#include <stdint.h>

#include <alp/can.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Controller bus-error state, injectable via
 *        @ref alp_testing_can_set_bus_state.
 *
 * `<alp/can.h>` documents bus-off / error-passive only as prose on
 * @ref alp_can_send's `ALP_ERR_IO` return (the portable dispatcher has
 * no state-query op of its own -- no real backend exposes one yet);
 * this double gives that prose an injectable, readable value so a test
 * can drive and observe it deterministically.
 */
typedef enum {
	/** Normal operation. The default state for a freshly-touched or
	 *  reset bus id (numeric value 0, so the instance table's
	 *  zero-init already produces it without a slot_reset() write). */
	ALP_CAN_STATE_ERROR_ACTIVE = 0,
	/** Elevated error count; the controller still arbitrates and
	 *  transmits, just less assertively (ISO 11898-1). @ref
	 *  alp_can_send still succeeds in this state on this double --
	 *  a real error-passive node can still transmit. */
	ALP_CAN_STATE_ERROR_PASSIVE = 1,
	/** Bus-off: the controller has disconnected from the bus and can
	 *  neither transmit nor receive. @ref alp_can_send returns
	 *  ALP_ERR_IO on this double while a bus id is in this state. */
	ALP_CAN_STATE_BUS_OFF = 2,
} alp_can_state_t;

/**
 * @brief Deliver a frame right now, dispatching to every filter
 *        registered on @p bus_id whose pattern matches it.
 *
 * Matches @p frame against every filter installed via
 * @ref alp_can_add_filter on this bus id's currently-open handle,
 * using the same rule @ref alp_can_filter_t documents (`(frame.id &
 * mask) == (id & mask)`, plus an exact match on `ext_id`), and invokes
 * EVERY matching filter's callback synchronously, on the calling
 * thread, before this function returns -- a frame may fire more than
 * one callback if more than one installed filter matches it, the same
 * way overlapping hardware filter banks would. A filter removed via
 * @ref alp_can_remove_filter (or dropped by @ref alp_can_close, which
 * removes every filter still installed on its handle) cannot be
 * matched by a subsequent call, even if this bus id is never closed
 * (no use-after-remove callback).
 *
 * @param[in] bus_id  The same id the app passes to @ref alp_can_open.
 * @param[in] frame   Frame to deliver. Copied; the caller's buffer
 *                     need not outlive this call.
 *
 * @return ALP_OK on success (including when nothing matches -- a
 *         non-match is not an error); ALP_ERR_INVAL if @p frame is
 *         NULL; ALP_ERR_NOMEM if the instance table is full.
 */
alp_status_t alp_testing_can_inject_rx(uint32_t bus_id, const alp_can_frame_t *frame);

/**
 * @brief Schedule an @ref alp_testing_can_inject_rx delivery for a
 *        future virtual-clock timestamp.
 *
 * The delivery fires the moment a subsequent
 * @ref alp_testing_clock_advance_ms carries the virtual clock's "now"
 * to `>= at_ms` -- synchronously, in the caller-of-advance's thread,
 * exactly like a direct @ref alp_testing_can_inject_rx call at that
 * instant. Deferred, not immediate: this call itself never dispatches
 * any callback.
 *
 * @param[in] bus_id  The same id the app passes to @ref alp_can_open.
 * @param[in] at_ms   Virtual-clock timestamp (@ref alp_testing_clock_now_ms)
 *                     at which the frame is delivered.
 * @param[in] frame   Frame to deliver once due. Copied.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p frame is NULL;
 *         ALP_ERR_NOMEM if the event queue or instance table is full.
 */
alp_status_t
alp_testing_can_inject_rx_at(uint32_t bus_id, uint64_t at_ms, const alp_can_frame_t *frame);

/**
 * @brief Inject the controller bus-error state @ref alp_can_send (and
 *        @ref alp_testing_can_get_bus_state) observe for @p bus_id.
 *
 * @param[in] bus_id  The same id the app passes to @ref alp_can_open.
 * @param[in] state   State to inject. Persists across
 *                     @ref alp_can_close / re-open on the same bus id
 *                     (mirrors the UART double's TX/RX side-state);
 *                     only @ref alp_testing_reset_all clears it back
 *                     to @ref ALP_CAN_STATE_ERROR_ACTIVE.
 *
 * @return ALP_OK on success; ALP_ERR_NOMEM if the instance table is full.
 */
alp_status_t alp_testing_can_set_bus_state(uint32_t bus_id, alp_can_state_t state);

/**
 * @brief Read back the bus-error state last injected with
 *        @ref alp_testing_can_set_bus_state.
 *
 * @param[in]  bus_id     The same id the app passes to @ref alp_can_open.
 * @param[out] state_out  Receives the current state. Must be non-NULL.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p state_out is NULL or
 *         @p bus_id has never been touched (no open, no injection).
 */
alp_status_t alp_testing_can_get_bus_state(uint32_t bus_id, alp_can_state_t *state_out);

/**
 * @brief Read back frames the app under test sent with
 *        @ref alp_can_send, draining them from the capture buffer.
 *
 * Non-blocking; copies up to @p cap frames, oldest first, and removes
 * them from the capture buffer (a second call without an intervening
 * send returns 0 more once everything has been drained). A frame that
 * @ref alp_can_send rejected outright (bus-off; see
 * @ref alp_testing_can_set_bus_state) is never captured here.
 *
 * @param[in]  bus_id  The same id the app passes to @ref alp_can_open.
 * @param[out] out     Destination buffer. Must be non-NULL if @p cap > 0.
 * @param[in]  cap     Capacity of @p out, in frames.
 *
 * @return Number of frames actually copied (0 if @p bus_id has never
 *         been touched, @p out is NULL with @p cap > 0, or nothing has
 *         been captured yet).
 */
size_t alp_testing_can_tx_drain(uint32_t bus_id, alp_can_frame_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_CAN_H */
