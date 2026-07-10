/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART test double.  Ordinary alp_uart_ops_t backend registered at
 * the reserved test-double priority (255, silicon_ref="*"), so with
 * CONFIG_ALP_SDK_TESTING_UART=y it outranks every real/proxy/fallback
 * UART backend and alp_uart_open() rides on it transparently -- see
 * the priority note on ALP_BACKEND_REGISTER in <alp/backend.h>.
 *
 * open()  -- ALWAYS succeeds.  Same deliberate ergonomic choice as the
 *            GPIO double (src/backends/gpio/testing_drv.c): a "*"
 *            silicon_ref double has no valid-port-range knowledge to
 *            reject a port_id with, and the injection API
 *            (<alp/testing/uart.h>) needs inject-before-open to work.
 * write() -- appends to a per-port TX capture ring, read back via
 *            alp_testing_uart_tx_drain().  Bytes beyond the capture
 *            ring's fixed capacity (ALP_TESTING_UART_TX_CAP) are
 *            silently dropped -- alp_uart_write() documents ALP_OK /
 *            ALP_ERR_INVAL / ALP_ERR_NOT_READY / ALP_ERR_IO /
 *            ALP_ERR_NOSUPPORT, not an overflow status, so a test that
 *            writes more than it means to drain must call
 *            alp_testing_uart_tx_drain() before the ring fills.
 * read()  -- THE LOAD-BEARING PIECE (issue #610 PR2).  Never sleeps or
 *            busy-waits: resolves entirely against the virtual clock
 *            (<alp/testing/clock.h>).  See t_read() below for the
 *            exact algorithm; <alp/testing/uart.h>'s file header
 *            documents the contract from the test-author's side.
 * close() -- detaches the handle from its slot (be_data = NULL) so a
 *            stale struct alp_uart cannot reach freed backend state;
 *            the port's queued RX/TX/error state itself is NOT
 *            cleared by close() (mirrors the GPIO double leaving
 *            input/output levels intact across a close/re-open on the
 *            same id) -- only alp_testing_reset_all() clears it.
 *
 * The injection API (<alp/testing/uart.h>) and the ops table share the
 * same per-port slot via the generic instance table
 * (src/testing/instance_table.h), keyed by port_id -- exactly the id
 * the app passes to alp_uart_open() -- so a test can queue RX bytes
 * before the app ever opens the port.
 *
 * @par Cost: ROM ~1.5 KB; RAM = capacity * sizeof(slot) (test-only,
 *      never linked into a production image -- gated by
 *      CONFIG_ALP_SDK_TESTING, itself gated on CONFIG_ZTEST).
 * @par Performance: O(capacity) per call (linear slot scan) plus
 *      O(entries) per read() (bounded FIFO walk); fine for the handful
 *      of ports and chunks a unit test ever touches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/testing/clock.h>
#include <alp/testing/uart.h>

#include "instance_table.h"
#include "reset_registry.h"
#include "uart_ops.h"
#include "virtual_clock.h"

#ifndef ALP_TESTING_UART_MAX_PORTS
#define ALP_TESTING_UART_MAX_PORTS 8
#endif

#ifndef ALP_TESTING_UART_MAX_CHUNK
#define ALP_TESTING_UART_MAX_CHUNK 128
#endif

#ifndef ALP_TESTING_UART_MAX_RX_ENTRIES
#define ALP_TESTING_UART_MAX_RX_ENTRIES 8
#endif

#ifndef ALP_TESTING_UART_TX_CAP
#define ALP_TESTING_UART_TX_CAP 256
#endif

/* One queued RX item: either a chunk of bytes or an in-stream error,
 * both carrying the virtual-clock timestamp at which they become
 * readable.  Immediate injections (alp_testing_uart_rx_feed,
 * alp_testing_uart_rx_inject_error) stamp ready_ts = "now" at the call
 * site; alp_testing_uart_rx_feed_at stamps the caller-supplied at_ms.
 * Consumption is strict FIFO by insertion order (a real wire cannot
 * deliver an earlier-queued byte after a later one), so an entry whose
 * ready_ts is still in the future blocks every entry queued behind
 * it from being consumed early, even if THEY happen to already be
 * ready -- a deliberate simplification documented on
 * <alp/testing/uart.h>. */
typedef struct {
	bool         is_error;
	uint64_t     ready_ts;
	alp_status_t err;                              /* valid when is_error */
	uint8_t      data[ALP_TESTING_UART_MAX_CHUNK]; /* valid when !is_error */
	size_t       len;                              /* valid when !is_error */
	size_t       off;                              /* bytes already consumed, !is_error */
} alp_testing_uart_rx_entry_t;

typedef struct {
	alp_testing_uart_rx_entry_t rx[ALP_TESTING_UART_MAX_RX_ENTRIES];
	size_t                      rx_head;  /* index of the oldest queued entry */
	size_t                      rx_count; /* entries currently queued */

	uint8_t tx[ALP_TESTING_UART_TX_CAP]; /* capture ring for alp_uart_write() */
	size_t  tx_head;
	size_t  tx_count;
} alp_testing_uart_slot_t;

static alp_testing_uart_slot_t      g_slots[ALP_TESTING_UART_MAX_PORTS];
static alp_testing_slot_hdr_t       g_hdrs[ALP_TESTING_UART_MAX_PORTS];
static alp_testing_instance_table_t g_table;
static bool                         g_table_ready;

/* Every field's zero value IS the correct "never touched" state (empty
 * queues, empty capture ring) -- the instance table's memset already
 * produces it, so this has nothing left to set.  Kept (rather than
 * passed as NULL) to mirror the GPIO double's explicit-reset style and
 * to give a future field with a non-zero empty state somewhere to go
 * without a silent gap. */
static void slot_reset(void *slot_v)
{
	(void)slot_v;
}

/* Reset hook (issue #610 PR2, pattern-setting per the GPIO precedent's
 * reset-registry contract): re-clears every bound port's RX/TX/error
 * side-state on alp_testing_reset_all(), in addition to the sweep
 * alp_testing_instance_table_reset_all() already performs on this
 * table.  Explicit and idempotent (a table already zeroed by the sweep
 * re-zeros to the same empty state) -- registered so this double's
 * reset contract is a standing, testable guarantee (see the
 * reset-frees-side-state regression in
 * tests/zephyr/conformance/src/behavior_uart.c) rather than an
 * implicit side effect of "it happens to be the only table this double
 * owns", which a future addition of true outside-the-table state
 * (mirroring the GPIO double's deferred-edge trampoline pool) could
 * otherwise silently stop covering. */
static void uart_reset_hook(void)
{
	alp_testing_instance_table_reset(&g_table);
}

static alp_testing_instance_table_t *table(void)
{
	if (!g_table_ready) {
		alp_testing_instance_table_init(
		    &g_table, g_slots, g_hdrs, sizeof(g_slots[0]), ALP_TESTING_UART_MAX_PORTS, slot_reset);
		alp_testing_register_reset_hook(uart_reset_hook);
		g_table_ready = true;
	}
	return &g_table;
}

static alp_testing_uart_rx_entry_t *rx_peek_front(alp_testing_uart_slot_t *slot)
{
	return (slot->rx_count == 0) ? NULL : &slot->rx[slot->rx_head];
}

static void rx_pop_front(alp_testing_uart_slot_t *slot)
{
	slot->rx_head = (slot->rx_head + 1) % ALP_TESTING_UART_MAX_RX_ENTRIES;
	slot->rx_count--;
}

static alp_status_t rx_push(alp_testing_uart_slot_t *slot,
                            uint64_t                 ready_ts,
                            bool                     is_error,
                            alp_status_t             err,
                            const uint8_t           *d,
                            size_t                   len)
{
	if (!is_error) {
		if (d == NULL && len > 0) return ALP_ERR_INVAL;
		if (len > ALP_TESTING_UART_MAX_CHUNK) return ALP_ERR_NOMEM;
	}
	if (slot->rx_count >= ALP_TESTING_UART_MAX_RX_ENTRIES) return ALP_ERR_NOMEM;

	size_t idx = (slot->rx_head + slot->rx_count) % ALP_TESTING_UART_MAX_RX_ENTRIES;
	alp_testing_uart_rx_entry_t *entry = &slot->rx[idx];

	entry->is_error = is_error;
	entry->ready_ts = ready_ts;
	if (is_error) {
		entry->err = err;
		entry->len = 0;
		entry->off = 0;
	} else {
		if (len > 0) memcpy(entry->data, d, len);
		entry->len = len;
		entry->off = 0;
	}
	slot->rx_count++;
	return ALP_OK;
}

alp_status_t alp_testing_uart_rx_feed(uint32_t port_id, const uint8_t *d, size_t len)
{
	alp_testing_uart_slot_t *slot = alp_testing_instance_table_touch(table(), port_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	return rx_push(slot, alp_testing_clock_now_ms(), false, ALP_OK, d, len);
}

alp_status_t
alp_testing_uart_rx_feed_at(uint32_t port_id, uint64_t at_ms, const uint8_t *d, size_t len)
{
	alp_testing_uart_slot_t *slot = alp_testing_instance_table_touch(table(), port_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	return rx_push(slot, at_ms, false, ALP_OK, d, len);
}

alp_status_t alp_testing_uart_rx_inject_error(uint32_t port_id, alp_status_t err)
{
	alp_testing_uart_slot_t *slot = alp_testing_instance_table_touch(table(), port_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	return rx_push(slot, alp_testing_clock_now_ms(), true, err, NULL, 0);
}

size_t alp_testing_uart_tx_drain(uint32_t port_id, uint8_t *out, size_t cap)
{
	alp_testing_uart_slot_t *slot = alp_testing_instance_table_find(table(), port_id);
	if (slot == NULL) return 0;
	if (out == NULL && cap > 0) return 0;

	size_t n = (cap < slot->tx_count) ? cap : slot->tx_count;
	for (size_t i = 0; i < n; ++i) {
		out[i] = slot->tx[(slot->tx_head + i) % ALP_TESTING_UART_TX_CAP];
	}
	slot->tx_head = (slot->tx_head + n) % ALP_TESTING_UART_TX_CAP;
	slot->tx_count -= n;
	return n;
}

/* ---- alp_uart_ops_t ---- */

static alp_status_t
t_open(const alp_uart_config_t *cfg, alp_uart_backend_state_t *st, alp_capabilities_t *caps_out)
{
	alp_testing_uart_slot_t *slot = alp_testing_instance_table_touch(table(), cfg->port_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	st->dev         = NULL;
	st->port_id     = cfg->port_id;
	st->be_data     = slot;
	st->rx_ringbuf  = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t t_write(alp_uart_backend_state_t *st, const uint8_t *data, size_t len)
{
	alp_testing_uart_slot_t *slot = (alp_testing_uart_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	for (size_t i = 0; i < len; ++i) {
		if (slot->tx_count >= ALP_TESTING_UART_TX_CAP) break; /* documented drop-on-overflow */
		size_t idx    = (slot->tx_head + slot->tx_count) % ALP_TESTING_UART_TX_CAP;
		slot->tx[idx] = data[i];
		slot->tx_count++;
	}
	return ALP_OK;
}

/*
 * THE LOAD-BEARING PIECE (issue #610 PR2): resolves `timeout_ms`
 * entirely against the virtual clock, never sleeping or busy-waiting.
 *
 * Walks the RX FIFO from the head, consuming bytes (or, at the head
 * with nothing collected yet, an error) whose ready_ts is within
 * `[now, now + timeout_ms]`, in strict queue order -- an entry not yet
 * ready by the deadline blocks everything queued behind it, even if a
 * later-queued entry's own ready_ts would otherwise qualify (a real
 * wire cannot deliver byte N+1 before byte N).
 *
 *   - Enough collected (>0 bytes, or an error reached at the head)
 *     within the window: advance the virtual clock to the timestamp
 *     of the last item consumed (never further -- reading data that
 *     was already ready costs no simulated time) and return ALP_OK
 *     (or the injected error's status).
 *   - Nothing collected within the window: advance the virtual clock
 *     by the FULL timeout_ms (exactly as a real port would have
 *     waited out the deadline) and return ALP_ERR_TIMEOUT.
 */
static alp_status_t
t_read(alp_uart_backend_state_t *st, uint8_t *data, size_t len, uint32_t timeout_ms)
{
	alp_testing_uart_slot_t *slot = (alp_testing_uart_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	uint64_t now        = alp_testing_clock_now_ms();
	uint64_t deadline   = now + (uint64_t)timeout_ms;
	uint64_t advance_to = now;
	size_t   collected  = 0;

	for (;;) {
		if (collected >= len) break;
		alp_testing_uart_rx_entry_t *head = rx_peek_front(slot);
		if (head == NULL || head->ready_ts > deadline) break;

		if (head->is_error) {
			if (collected > 0) break; /* surface only when nothing collected yet this call */
			alp_status_t err = head->err;
			if (head->ready_ts > advance_to) advance_to = head->ready_ts;
			rx_pop_front(slot);
			(void)alp_testing_clock_advance_ms(advance_to - now);
			return err;
		}

		size_t avail = head->len - head->off;
		size_t take  = (len - collected < avail) ? (len - collected) : avail;
		if (take > 0) {
			memcpy(data + collected, head->data + head->off, take);
			head->off += take;
			collected += take;
		}
		if (head->ready_ts > advance_to) advance_to = head->ready_ts;
		if (head->off == head->len) rx_pop_front(slot);
	}

	if (collected > 0) {
		(void)alp_testing_clock_advance_ms(advance_to - now);
		return ALP_OK;
	}

	(void)alp_testing_clock_advance_ms(timeout_ms);
	return ALP_ERR_TIMEOUT;
}

static void t_close(alp_uart_backend_state_t *st)
{
	st->be_data = NULL;
}

static const alp_uart_ops_t _ops = {
	.open  = t_open,
	.write = t_write,
	.read  = t_read,
	.close = t_close,
};

ALP_BACKEND_REGISTER(uart,
                     alp_testing,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_testing",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
